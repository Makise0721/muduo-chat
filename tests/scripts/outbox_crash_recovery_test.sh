#!/bin/bash
# P3-09 outbox crash recovery process test against a real MySQL schema
# (chat_p309_$$): real two-relay lease competition (two ChatServer instances,
# same schema, disjoint port pairs) + kill -9/restart recovery + poison
# visibility.  The harness owns every server process (spawn via
# `setarch x86_64 -R`, precise pid kill -9, restart) so no broad pkill can hit
# an unrelated process; this .sh owns schema migration, config files, env, log
# tee and cleanup.  Any step failure exits 1 (never skip).
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
# 独立端口对：S1 (v1, v1+1000)，S2 (v1+2000, v1+3000)，互不重叠；$$ 隔离运行。
BASE=$((23000 + ($$ % 400)))
S1_V1=$BASE
S1_V2=$((BASE + 1000))
S2_V1=$((BASE + 2000))
S2_V2=$((BASE + 3000))
DB_NAME="${2:-chat_p309_$$}"
WORK=$(mktemp -d)
OUT="$WORK/harness.out"

cleanup() {
    if [ -n "${P309_KEEP_LOG:-}" ]; then
        mkdir -p "$P309_KEEP_LOG"
        cp "$WORK"/*.log "$P309_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P309_KEEP_LOG/harness.out" 2>/dev/null || true
    fi
    # 只清本测试端口上的残留服务器（harness 异常退出时的兜底），不宽 pkill。
    pkill -f "ChatServer 127.0.0.1 $S1_V1" 2>/dev/null || true
    pkill -f "ChatServer 127.0.0.1 $S2_V1" 2>/dev/null || true
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

# P3-09 冻结参数走 config 注入（测试小值，绝不为生产默认）：claim_batch=2 让
# 两 relay 高频互斥竞 claim；dormant（scan=300000ms）使 kill 确定发生在 relay
# 消费之前（丢失 wakeup / kill-before-processed 窗口，无 SQL 模拟）。
write_config() { # $1=path $2=v1_port $3=v2_port $4=scan_interval_ms
    cat >"$1" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":$2,"threads":1},
"v2":{"port":$3}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"$DB_NAME","pool_size":4},
"executor":{"workers":1,"queue_capacity":32},
"outbox":{"claim_batch":2,"scan_interval_ms":$4,"claim_lease_ms":30000}}
EOF
}
write_config "$WORK/s1_boot.json" "$S1_V1" "$S1_V2" 300000
write_config "$WORK/s1_act.json" "$S1_V1" "$S1_V2" 200
write_config "$WORK/s2_act.json" "$S2_V1" "$S2_V2" 200

python3 "$(dirname "$0")/outbox_crash_recovery_test.py" \
    127.0.0.1 "$S1_V1" "$S1_V2" "$S2_V1" "$S2_V2" \
    "$SERVER_BIN" "$DB_NAME" "$DB_PASSWORD" \
    "$WORK/s1_boot.json" "$WORK/s1_act.json" "$WORK/s2_act.json" \
    "$WORK" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
