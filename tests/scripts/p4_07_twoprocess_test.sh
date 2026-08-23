#!/bin/bash
# P4-07 M4 两进程跨节点投递 RED/GREEN 验收（H 级缺口：共享 consumer group 使跨节点
# wakeup 空转）。真两个独立 ChatServer 进程（共享 MySQL/Redis/Kafka、独立端口、
# gateway.id=1/2）验证 Bob@gw2 在线收到 Alice@gw1 的 direct 并在 ≤delay_s 内 ACK。
#
# RED：二进制为 p4-06-final-release（共享组 muduo-outbox-consumer 缺省）→ Bob 在线
# 收不到/延迟>10s（依赖 gw 谁抢事件，gw1 抢则空转，需等 gw2 scheduler ≤30s 或重连）。
# GREEN：修复后二进制（Config 默认 groupId 按 Gateway 派生，显式 group_id 优先）→
# 每 Gateway 独立消费组、只 wake 本地 active 用户 → Bob@gw2 ≤delay_s 在线收到。
#
# 用法：p4_07_twoprocess_test.sh <SERVER_BIN> [DB_NAME] [DELAY_S]
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
DB_NAME="${2:-chat_m4_$$}"
DELAY_S="${3:-10}"
BASE=$((16200 + ($$ % 40)))
V1_PORT=$BASE
WORK=$(mktemp -d)
OUT="$WORK/twoprocess.out"

cleanup() {
    # 只清本测试端口上的残留服务器（harness 异常退出兜底），不宽 pkill。
    for p in $(pgrep -f "ChatServer 127.0.0.1 $V1_PORT" 2>/dev/null); do
        kill -9 "$p" 2>/dev/null || true
    done
    # 清理本测试可能写入的 presence 键（claim SET 无 EXPIRE、SIGKILL 无 release）。
    for db in 0 1; do
        for k in $(redis-cli -n "$db" --scan --pattern 'presence:v1:*' 2>/dev/null); do
            redis-cli -n "$db" DEL "$k" >/dev/null 2>&1 || true
        done
    done
    if [ -n "${P407_KEEP_LOG:-}" ]; then
        mkdir -p "$P407_KEEP_LOG"
        cp "$WORK"/gw*.log "$P407_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P407_KEEP_LOG/twoprocess.out" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
# P4-07 RED 轮：P407_EXPLICIT_GROUP 非空 → 显式共享 group_id（模拟修复前）；
# GREEN 轮留空 → 按 gateway.id 派生。
export P407_EXPLICIT_GROUP="${P407_EXPLICIT_GROUP:-}"

if ! mysql -uroot -p"$DB_PASSWORD" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME DEFAULT CHARSET utf8" >/dev/null 2>&1; then
    echo "FAIL: cannot create $DB_NAME"
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
if ! (exec 3<>/dev/tcp/127.0.0.1/9092) 2>/dev/null; then
    echo "FAIL: Kafka not reachable on 9092 (real dep, no skip)"
    exit 1
fi
if ! mysql -uroot -p"$DB_PASSWORD" -N -B -e "SELECT 1" >/dev/null 2>&1; then
    echo "FAIL: MySQL not reachable (real dep, no skip)"
    exit 1
fi

python3 "$(dirname "$0")/p4_07_twoprocess_test.py" \
    "$SERVER_BIN" "$DB_NAME" "$DB_PASSWORD" "$WORK" "$DELAY_S" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
