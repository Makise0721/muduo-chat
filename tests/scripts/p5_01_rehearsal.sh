#!/bin/bash
# P5-01 real rehearsal orchestrator (WSL bash). Owns env, kafka broker pid
# detection, log tee and cleanup.  Each scenario creates its own fresh schema
# (isolated pending/outbox) via dbmigrate.  Any step failure exits 1.
# Usage: bash tests/scripts/p5_01_rehearsal.sh <SERVER_BIN> [DB_PREFIX] [ROUNDS]
#   PROM_BIN env overrides prometheus binary path (default tarball).
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
PROM_BIN="${PROM_BIN:-/mnt/d/muduo-chat-build/tools/prometheus-3.14.0.linux-amd64/prometheus}"
DB_PREFIX="${2:-chat_p501_$$}"
ROUNDS="${3:-3}"
# 独立端口对（不与既有 process 套件撞带）；metrics 端口固定 9095（卡内约定）。
BASE=$((36000 + ($$ % 100)))
V1_PORT=$BASE
V2_PORT=$((BASE + 1000))
WORK=$(mktemp -d)
OUT="$WORK/rehearsal.out"

cleanup() {
    # kp7 安全网：若 harness 在 broker SIGSTOP 期间异常退出，恢复 broker（CONT）。
    if [ -n "${KAFKA_BROKER_PID:-}" ]; then
        kill -CONT "$KAFKA_BROKER_PID" 2>/dev/null || true
    fi
    if [ -n "${P501_KEEP_LOG:-}" ]; then
        mkdir -p "$P501_KEEP_LOG"
        cp "$WORK"/*.log "$P501_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P501_KEEP_LOG/rehearsal.out" 2>/dev/null || true
    fi
    # 只清本测试端口上的残留服务器与 prometheus，不宽 pkill。
    pkill -f "ChatServer 127.0.0.1 $V1_PORT" 2>/dev/null || true
    pkill -f "prometheus --config.file=$WORK/prometheus" 2>/dev/null || true
    # P4-06 环境复原项：清理 presence 键。
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
if [ ! -x "$PROM_BIN" ]; then
    echo "FAIL: prometheus binary not found: $PROM_BIN"
    exit 1
fi
if [ ! -x "$BIN_DIR/dbmigrate" ]; then
    echo "FAIL: dbmigrate not found in $BIN_DIR"
    exit 1
fi

# kp7 需要真实 Kafka broker PID（kill -STOP/-CONT，不做宽 pkill）。
KAFKA_BROKER_PID=""
for p in $(pgrep -f 'kafka.Kafka' 2>/dev/null); do
    if [ "$(basename "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | awk '{print $1}')" 2>/dev/null)" = "java" ]; then
        KAFKA_BROKER_PID=$p
        break
    fi
done
export KAFKA_BROKER_PID

python3 "$(dirname "$0")/p5_01_rehearsal.py" \
    127.0.0.1 "$V1_PORT" "$V2_PORT" "$SERVER_BIN" "$PROM_BIN" \
    "$BIN_DIR/dbmigrate" "$ROOT/sql/migrations" "$DB_PREFIX" "$DB_PASSWORD" \
    "$WORK" "$ROUNDS" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
