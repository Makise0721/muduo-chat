#!/bin/bash
# P3-07 delivery/ACK process test: start a real ChatServer (v1 on 6000, v2 on
# 7000) against a dedicated migrated schema (chat_p307) and run the delivery +
# client-ACK matrix (online HOL/ACK release, duplicate ACK, disconnect replay,
# foreign ACK, offline login claim, legacy implicit-ack).
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
LOG=$(mktemp)
WORK=$(mktemp -d)
PIDS=""

cleanup() {
    if [ -n "$PIDS" ]; then
        kill -9 $PIDS 2>/dev/null
    fi
    rm -f "$LOG"
    rm -rf "$WORK"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"
pkill -f "ChatServer 127.0.0.1 6000" 2>/dev/null || true
sleep 0.5

if ! mysql -uroot -p"$DB_PASSWORD" -e "DROP DATABASE IF EXISTS chat_p307; CREATE DATABASE chat_p307 DEFAULT CHARSET utf8" >/dev/null 2>&1; then
    echo "FAIL: cannot create chat_p307 (need mysql client + root access)"
    exit 1
fi
if ! "$BIN_DIR/dbmigrate" --db chat_p307 --password "$DB_PASSWORD" --migrations-dir "$ROOT/sql/migrations"; then
    echo "FAIL: dbmigrate chat_p307"
    exit 1
fi

cat >"$WORK/server.json" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":6000,"threads":2},
"v2":{"port":7000}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"chat_p307","pool_size":4},
"executor":{"workers":1,"queue_capacity":32}}
EOF

setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 --config "$WORK/server.json" >"$LOG" 2>&1 &
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

python3 "$(dirname "$0")/delivery_ack_test.py" 127.0.0.1 6000 7000
RC=$?
exit $RC
