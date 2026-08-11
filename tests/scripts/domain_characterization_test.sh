#!/bin/bash
# P2-00 domain characterization: start a real ChatServer (v1 on 6000, v2 hardcoded
# on 7000) and run the full use-case matrix over both protocols, asserting v1/v2
# agree (see docs/specs/domain-behavior-matrix.md).
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
# TSan needs ASLR disabled (setarch); harmless for Debug/ASan builds.
# libcrypto allocator bookkeeping is a known single-thread TSan false positive.
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6000" 2>/dev/null || true
sleep 0.5

setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"

for i in $(seq 1 60); do
    if grep -q 'Server started' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -q 'Server started' "$LOG" 2>/dev/null; then
    echo "FAIL: server did not start"
    cat "$LOG"
    exit 1
fi

python3 "$(dirname "$0")/domain_characterization.py" 127.0.0.1 6000 7000
RC=$?
exit $RC
