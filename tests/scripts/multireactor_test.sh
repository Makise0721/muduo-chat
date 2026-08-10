#!/bin/bash
# P2-08 multi-reactor process test: start ChatServer with threadNum N (2 and 4)
# and run a concurrent business matrix (register/login/direct chat/logout/
# disconnect/reconnect) over 8 connections. Correctness must hold on 2 and 4
# I/O loops.
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
pkill -f "ChatServer 127.0.0.1 6000" 2>/dev/null || true
sleep 0.5

for LOOPS in 2 4; do
    setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 "$LOOPS" >"$LOG" 2>&1 &
    SRV_PID=$!
    PIDS="$SRV_PID"
    READY=0
    for i in $(seq 1 60); do
        if grep -q 'Server started' "$LOG" 2>/dev/null; then
            READY=1
            break
        fi
        sleep 0.2
    done
    if [ "$READY" -ne 1 ]; then
        echo "FAIL: server did not start (loops=$LOOPS)"
        cat "$LOG"
        exit 1
    fi
    python3 "$(dirname "$0")/multireactor_test.py" 127.0.0.1 6000 "$LOOPS"
    RC=$?
    kill -9 "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    if [ "$RC" -ne 0 ]; then
        exit "$RC"
    fi
done

exit 0
