#!/bin/bash
# 为 HarmonyOS 交叉编译 OpenSSL 静态库（arm64-v8a + x86_64）。
#
# 环境二选一：
#   - WSL (Ubuntu 22.04)：sysroot 经 /mnt/d 直连本机 NDK，产物直接写回工程
#   - Ubuntu 工作站：sysroot 需先拷贝到 ~/ohos-ssl/sysroot-real，产物用 scp 回传
#
# 依赖：clang-14 llvm-14 perl make（sudo apt install -y clang-14 llvm-14）
# 可用环境变量：
#   PXC_SYSROOT  OHOS NDK sysroot 路径（缺省：~/ohos-ssl/sysroot，WSL 下自动建软链）
#   PXC_OUT      产物输出根目录（缺省：本脚本所在目录）
#
# 产物：$PXC_OUT/{arm64-v8a,x86_64}/（include + lib/libcrypto.a + lib/libssl.a）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${PXC_WORK:-$HOME/ohos-ssl}"
VERSION="${PXC_OPENSSL_VERSION:-openssl-3.1.8}"
SRC="$WORK/$VERSION"

command -v clang-14 >/dev/null || { echo "缺少 clang-14：sudo apt install -y clang-14 llvm-14"; exit 1; }
command -v llvm-ar-14 >/dev/null || { echo "缺少 llvm-ar-14：sudo apt install -y clang-14 llvm-14"; exit 1; }

mkdir -p "$WORK"

SYSROOT="${PXC_SYSROOT:-}"
if [ -z "$SYSROOT" ]; then
  echo "请通过 PXC_SYSROOT 指定 HarmonyOS SDK native/sysroot 的实际路径。" >&2
  exit 1
fi
if [ ! -d "$SYSROOT/usr/include" ]; then
  echo "错误：sysroot 无效（缺 usr/include）：$SYSROOT" >&2
  exit 1
fi
OUT_PREFIX="${PXC_OUT:-$SCRIPT_DIR}"

if [ ! -d "$SRC" ]; then
  echo "下载 OpenSSL $VERSION ..."
  curl -L --retry 3 -o "$WORK/$VERSION.tar.gz" \
    "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/$VERSION"
  tar -xzf "$WORK/$VERSION.tar.gz" -C "$WORK"
  # codeload 包根目录形如 openssl-openssl-3.1.8
  mv "$WORK/openssl-$VERSION" "$SRC"
fi

build_one() {
  local abi="$1" triple="$2" target="$3" archinc="$4"
  local bdir="$WORK/build-$abi"
  local prefix="$OUT_PREFIX/$abi"
  echo "==== 构建 $abi ($triple) sysroot=$SYSROOT ===="
  # Reuse existing build/output directories; never delete computed paths.
  mkdir -p "$bdir"
  cd "$bdir"
  # OHOS musl sysroot 的多架构头文件在 usr/include/<archinc>/ 下（无 vendor 段，
  # 如 aarch64-linux-ohos），NDK 包装器会自动加 -isystem，裸 clang 必须显式带上
  CC="clang-14 --target=$triple --sysroot=$SYSROOT -isystem $SYSROOT/usr/include/$archinc" \
  CXX="clang++-14 --target=$triple --sysroot=$SYSROOT -isystem $SYSROOT/usr/include/$archinc" \
  AR=llvm-ar-14 RANLIB=llvm-ranlib-14 \
  perl "$SRC/Configure" "$target" \
    --prefix="$prefix" \
    --openssldir=/system/etc/ssl \
    no-shared no-tests no-legacy
  # 宿主 ld 不认目标架构，只编静态库并跳过 apps 链接（build_libs/install_dev）
  make build_libs -j"$(nproc)" >"$WORK/log-$abi.txt" 2>&1 || { echo "构建失败，见 $WORK/log-$abi.txt"; tail -30 "$WORK/log-$abi.txt"; exit 1; }
  make install_dev >>"$WORK/log-$abi.txt" 2>&1
  # 部分目标装到 lib64，统一成 lib
  if [ -d "$prefix/lib64" ] && [ ! -d "$prefix/lib" ]; then
    mv "$prefix/lib64" "$prefix/lib"
  fi
  ls "$prefix/lib"
  echo "$abi 完成: $prefix"
}

build_one arm64-v8a aarch64-unknown-linux-ohos linux-aarch64 aarch64-linux-ohos
build_one x86_64  x86_64-unknown-linux-ohos  linux-x86_64  x86_64-linux-ohos

echo "全部完成。产物在 $OUT_PREFIX/{arm64-v8a,x86_64}"
