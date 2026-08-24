#!/bin/bash
# P5-01 tarball-equivalent smoke orchestrator (WSL bash). Owns schema migration,
# env, log tee and cleanup.  Any failure exits 1.
# Usage: bash tests/scripts/p5_01_smoke.sh <SERVER_BIN> [DB_NAME]
#   PROM_BIN / GRAFANA_BIN env override tool binaries (default tarball paths).
set -u

SERVER_BIN="$1"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN_DIR=$(dirname "$SERVER_BIN")
PROM_BIN="${PROM_BIN:-/mnt/d/muduo-chat-build/tools/prometheus-3.14.0.linux-amd64/prometheus}"
GRAFANA_BIN="${GRAFANA_BIN:-/mnt/d/muduo-chat-build/tools/grafana-13.2.0/bin/grafana}"
DB_NAME="${2:-chat_p501smoke_$$}"
BASE=$((37000 + ($$ % 100)))
V1_PORT=$BASE
V2_PORT=$((BASE + 1000))
WORK=$(mktemp -d)
OUT="$WORK/smoke.out"

cleanup() {
    if [ -n "${P501_KEEP_LOG:-}" ]; then
        mkdir -p "$P501_KEEP_LOG"
        cp "$WORK"/*.log "$P501_KEEP_LOG/" 2>/dev/null || true
        cp "$OUT" "$P501_KEEP_LOG/smoke.out" 2>/dev/null || true
    fi
    pkill -f "ChatServer 127.0.0.1 $V1_PORT" 2>/dev/null || true
    pkill -f "prometheus --config.file=$ROOT/docker/prometheus/prometheus.yml" 2>/dev/null || true
    pkill -f "grafana server" 2>/dev/null || true
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
if [ ! -x "$PROM_BIN" ]; then
    echo "FAIL: prometheus binary not found: $PROM_BIN"
    exit 1
fi
if [ ! -x "$GRAFANA_BIN" ]; then
    echo "FAIL: grafana binary not found: $GRAFANA_BIN"
    exit 1
fi

python3 "$(dirname "$0")/p5_01_smoke.py" \
    "$SERVER_BIN" "$PROM_BIN" "$GRAFANA_BIN" "$V1_PORT" "$V2_PORT" \
    "$DB_NAME" "$DB_PASSWORD" "$WORK" 2>&1 | tee "$OUT"
RC=${PIPESTATUS[0]}
exit $RC
