#!/bin/bash
set -euo pipefail  # 增强错误检测：u-未定义变量报错，o pipefail-管道错误传递

# 定义常量，提高可维护性
BUILD_DIR="$(pwd)/build"
INSTALL_INC_DIR="/usr/include/mymuduo"
INSTALL_LIB_DIR="/usr/lib"
LIB_NAME="libmymuduo.so"

# 1. 创建/清空编译目录（修复原脚本[]空格、pwd反引号的兼容性问题）
if [ ! -d "${BUILD_DIR}" ]; then
  mkdir -p "${BUILD_DIR}"  # -p 兼容多级目录
fi
rm -rf "${BUILD_DIR:?}"/*  # :? 防止BUILD_DIR为空时删除/

# 2. 编译源码（cd + cmake + make）
echo "开始编译源码..."
cd "${BUILD_DIR}" && cmake .. && make -j$(nproc)  # -j 多核编译加速

# 3. 安装头文件（修复ls *.h的潜在问题）
echo "安装头文件到 ${INSTALL_INC_DIR}..."
mkdir -p "${INSTALL_INC_DIR}"
# 改用find避免ls的通配符问题，只找当前目录的.h文件
find "$(pwd)/.." -maxdepth 1 -type f -name "*.h" -exec cp {} "${INSTALL_INC_DIR}/" \;

# 4. 安装动态库并更新缓存
echo "安装动态库到 ${INSTALL_LIB_DIR}..."
LIB_PATH="$(pwd)/../lib/${LIB_NAME}"
if [ ! -f "${LIB_PATH}" ]; then
  echo "错误：未找到动态库 ${LIB_PATH}"
  exit 1
fi
cp "${LIB_PATH}" "${INSTALL_LIB_DIR}/"
ldconfig  # 更新系统库缓存

echo "安装完成！mymuduo库已部署到系统目录"