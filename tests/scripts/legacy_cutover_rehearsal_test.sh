#!/bin/bash
# P3-10 升级 + 应用回滚演练 process test（docs/tasks/P3-10.md RED 计划 8 / 验证节）：
# 独立 schema chat_p310_rehearsal_$$。本 .sh 拥有 schema 创建/迁移/checksum 核对、
# 端口与 config 文件、日志 tee 与清理；legacy_cutover_rehearsal_test.py 拥有服务器
# 进程与协议/数据断言（spawn `setarch x86_64 -R`、精确 pid kill，无宽 pkill）。
#
# 步骤：migrate 0001+0002（旧五表 + 新六表）→ schema checksum 证据（真实库
# schema_migrations 的 version/checksum 与 0001/0002 文件 SHA-256 核对）→ python 驱动
# （种旧 OfflineMessage 快照 → backfill CLI --dry-run/--run → 断言 → 新版 ChatServer
# 登录收迁移离线消息 → 应用回滚演练：旧 P2/P3-07 Release ChatServer 二进制对同一库
# 登录，验证旧路径读删不破坏新 ledger；旧二进制不可得则跳过该子项并记录）。
# 任何失败 exit 1，不 skip；数据库不可用 fail-fast。
set -u

SERVER_BIN="$1"
BACKFILL_BIN="$2"
OLD_SERVER_BIN="${3:-/mnt/d/muduo-chat-build/p3-07-final-release-20260813/bin/ChatServer}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
# $$ 隔离端口对：v1 与 v2（+1000）不重叠、不碰其它 process 测试区间。
BASE=$((26000 + ($$ % 200)))
V1=$BASE
V2=$((BASE + 1000))
DB_NAME="chat_p310_rehearsal_$$"
WORK=$(mktemp -d)
OUT="$WORK/harness.out"

cleanup() {
    if [ -n "${P310_KEEP_LOG:-}" ]; then
        mkdir -p "$P310_KEEP_LOG"
        cp "$WORK"/*.log "$OUT" "$P310_KEEP_LOG/" 2>/dev/null || true
    fi
    # 只清本测试端口上的残留服务器（harness 异常退出时的兜底），不宽 pkill。
    pkill -f "ChatServer 127.0.0.1 $V1" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

export DB_PASSWORD="${DB_PASSWORD:-123456}"
export TSAN_OPTIONS="suppressions=$(dirname "$0")/tsan.supp"

# 全部步骤输出追加进 harness.out（checksum 证据与断言清单同一日志）。
note() { echo "$@" | tee -a "$OUT"; }

if ! mysql -uroot -p"$DB_PASSWORD" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME DEFAULT CHARSET utf8" >/dev/null 2>&1; then
    note "FAIL: cannot create $DB_NAME (need mysql client + root access)"
    exit 1
fi
if ! "$BIN_DIR/dbmigrate" --db "$DB_NAME" --password "$DB_PASSWORD" --migrations-dir "$ROOT/sql/migrations"; then
    note "FAIL: dbmigrate $DB_NAME"
    exit 1
fi

# schema checksum 证据：真实库 schema_migrations（version/checksum）与 0001/0002
# 文件 SHA-256 核对，任一不符即失败。
CHK_FAIL=0
for V in 0001 0002; do
    FILE_SHA=$(sha256sum "$ROOT"/sql/migrations/${V}_*.sql | awk '{print $1}')
    DB_SHA=$(mysql -uroot -p"$DB_PASSWORD" -N -B "$DB_NAME" \
        -e "SELECT checksum FROM schema_migrations WHERE version='$V'")
    note "schema_checksum $V file=$FILE_SHA db=$DB_SHA"
    if [ -z "$FILE_SHA" ] || [ -z "$DB_SHA" ] || [ "$FILE_SHA" != "$DB_SHA" ]; then
        note "FAIL: schema checksum mismatch for $V"
        CHK_FAIL=1
    fi
done
if [ "$CHK_FAIL" != 0 ]; then
    exit 1
fi

# 同一 config（无 outbox 段，新旧二进制均接受）供新旧服务器使用。
cat >"$WORK/server.json" <<EOF
{"server":{"v1":{"ip":"127.0.0.1","port":$V1,"threads":1},
"v2":{"port":$V2}},
"db":{"host":"127.0.0.1","port":3306,"user":"root",
"password":"$DB_PASSWORD","dbname":"$DB_NAME","pool_size":4},
"executor":{"workers":1,"queue_capacity":32}}
EOF

python3 "$(dirname "$0")/legacy_cutover_rehearsal_test.py" \
    127.0.0.1 "$V1" "$V2" \
    "$SERVER_BIN" "$BACKFILL_BIN" "$DB_NAME" "$DB_PASSWORD" "$OLD_SERVER_BIN" \
    "$WORK/server.json" "$WORK" 2>&1 | tee -a "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
