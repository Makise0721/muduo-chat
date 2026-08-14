#!/bin/bash
# P3-08 kill/restart reconnect replay process test: a real ChatServer runs against
# a dedicated migrated schema (chat_p308_$$, independent v1/v2 ports) and the
# harness spawn/kill -9/restarts it.  The harness owns the server process so the
# exact pid is captured for `kill -9` (no broad pkill can hit an unrelated
# process); this .sh owns schema migration, config, env, log tee and cleanup.
# Any step failure exits 1 (no skip).
# P3-11: EXECUTOR_WORKERS env (default 1) injects executor workers into the server config.
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
V1_PORT="${2:-$((17000 + ($$ % 1000)))}"
V2_PORT="${3:-$((V1_PORT + 1000))}"
DB_NAME="${4:-chat_p308_$$}"
WORK=$(mktemp -d)
LOG="$WORK/server.log"
OUT="$WORK/harness.out"
PIDS=""

cleanup() {
    if [ -n "${P308_KEEP_LOG:-}" ]; then
        cp "$LOG" "$P308_KEEP_LOG" 2>/dev/null || true
        cp "$LOG.phase2" "$P308_KEEP_LOG.phase2" 2>/dev/null || true
        cp "$OUT" "$P308_KEEP_LOG.out" 2>/dev/null || true
    fi
    if [ -n "$PIDS" ]; then
        kill -9 $PIDS 2>/dev/null || true
    fi
    # 只清本测试端口上的残留服务器（harness 异常退出时的兜底），不宽 pkill。
    pkill -f "ChatServer 127.0.0.1 $V1_PORT" 2>/dev/null || true
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
"executor":{"workers":${EXECUTOR_WORKERS:-1},"queue_capacity":32}}
EOF

python3 "$(dirname "$0")/reconnect_replay_test.py" \
    127.0.0.1 "$V1_PORT" "$V2_PORT" "$SERVER_BIN" "$WORK/server.json" "$LOG" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
