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
pkill -f "setarch x86_64 -R .*$SERVER_BIN" 2>/dev/null || true
sleep 0.5
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"

# wait for bind
for i in $(seq 1 50); do
    if (echo > /dev/tcp/127.0.0.1/6000) 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "FAIL: server died before signal storm"
    cat "$LOG"
    exit 1
fi

# hold one connection so drain takes the hard-deadline path is NOT required:
# with zero connections drain is fast; storm exercises handler + idempotency.
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
