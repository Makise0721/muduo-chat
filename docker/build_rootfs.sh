#!/bin/bash
# P4-07 M4 3-Gateway Compose: 自制最小 rootfs 构建脚本（环境无可用 base 镜像、Docker Hub 拉取受限）。
# 从 WSL2 宿主 Ubuntu 24.04 拷贝 ChatServer/dbmigrate + libmymuduo + 全部 ldd 动态依赖，
# docker import 为本地镜像 chat-m4-rootfs:latest（与构建环境同 glibc ABI，可运行 ELF）。
# 用法: bash /mnt/d/agent_learning/muduo-chat/docker/build_rootfs.sh
set -euo pipefail

SRC_RELEASE=/mnt/d/muduo-chat-build/p4-06-final-release-20260817
R=/tmp/m4rootfs
rm -rf "$R"
mkdir -p "$R/bin" "$R/lib" "$R/lib64" "$R/etc"

cp "$SRC_RELEASE/bin/ChatServer" "$R/bin/"
cp "$SRC_RELEASE/bin/dbmigrate"  "$R/bin/"
cp "$SRC_RELEASE/lib/libmymuduo.so" "$R/lib/"

# 拷贝全部动态依赖（保留原绝对路径，保证 loader 解析）。
collect() {
    ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | grep '^/'
}
for bin in "$R/bin/ChatServer" "$R/bin/dbmigrate" "$R/lib/libmymuduo.so"; do
    while IFS= read -r lib; do
        [ -n "$lib" ] || continue
        dest="$R$lib"
        mkdir -p "$(dirname "$dest")"
        if [ ! -e "$dest" ]; then
            cp -L "$lib" "$dest"
        fi
    done < <(collect "$bin")
done

# 动态链接器
if [ ! -e "$R/lib64/ld-linux-x86-64.so.2" ]; then
    cp -L /lib64/ld-linux-x86-64.so.2 "$R/lib64/" 2>/dev/null || true
fi

# 最小 /etc（resolv.conf 由容器运行时挂载/生成，这里给空占位；locale 用 C）
printf '' > "$R/etc/resolv.conf"
mkdir -p "$R/usr/share/zoneinfo"
cp -rL /usr/share/zoneinfo/UTC "$R/usr/share/zoneinfo/" 2>/dev/null || true
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$R/etc/passwd"
printf 'root:x:0:\n' > "$R/etc/group"

# nss 相关（mysqlclient 可能链接 nss 用于解析；拷贝全套减少运行期缺失）
for lib in /lib/x86_64-linux-gnu/libnss_* /lib/x86_64-linux-gnu/libresolv* /lib/x86_64-linux-gnu/libnsl* /lib/x86_64-linux-gnu/libc.so.6; do
    [ -e "$lib" ] || continue
    dest="$R$lib"
    mkdir -p "$(dirname "$dest")"
    cp -L "$lib" "$dest" 2>/dev/null || true
done

echo "rootfs staged at $R"
