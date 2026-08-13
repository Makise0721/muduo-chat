#!/bin/bash
# P3-07 backpressure delivery process test: slow consumer (B does not read until
# the Python harness observes an accepted-count barrier and the server's own
# Channel pressure event).  The Python harness then uses one reader owner for
# B's socket and verifies the low-water recovery event and delivery drain.
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
WORK=$(mktemp -d)
V1_PORT="${2:-$((16000 + ($$ % 1000)))}"
V2_PORT="${3:-$((V1_PORT + 1000))}"
DB_NAME="${4:-chat_p307_bp_$$}"
LOG="$WORK/server.log"
PIDS=""

cleanup() {
    if [ -n "${P307_KEEP_LOG:-}" ]; then
        cp "$LOG" "$P307_KEEP_LOG" 2>/dev/null || true
    fi
    if [ -n "$PIDS" ]; then
        kill -9 $PIDS 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"

if ! mysql -uroot -p"$DB_PASSWORD" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME DEFAULT CHARSET utf8" >/dev/null 2>&1; then
    echo "FAIL: cannot create $DB_NAME (need mysql client + root access)"
    exit 1
fi
if ! "$BIN_DIR/dbmigrate" --db "$DB_NAME" --password "$DB_PASSWORD" --migrations-dir "$ROOT/sql/migrations"; then
    echo "FAIL: dbmigrate $DB_NAME"
    exit 1
fi

cat >"$WORK/server.json" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":$V1_PORT,"threads":2},
"v2":{"port":$V2_PORT}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"$DB_NAME","pool_size":4},
"executor":{"workers":1,"queue_capacity":32}}
EOF

setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 "$V1_PORT" --config "$WORK/server.json" >"$LOG" 2>&1 &
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

P307_SERVER_LOG="$LOG" python3 "$(dirname "$0")/backpressure_delivery_test.py" 127.0.0.1 "$V1_PORT" "$V2_PORT"
RC=$?
exit $RC
