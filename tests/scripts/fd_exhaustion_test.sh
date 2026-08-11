#!/bin/bash
# Process-level fd-exhaustion test (P1R-06):
# lower RLIMIT_NOFILE, drive the server into EMFILE via many connections,
# verify the server survives and recovers (idle-fd accept restore).
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
# 带端口限定，避免与并行进程测试（RESOURCE_LOCK 下仍保险）互相误杀。
pkill -f "setarch x86_64 -R .*$SERVER_BIN 127.0.0.1 6000" 2>/dev/null || true
sleep 0.5

ulimit -n 128
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 >"$LOG" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"

sleep 1
if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "FAIL: server died at startup"
    cat "$LOG"
    exit 1
fi

python3 "$(dirname "$0")/fd_exhaustion_client.py"
RC=$?
if [ "$RC" -ne 0 ]; then
    cat "$LOG"
    exit 1
fi

if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "FAIL: server died during fd exhaustion"
    cat "$LOG"
    exit 1
fi

echo "FD_EXHAUSTION_ALL_PASS"
exit 0
