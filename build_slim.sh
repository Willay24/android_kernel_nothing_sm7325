#!/bin/bash

PREFIX="$HOME/build_toolchain"
CLANG="llvm-23.1.0-x86_64"

mkdir -p $PREFIX
cd $PREFIX
aria2c -q https://mirrors.edge.kernel.org/pub/tools/llvm/files/$CLANG.tar.xz
tar -xJf $CLANG.tar.xz
cd - > /dev/null

export LD_LIBRARY_PATH="$PREFIX/$CLANG/lib:$PREFIX/$CLANG/lib64:$LD_LIBRARY_PATH"
export PATH="$PREFIX/$CLANG/bin:$PATH"
export CLANG_DIR
export CLANG_LABEL="Slim LLVM 23.1.0"

#rm -rf out
#mkdir out
#rm -rf error.log
#make O=out clean 
#make mrproper

# Build

echo $PATH

# fog.config with bengal_perf

ARCH=arm64 scripts/kconfig/merge_config.sh -O "out" arch/arm64/configs/vendor/bengal-perf_defconfig arch/arm64/configs/vendor/xiaomi/fog.config arch/arm64/configs/vendor/xiaomi/ksu.config 

make -j $(nproc) ARCH=arm64 SUBARCH=arm64 O=out \
	CC="ccache clang"\
	AR="llvm-ar" \
	NM="llvm-nm" \
	LD="ld.lld -S" \
	OBJCOPY="llvm-objcopy" \
	OBJDUMP="llvm-objdump" \
	STRIP="llvm-strip" \
	CLANG_TRIPLE="aarch64-linux-gnu-" \
	CROSS_COMPILE="aarch64-linux-gnu-" \
	CROSS_COMPILE_ARM32="arm-linux-gnueabi-" \
	CROSS_COMPILE_COMPAT="arm-linux-gnueabi-" \
	LLVM=1 \
	LLVM_IAS=1 \
	INSTALL_MOD_STRIP=1 \
	KBUILD_BUILD_USER="$(git rev-parse --short HEAD | cut -c1-7)" \
	KBUILD_BUILD_HOST="$(git symbolic-ref --short HEAD)"	
	
ccache -s

# EOF
