#!/bin/bash
# P5-01 promtool check + test rules 断言脚本（WSL bash）。
# 用法：bash tests/scripts/p5_01_promtool_checks.sh [PROMTOOL_BIN]
# exit 0 iff `promtool check rules` 全绿 && `promtool test rules` 全绿。
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PROMTOOL="${1:-/mnt/d/muduo-chat-build/tools/prometheus-3.14.0.linux-amd64/promtool}"
LOG="${P5_01_CHECK_LOG:-/mnt/d/muduo-chat-build/p5-01-check.log}"
TEST_LOG="${P5_01_TEST_LOG:-/mnt/d/muduo-chat-build/p5-01-test.log}"

cd "$ROOT"

echo "=== promtool check rules ===" | tee "$LOG"
"$PROMTOOL" check rules \
    docker/prometheus/rules/delivery.yml \
    docker/prometheus/rules/capacity.yml \
    docker/prometheus/rules/loop.yml 2>&1 | tee -a "$LOG"
CHECK_RC=${PIPESTATUS[0]}
echo "check_rules_rc=$CHECK_RC" | tee -a "$LOG"

echo "=== promtool test rules ===" | tee "$TEST_LOG"
"$PROMTOOL" test rules tests/scripts/p5_01_test_rules.yml 2>&1 | tee -a "$TEST_LOG"
TEST_RC=${PIPESTATUS[0]}
echo "test_rules_rc=$TEST_RC" | tee -a "$TEST_LOG"

if [ "$CHECK_RC" -ne 0 ] || [ "$TEST_RC" -ne 0 ]; then
    echo "P5_01_PROMTOOL_FAIL (check=$CHECK_RC test=$TEST_RC)" | tee -a "$TEST_LOG"
    exit 1
fi
echo "P5_01_PROMTOOL_PASS (check=$CHECK_RC test=$TEST_RC)" | tee -a "$TEST_LOG"
exit 0
