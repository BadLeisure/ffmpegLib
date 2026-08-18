#!/bin/bash
set -e
set -x

# 脚本所在目录
SCRIPTPATH="$(cd "$(dirname "$0")" && pwd)"

export ANDROID_NDK_HOME=/Users/ruanjianbu-aa/Library/Android/sdk/ndk/26.1.10909125
OPENSSL_DIR=$SCRIPTPATH/openssl-1.1.1w
export CC=clang

# 配置toolchains
export PATH=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH

# Android API
ANDROID_API=21

# 架构
architecture=android-arm64
ARCH=arm64
CPU=armv8-a

cd ${OPENSSL_DIR}
# 清理旧构建
make clean || true
# Configure
./Configure ${architecture} -D__ANDROID_API__=$ANDROID_API shared no-unit-test no-ssl2 no-ssl3

# 构建
make -j$(nproc)

# 输出目录
OUTPUT_INCLUDE=$SCRIPTPATH/output/include
OUTPUT_LIB=$SCRIPTPATH/output/lib/${architecture}
mkdir -p $OUTPUT_INCLUDE
mkdir -p $OUTPUT_LIB

# 拷贝
cp -RL include/openssl $OUTPUT_INCLUDE
cp libcrypto.so libcrypto.a libssl.so libssl.a $OUTPUT_LIB
