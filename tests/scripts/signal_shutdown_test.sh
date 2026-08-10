#!/bin/bash
# Process-level signal shutdown test (P1R-05):
# - SIGINT/SIGTERM handler must never block under a signal storm;
# - repeated signals must merge into a single shutdown flow (DRAINED printed once);
# - process must exit within the timeout.
set -u

SERVER_BIN="$1"
TIMEOUT_SECS="${2:-15}"
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
# TSan needs ASLR disabled (setarch); harmless for Debug/ASan builds.
# libcrypto allocator bookkeeping is a known single-thread TSan false positive.
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6000" 2>/dev/null || true
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

# Scenario 1: held connection forces the 5s hard-deadline path.
LOG1=$(mktemp)
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 >"$LOG1" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"
if ! wait_server_ready "$LOG1"; then
    echo "FAIL: server did not start (scenario 1)"
    cat "$LOG1"
    ss -tlnp 2>/dev/null | grep -E ':(6000|7000)' || echo "NO_LISTENER_ON_6000_7000"
    ps aux | grep ChatServer | grep -v grep || echo "NO_CHATSERVER_PROC"
    exit 1
fi
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 6000), timeout=3)
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
DEADLINE=$((SECONDS + 9))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: server did not exit within 9s on hard-deadline path"
        cat "$LOG1"
        exit 1
    fi
    sleep 0.2
done
wait "$SRV_PID" 2>/dev/null
RC=$?
wait "$HOLDER" 2>/dev/null
if ! grep -q 'DRAIN_TIMEOUT' "$LOG1"; then
    echo "FAIL: DRAIN_TIMEOUT not printed on held-connection shutdown"
    cat "$LOG1"
    exit 1
fi
rm -f "$LOG1"
echo "PASS: hard-deadline path exit=$RC"

# Scenario 2: signal storm with zero connections; fast drain, idempotency.
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 >>"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$PIDS $SRV_PID"
if ! wait_server_ready "$LOG"; then
    echo "FAIL: server did not start (scenario 2)"
    cat "$LOG"
    exit 1
fi
sleep 1

for i in $(seq 1 30); do
    kill -INT "$SRV_PID" 2>/dev/null
    kill -TERM "$SRV_PID" 2>/dev/null
    sleep 0.01
done

DEADLINE=$((SECONDS + TIMEOUT_SECS))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: server did not exit within ${TIMEOUT_SECS}s under signal storm"
        cat "$LOG"
        exit 1
    fi
    sleep 0.2
done

wait "$SRV_PID"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "FAIL: server exit code $RC"
    cat "$LOG"
    exit 1
fi

DRAIN_COUNT=$(grep -c 'DRAINED pending=0' "$LOG" 2>/dev/null || true)
if [ "$DRAIN_COUNT" -ne 1 ]; then
    echo "FAIL: DRAINED printed $DRAIN_COUNT times (expected exactly 1, idempotent shutdown)"
    cat "$LOG"
    exit 1
fi

echo "PASS: signal storm exit=$RC drained_count=$DRAIN_COUNT"
exit 0
