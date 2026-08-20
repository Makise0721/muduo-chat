#!/bin/bash
# P4-06 three-node chaos + capacity/storm-protection process test against a real
# ChatServer + real Redis + real Kafka + real MySQL (dedicated schema chat_p306_$$,
# independent v1/v2 ports, per-scenario presence-db isolation and Kafka topic).
# The harness owns every server process (spawn via `setarch x86_64 -R`, precise
# pid kill -9); this .sh owns schema migration, env, log tee and cleanup.
# Any step failure exits 1 (never skip). P306_KEEP_LOG preserves original logs.
#
# RED：cluster_chaos_test.py 的 l5_lastseen_unbounded 与 db0_presence_accumulation
# 两场景在当前实现（P4-04 L-5 / P4-06 环境复原项）下必须失败 → 合法 RED；其余
# 场景照常执行，失败也不 skip，原始日志/DB/Redis 证据全部保留。
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
# 独立端口对（不与既有 process 套件撞带）：v1 各异、v2=v1+1000；$$ 隔离运行。
BASE=$((30000 + ($$ % 400)))
V1_PORT=$BASE
V2_PORT=$((BASE + 1000))
DB_NAME="${2:-chat_p306_$$}"
WORK=$(mktemp -d)
OUT="$WORK/harness.out"
GLOBAL_LWT_ORIG=""

# 清理（含 P4-06 环境复原项：db0/db1 presence 键测试侧清理）。
cleanup() {
    # kp7 安全网：若 harness 在 broker SIGSTOP 期间异常退出，恢复 broker（CONT）。
    # 只针对检测到的 java broker pid；幂等（未 STOP 时 CONT 无副作用）。
    if [ -n "${KAFKA_BROKER_PID:-}" ]; then
        kill -CONT "$KAFKA_BROKER_PID" 2>/dev/null || true
    fi
    # H3：无论 python/kp 成败，把 GLOBAL lock_wait_timeout 复位为原值。
    if [ -n "$GLOBAL_LWT_ORIG" ]; then
        mysql -uroot -p"$DB_PASSWORD" -e "SET GLOBAL lock_wait_timeout=$GLOBAL_LWT_ORIG" >/dev/null 2>&1 || true
    fi
    if [ -n "${P306_KEEP_LOG:-}" ]; then
        mkdir -p "$P306_KEEP_LOG"
        cp "$WORK"/*.log "$P306_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P306_KEEP_LOG/harness.out" 2>/dev/null || true
    fi
    # 只清本测试端口上的残留服务器（harness 异常退出时的兜底），不宽 pkill。
    pkill -f "ChatServer 127.0.0.1 $V1_PORT" 2>/dev/null || true
    # P4-06 环境复原项：清理本测试可能写入的 db0/db1 presence 键（claim SET 无
    # EXPIRE、SIGKILL 无 release → 物理键累积；逐条 DEL，含 epoch 计数器——
    # 单调计数器无持久语义，可重建，沿 P4-05 清理先例）。
    for db in 0 1; do
        for k in $(redis-cli -n "$db" --scan --pattern 'presence:v1:*' 2>/dev/null); do
            redis-cli -n "$db" DEL "$k" >/dev/null 2>&1 || true
        done
    done
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
if ! redis-cli -n 0 ping >/dev/null 2>&1; then
    echo "FAIL: Redis not reachable (real dep, no skip)"
    exit 1
fi
if ! mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT 1" >/dev/null 2>&1; then
    echo "FAIL: MySQL not reachable (real dep, no skip)"
    exit 1
fi
if ! (exec 3<>/dev/tcp/127.0.0.1/9092) 2>/dev/null; then
    echo "FAIL: Kafka not reachable on 9092 (real dep, no skip)"
    exit 1
fi

GLOBAL_LWT_ORIG=$(mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT @@GLOBAL.lock_wait_timeout" 2>/dev/null)
if [ -z "$GLOBAL_LWT_ORIG" ]; then
    echo "FAIL: cannot read @@GLOBAL.lock_wait_timeout"
    exit 1
fi

# kp7 需要真实 Kafka broker PID（kill -STOP/-CONT 暂停/恢复，不做宽 pkill）。
# pgrep -f 'kafka.Kafka' 精确形态（沿 P4-03 记录的 `java kafka.Kafka` 主进程）：
# 过滤到 java 进程，避免误中启动包装（kafka-server-start.sh/nohup 包装）。
KAFKA_BROKER_PID=""
for p in $(pgrep -f 'kafka.Kafka' 2>/dev/null); do
    if [ "$(basename "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | awk '{print $1}')" 2>/dev/null)" = "java" ]; then
        KAFKA_BROKER_PID=$p
        break
    fi
done
export KAFKA_BROKER_PID

python3 "$(dirname "$0")/cluster_chaos_test.py" \
    127.0.0.1 "$V1_PORT" "$V2_PORT" "$SERVER_BIN" "$DB_NAME" "$DB_PASSWORD" \
    "$WORK" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}

# H3：尾部断言——GLOBAL lock_wait_timeout 已复位为原值。
CUR_LWT=$(mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT @@GLOBAL.lock_wait_timeout" 2>/dev/null)
if [ -z "$CUR_LWT" ] || [ "$CUR_LWT" != "$GLOBAL_LWT_ORIG" ]; then
    echo "FAIL: @@GLOBAL.lock_wait_timeout=$CUR_LWT (expected restored $GLOBAL_LWT_ORIG)"
    exit 1
fi
echo "PASS global_lock_wait_timeout_restored=$GLOBAL_LWT_ORIG"
exit $RC
