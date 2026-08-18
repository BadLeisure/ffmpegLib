
#!/bin/bash
NDK=/Users/ruanjianbu-aa/Library/Android/sdk/ndk/26.1.10909125
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64
API=21
BASEPATH=$(cd `dirname $0`; pwd)

function build_android
{
echo "Compiling FFmpeg for $CPU"
./configure \
    --prefix=$PREFIX \
	--target-os=android \
    --arch=$ARCH \
    --cpu=$CPU \
    --cc=$CC \
    --cxx=$CXX \
    --strip=$STRIP \
    --sysroot=$SYSROOT \
    --extra-cflags="-Os -fpic $OPTIMIZE_CFLAGS" \
    --extra-ldflags="$ADDI_LDFLAGS" \
    --disable-static \
    --enable-shared \
	--disable-everything \
    --enable-hwaccels \
	--enable-jni \
	--enable-mediacodec \
	--enable-decoder=h264_mediacodec \
	--enable-decoder=hevc_mediacodec \
	--enable-decoder=mpeg4_mediacodec \
    --disable-nonfree \
    --disable-gpl \
	--enable-neon \
	--enable-asm \
    --enable-decoder=hevc \
    --enable-parser=hevc \
    --enable-demuxer=rtsp,mov \
    --enable-protocol=rtsp,tcp,udp \
	--disable-vulkan \
    --enable-cross-compile \
    
    $ADDITIONAL_CONFIGURE_FLAG
make clean
make
make install
echo "The Compilation of FFmpeg for $CPU is completed"
}

#armv8-a
ARCH=arm64
CPU=armv8-a
export AR=$TOOLCHAIN/bin/llvm-ar
export AS=$TOOLCHAIN/bin/llvm-as
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib
export STRIP=$TOOLCHAIN/bin/llvm-strip
CC=$TOOLCHAIN/bin/aarch64-linux-android$API-clang
CXX=$TOOLCHAIN/bin/aarch64-linux-android$API-clang++
SYSROOT=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot
PREFIX=$(pwd)/android/$CPU
OPTIMIZE_CFLAGS="-march=$CPU"


build_android


