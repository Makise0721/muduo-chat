#!/bin/bash
# P2-09 config fail-fast test:
# - missing file / malformed json / out-of-range value / unknown field
#   -> process exits nonzero, stderr contains 'config error', no listener.
# - valid config -> starts, SIGTERM -> exit 0.
set -u

SERVER_BIN="$1"
WORK=$(mktemp -d)
PIDS=""

cleanup() {
    if [ -n "$PIDS" ]; then
        kill -9 $PIDS 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6002" 2>/dev/null || true
sleep 0.5

expect_config_error() {
    local DESC="$1"
    shift
    local LOGFILE="$1"
    shift
    setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6002 "$@" >"$LOGFILE" 2>&1
    RC=$?
    if [ "$RC" -eq 0 ]; then
        echo "FAIL: $DESC exited 0 (expected config error)"
        cat "$LOGFILE"
        exit 1
    fi
    if ! grep -q 'config error' "$LOGFILE"; then
        echo "FAIL: $DESC stderr lacks 'config error'"
        cat "$LOGFILE"
        exit 1
    fi
    echo "PASS: $DESC (exit=$RC)"
}

# 1. missing file
expect_config_error "missing config file" "$WORK/1.log" --config "$WORK/nope.json"

# 2. malformed json
echo "{oops" >"$WORK/bad.json"
expect_config_error "malformed json" "$WORK/2.log" --config "$WORK/bad.json"

# 3. out-of-range port
echo '{"server":{"v1":{"port":99999}}}' >"$WORK/port.json"
expect_config_error "port out of range" "$WORK/3.log" --config "$WORK/port.json"

# 4. unknown field
echo '{"db":{"pool_siz":5}}' >"$WORK/unknown.json"
expect_config_error "unknown field" "$WORK/4.log" --config "$WORK/unknown.json"

# 5. invalid threads via argv (fail-fast instead of clamping)
expect_config_error "argv threads=0" "$WORK/5.log" 127.0.0.1 6002 0

# 6. valid config starts and shuts down cleanly
cat >"$WORK/ok.json" <<'EOF'
{"server":{"v1":{"ip":"127.0.0.1","port":6002,"threads":2},
"v2":{"port":7002}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"123456","dbname":"chat","pool_size":3},
"executor":{"workers":1,"queue_capacity":32}}
EOF
LOG6="$WORK/6.log"
setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6002 --config "$WORK/ok.json" >"$LOG6" 2>&1 &
SRV_PID=$!
PIDS="$SRV_PID"
READY=""
for i in $(seq 1 60); do
    if grep -q 'Server started' "$LOG6" 2>/dev/null; then
        READY=1
        break
    fi
    sleep 0.2
done
if [ -z "$READY" ]; then
    echo "FAIL: valid config did not start"
    cat "$LOG6"
    exit 1
fi
kill -TERM "$SRV_PID" 2>/dev/null
DEADLINE=$((SECONDS + 10))
while kill -0 "$SRV_PID" 2>/dev/null; do
    if [ "$SECONDS" -gt "$DEADLINE" ]; then
        echo "FAIL: valid config server did not exit"
        cat "$LOG6"
        exit 1
    fi
    sleep 0.2
done
wait "$SRV_PID"
RC=$?
if [ "$RC" -ne 0 ]; then
    echo "FAIL: valid config exit code $RC"
    cat "$LOG6"
    exit 1
fi
echo "PASS: valid config start+shutdown (exit=$RC)"
exit 0
