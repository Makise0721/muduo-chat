#!/bin/bash
# P2-00/P3-06 domain characterization: start a real ChatServer (v1 on 6000, v2
# hardcoded on 7000) against a dedicated migrated schema (chat_p306) and run the
# full use-case matrix over both protocols, asserting v1/v2 agree
# (see docs/specs/domain-behavior-matrix.md).
# P3-06: matrix asserts durable accept semantics (MESSAGE_ACCEPTED msgid=11、
# 稳定错误码 101..107、幂等重试) which require the ledger tables — the harness
# recreates and migrates chat_p306 (dbmigrate) before starting the server.
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
# TSan needs ASLR disabled (setarch); harmless for Debug/ASan builds.
# libcrypto allocator bookkeeping is a known single-thread TSan false positive.
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

python3 "$(dirname "$0")/domain_characterization.py" 127.0.0.1 6000 7000
RC=$?
exit $RC
