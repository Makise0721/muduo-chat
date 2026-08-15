#!/bin/bash
# P3-12 reliable message fault matrix process test against a real ChatServer +
# MySQL (dedicated schema chat_p312_$$, independent v1/v2 ports, three config
# profiles: active / dormant-relay / short-retention).  The harness owns every
# server process (spawn via `setarch x86_64 -R`, precise pid kill) so no broad
# pkill can hit an unrelated process; this .sh owns schema migration, config
# files, env, log tee and cleanup.  Any step failure exits 1 (never skip).
#
# RED：reliable_message_fault_test.py 的指标断言引用 SIGUSR1 METRICS 行的
# reliable_* 字段（P3-12 最小扩展，尚不存在）→ 指标断言失败即合法 RED；
# DB/客户端断言照常执行，失败也不 skip，原始日志/DB 证据全部保留。
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
# 独立端口对（不与既有 process 套件撞带）：v1 各异、v2=v1+1000；$$ 隔离运行。
BASE=$((26000 + ($$ % 400)))
V1_PORT=$BASE
V2_PORT=$((BASE + 1000))
DB_NAME="${2:-chat_p312_$$}"
WORK=$(mktemp -d)
LOG="$WORK/server.log"
OUT="$WORK/harness.out"

cleanup() {
    # H3：无论 python/kp2 成败，把 GLOBAL lock_wait_timeout 复位为原值
    # （不写死 31536000——机器可能已配置非默认值）。
    if [ -n "${GLOBAL_LWT_ORIG:-}" ]; then
        mysql -uroot -p"$DB_PASSWORD" -e "SET GLOBAL lock_wait_timeout=$GLOBAL_LWT_ORIG" >/dev/null 2>&1 || true
    fi
    if [ -n "${P312_KEEP_LOG:-}" ]; then
        mkdir -p "$P312_KEEP_LOG"
        cp "$WORK"/*.log "$P312_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P312_KEEP_LOG/harness.out" 2>/dev/null || true
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

# 三个配置档：active（relay 常开）/ dormant（relay 300s：kill 必发生在 relay
# 消费前）/ expire（短 retention：过期场景确定性）。参数走 config 注入，绝不
# 为生产默认。
write_config() { # $1=path $2=v1_port $3=v2_port $4=scan_interval_ms
    cat >"$1" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":$2,"threads":2},
"v2":{"port":$3}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"$DB_NAME","pool_size":4},
"executor":{"workers":2,"queue_capacity":32},
"outbox":{"claim_batch":8,"scan_interval_ms":$4,"claim_lease_ms":30000}}
EOF
}
write_config "$WORK/active.json" "$V1_PORT" "$V2_PORT" 200
write_config "$WORK/dormant.json" "$V1_PORT" "$V2_PORT" 300000
cat >"$WORK/expire.json" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":$V1_PORT,"threads":2},
"v2":{"port":$V2_PORT}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"$DB_NAME","pool_size":4},
"executor":{"workers":2,"queue_capacity":32},
"reliable":{"ack_timeout_ms":1000,"backoff_base_ms":1000,"backoff_cap_ms":2000,
"jitter_fraction":0,"message_retention_ms":3000,"acked_retention_ms":3600000,
"expired_retention_ms":3600000,"cleanup_batch":100,"cleanup_cycle_ms":3600000,
"retry_batch_limit":50},
"outbox":{"claim_batch":8,"scan_interval_ms":200,"claim_lease_ms":30000}}
EOF

# H3：kp2 会把 GLOBAL lock_wait_timeout 临时改为 2；先读原值，脚本尾部断言复位。
GLOBAL_LWT_ORIG=$(mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT @@GLOBAL.lock_wait_timeout" 2>/dev/null)
if [ -z "$GLOBAL_LWT_ORIG" ]; then
    echo "FAIL: cannot read @@GLOBAL.lock_wait_timeout"
    exit 1
fi

python3 "$(dirname "$0")/reliable_message_fault_test.py" \
    127.0.0.1 "$V1_PORT" "$V2_PORT" "$SERVER_BIN" "$DB_NAME" "$DB_PASSWORD" \
    "$WORK/active.json" "$WORK/dormant.json" "$WORK/expire.json" \
    "$WORK" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}

# H3：尾部断言——GLOBAL lock_wait_timeout 已复位为原值（.sh trap 之外再验一遍）。
CUR_LWT=$(mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT @@GLOBAL.lock_wait_timeout" 2>/dev/null)
if [ -z "$CUR_LWT" ] || [ "$CUR_LWT" != "$GLOBAL_LWT_ORIG" ]; then
    echo "FAIL: @@GLOBAL.lock_wait_timeout=$CUR_LWT (expected restored $GLOBAL_LWT_ORIG)"
    exit 1
fi
echo "PASS global_lock_wait_timeout_restored=$GLOBAL_LWT_ORIG"
exit $RC
