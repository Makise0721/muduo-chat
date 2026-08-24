#!/bin/bash
# P5-01 工具 tarball 下载/校验（幂等；版本 + sha256 内联记录）。
# 只落 /mnt/d/muduo-chat-build/tools/（构建树外，永不进仓库）。见
# docs/tasks/P5-01.md 环境决策节（2026-08-24 实测）。
#
# 用法：bash tests/scripts/p5_01_download_tools.sh
set -u

TOOLS=/mnt/d/muduo-chat-build/tools
mkdir -p "$TOOLS"
cd "$TOOLS"

# prometheus 3.14.0（GitHub Release）
PROM_URL="https://github.com/prometheus/prometheus/releases/download/v3.14.0/prometheus-3.14.0.linux-amd64.tar.gz"
PROM_SHA="f665c6da19eb7ba399c915d30c7d9793c9b417bf8a749b504bc470678631478d"
PROM_TAR="prometheus-3.14.0.linux-amd64.tar.gz"

# grafana 13.2.0（dl.grafana.com；v13 起 GitHub 不再发布 linux-amd64 tarball）
GRAFANA_URL="https://dl.grafana.com/oss/release/grafana-13.2.0.linux-amd64.tar.gz"
GRAFANA_SHA="4669384cdb0bb5b4a3f804927e57490d17f4cc47258cd1698fc124e99ee58265"
GRAFANA_TAR="grafana-13.2.0.linux-amd64.tar.gz"

fail=0

fetch() { # $1=url $2=tar $3=sha $4=name
    if [ -f "$2" ] && [ "$(sha256sum "$2" | awk '{print $1}')" = "$3" ]; then
        echo "PASS $4 cached (sha256 ok)"
        return 0
    fi
    echo "fetch $4 from $1"
    if ! curl -fsSL -o "$2" "$1"; then
        echo "FAIL $4 download"
        fail=1
        return 1
    fi
    got=$(sha256sum "$2" | awk '{print $1}')
    if [ "$got" != "$3" ]; then
        echo "FAIL $4 sha256 mismatch: got $got want $3"
        fail=1
        return 1
    fi
    echo "PASS $4 sha256 verified"
}

fetch "$PROM_URL" "$PROM_TAR" "$PROM_SHA" "prometheus"
fetch "$GRAFANA_URL" "$GRAFANA_TAR" "$GRAFANA_SHA" "grafana"

if [ "$fail" -ne 0 ]; then
    echo "P5_01_DOWNLOAD_FAIL"
    exit 1
fi
echo "P5_01_DOWNLOAD_PASS"
exit 0
