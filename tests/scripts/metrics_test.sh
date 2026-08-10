#!/bin/bash
# P2-10 server metrics test:
# SIGUSR1 prints one METRICS line with pool + executor snapshot fields;
# process then exits cleanly on SIGTERM.
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
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6003" 2>/dev/null || true
sleep 0.5

setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6003 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"
READY=""
for i in $(seq 1 60); do
    if grep -q 'Server started' "$LOG" 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.2
done
if [ -z "$READY" ]; then
    echo "FAIL: server did not start"
    cat "$LOG"
    exit 1
fi

kill -USR1 "$SRV_PID" 2>/dev/null
sleep 1
if ! grep -q 'METRICS pool_total=5 pool_idle=5 pool_active=0 executor_queue=0 executor_drop_full=0 executor_drop_shutdown=0' "$LOG"; then
    echo "FAIL: METRICS line missing or malformed"
    cat "$LOG"
    exit 1
fi

kill -TERM "$SRV_PID" 2>/dev/null
DEADLINE=$((SECONDS + 10))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: server did not exit"
        cat "$LOG"
        exit 1
    fi
    sleep 0.2
done
wait "$SRV_PID"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "FAIL: exit code $RC"
    cat "$LOG"
    exit 1
fi
echo "PASS: metrics snapshot + clean shutdown (exit=$RC)"
exit 0
