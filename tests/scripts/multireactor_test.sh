#!/bin/bash
# P2-08 multi-reactor process test: start ChatServer with threadNum N (2 and 4)
# against a dedicated migrated schema (chat_p306) and run a concurrent business
# matrix (register/login/direct chat/logout/disconnect/reconnect) over 8
# connections. Correctness must hold on 2 and 4 I/O loops.
# P3-06: chat matrix asserts durable accept semantics which require the ledger
# tables — recreate and migrate chat_p306 before starting the server.
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

if ! mysql -uroot -p"$DB_PASSWORD" -e "DROP DATABASE IF EXISTS chat_p306; CREATE DATABASE chat_p306 DEFAULT CHARSET utf8" >/dev/null 2>&1; then
    echo "FAIL: cannot create chat_p306 (need mysql client + root access)"
    exit 1
fi
if ! "$BIN_DIR/dbmigrate" --db chat_p306 --password "$DB_PASSWORD" --migrations-dir "$ROOT/sql/migrations"; then
    echo "FAIL: dbmigrate chat_p306"
    exit 1
fi

cat >"$WORK/server.json" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":6000,"threads":2},
"v2":{"port":7000}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"chat_p306","pool_size":4},
"executor":{"workers":1,"queue_capacity":32}}
EOF

for LOOPS in 2 4; do
    setarch x86_64 -R "$SERVER_BIN" 127.0.0.1 6000 "$LOOPS" --config "$WORK/server.json" >"$LOG" 2>&1 &
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
