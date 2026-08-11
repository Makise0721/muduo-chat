#!/bin/bash
# P2-09 ordered shutdown test:
# - scenario A: no connections -> SIGTERM -> STOP_ACCEPT, DRAINED, EXECUTOR_SHUTDOWN,
#   POOL_SHUTDOWN, QUIT_LOOPS, EXITED in order; DRAINED exactly once; exit 0.
# - scenario B: held connection -> CHAT_SHUTDOWN_TIMEOUT_MS hard deadline ->
#   DRAIN_TIMEOUT then same tail order; exit 0.
set -u

SERVER_BIN="$1"
LOG=$(mktemp)
PIDS=""

cleanup() {
    if [ -n "$PIDS" ]; then
        kill -9 $PIDS 2>/dev/null
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
export CHAT_SHUTDOWN_TIMEOUT_MS=800
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6001" 2>/dev/null || true
sleep 0.5

wait_server_ready() {
    local LOGFILE="$1"
    for i in $(seq 1 60); do
        if grep -q 'Server started' "$LOGFILE" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# 每个标记必须恰好出现一次（idempotent shutdown 保证）且行号严格递增。
assert_order() {
    local LOGFILE="$1"
    shift
    local prev=0
    local m
    for m in "$@"; do
        local count
        count=$(grep -c "$m" "$LOGFILE")
        if [ "$count" -ne 1 ]; then
            echo "FAIL: marker '$m' appears $count times (expected exactly 1)"
            cat "$LOGFILE"
            exit 1
        fi
        local line
        line=$(grep -n "$m" "$LOGFILE" | head -1 | cut -d: -f1)
        if [ "$line" -le "$prev" ]; then
            echo "FAIL: order broken at '$m' (line $line after $prev)"
            cat "$LOGFILE"
            exit 1
        fi
        prev=$line
    done
}

# Scenario A: zero connections, fast drain.
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6001 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"
if ! wait_server_ready "$LOG"; then
    echo "FAIL: server did not start (scenario A)"
    cat "$LOG"
    exit 1
fi
kill -TERM "$SRV_PID" 2>/dev/null
DEADLINE=$((SECONDS + 10))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: server did not exit (scenario A)"
        cat "$LOG"
        exit 1
    fi
    sleep 0.2
done
wait "$SRV_PID"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "FAIL: scenario A exit code $RC"
    cat "$LOG"
    exit 1
fi
assert_order "$LOG" STOP_ACCEPT DRAINED EXECUTOR_SHUTDOWN POOL_SHUTDOWN QUIT_LOOPS EXITED
rm -f "$LOG"
echo "PASS: scenario A (fast drain) exit=$RC"

# Scenario B: held connection forces DRAIN_TIMEOUT, then full tail order.
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6001 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"
if ! wait_server_ready "$LOG"; then
    echo "FAIL: server did not start (scenario B)"
    cat "$LOG"
    exit 1
fi
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 6001), timeout=3)
s.sendall(b'{\"msgid\":1,\"id\":1,\"password\":\"123456\"}\n')
s.settimeout(20)
try:
    while s.recv(4096):
        pass
except socket.timeout:
    pass
" &
HOLDER=$!
sleep 1
kill -TERM "$SRV_PID" 2>/dev/null
DEADLINE=$((SECONDS + 12))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: server did not exit (scenario B)"
        cat "$LOG"
        exit 1
    fi
    sleep 0.2
done
wait "$SRV_PID"
RC=$?
wait "$HOLDER" 2>/dev/null
if [ "$RC" -ne 0 ]; then
    echo "FAIL: scenario B exit code $RC"
    cat "$LOG"
    exit 1
fi
# main.cpp:70 已用 !flow->forced 限定：DRAINED 只在非强制路径打印。场景 B 经
# DRAIN_TIMEOUT 置 forced 后再归零不会打 DRAINED，A/B 标记互斥已落地。
assert_order "$LOG" STOP_ACCEPT DRAIN_TIMEOUT EXECUTOR_SHUTDOWN POOL_SHUTDOWN QUIT_LOOPS EXITED
rm -f "$LOG"
echo "PASS: scenario B (hard deadline) exit=$RC"
exit 0
