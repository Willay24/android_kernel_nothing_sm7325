#!/bin/bash
# Copyright (c) 2026 ravindu644 <droidcasts@protonmail.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Linux 5.4 arm64 kernel build script.
# Toolchain: Snapdragon LLVM 10.0.9 + ARM GNU 14.2 (this era still needs a real GCC
# cross compiler). Same recipe as build_qGKI.sh -- 5.4 Qualcomm trees are qGKI.
#
# Put this in your kernel root, edit the settings below, then run:
#   chmod +x build_5.4.sh && ./build_5.4.sh

set -euo pipefail

# ---------------------------------------------------------------------------
#  SETTINGS -- the only part you normally need to touch
# ---------------------------------------------------------------------------
DEFCONFIG="vendor/spacewar-stock_defconfig"
EXTRA_CONFIGS=(ksu.config)
KERNEL_IMAGE="Image"
USE_OUT_DIR=1
MENUCONFIG=0
export KBUILD_BUILD_USER="spacewar-ksu"

# Some OEM trees need extra variables -- check README_Kernel.txt or build_kernel.sh:
# export TARGET_SOC=s5e9925 PLATFORM_VERSION=12 ANDROID_MAJOR_VERSION=s
# ---------------------------------------------------------------------------

KERNEL_ROOT="$(dirname "$(readlink -f "$0")")"
CLANG="${HOME}/toolchains/clang-r383902b"
COMPAT="${HOME}/toolchains/.compat"
GCC="${HOME}/toolchains/arm-gnu-14.2"
cd "${KERNEL_ROOT}"

info(){ echo -e "\n[INFO]: $*\n"; }
die(){ echo -e "\n[ERROR]: $*\n" >&2; exit 1; }

# fetch <dir> <url> [strip-components] -- does nothing if <dir> already exists
# fetch <dir> <url> [strip-components] [check-file]
# When <check-file> is given, the toolchain is re-downloaded if that file is
# missing (protects against a cache that only restored a partial tree).
fetch(){
    if [ -n "${4:-}" ]; then
        [ -e "$1/$4" ] && return 0
    else
        [ -d "$1" ] && return 0
    fi
    info "Downloading $(basename "$1")..."
    rm -rf "$1"; mkdir -p "$1"
    local z=z; case "$2" in *.xz) z=J;; esac   # tar can't sniff compression off a pipe
    curl -Lf --progress-bar "$2" | tar -x"$z" -C "$1" --strip-components="${3:-0}" \
        || { rm -rf "$1"; die "Failed to download $2"; }
}

RPM_PKGS=(make gcc gcc-c++ bc bison flex pkgconf git curl tar xz zip unzip cpio rsync kmod
          perl python3 openssl openssl-devel openssl-devel-engine elfutils-libelf-devel dwarves
          ncurses-devel zlib-devel libyaml-devel lz4 zstd dtc ncurses-compat-libs)
DEB_PKGS=(build-essential bc bison flex pkg-config git curl tar xz-utils zip unzip cpio rsync
          kmod perl python3 python-is-python3 libssl-dev libelf-dev pahole libncurses-dev
          zlib1g-dev libyaml-dev lz4 zstd device-tree-compiler libtinfo5)

install_deps(){
    local missing=() available=() p
    # Prefer the native distro manager: some Debian/Ubuntu images also ship
    # 'rpm', which used to make this take the dnf branch and fail.
    if command -v apt-get &>/dev/null; then
        for p in "${DEB_PKGS[@]}"; do
            [ "$(dpkg-query -W -f='${db:Status-Status}' "$p" 2>/dev/null)" = installed ] || missing+=("$p")
        done
        [ "${#missing[@]}" = 0 ] && return 0
        for p in "${missing[@]}"; do apt-cache show "$p" &>/dev/null && available+=("$p"); done
        [ "${#available[@]}" = 0 ] && return 0
        info "Installing: ${available[*]}"
        sudo apt-get update && sudo apt-get install -y "${available[@]}" || die "apt failed"
    elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
        # --whatprovides, not -q: some names are virtual now (zlib-devel -> zlib-ng-compat-devel)
        for p in "${RPM_PKGS[@]}"; do
            rpm -q --whatprovides "$p" &>/dev/null || missing+=("$p")
        done
        [ "${#missing[@]}" = 0 ] && return 0
        info "Installing: ${missing[*]}"
        sudo dnf install -y --skip-unavailable "${missing[@]}" || die "dnf failed"
    else
        info "Unknown package manager -- install the kernel build dependencies yourself."
    fi
}

[ -f Makefile ] && [ -d arch/arm64 ] || die "Run this from the kernel source root."
install_deps
[ -f .gitmodules ] && git submodule update --init --recursive

fetch "${CLANG}" "https://github.com/ravindu644/Android-Kernel-Tutorials/releases/download/toolchains/clang-r383902b.tar.gz" "" "bin/clang"
fetch "${GCC}" "https://github.com/ravindu644/Android-Kernel-Tutorials/releases/download/toolchains/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz" 1 "bin/aarch64-none-linux-gnu-gcc"


# Snapdragon LLVM wants libtinfo.so.5; no current distro ships it. If the compat
# package above didn't provide one, point it at the system's libtinfo.so.6.
# (no grep -q / awk exit below: closing the pipe early makes ldconfig die of
#  SIGPIPE, and pipefail would then kill the script without a word)
if ! ldconfig -p | grep "libtinfo\.so\.5" >/dev/null; then
    tinfo="$(ldconfig -p | awk '/libtinfo\.so\.6/ && !seen++ {print $NF}')"
    [ -n "${tinfo}" ] || die "No libtinfo found -- install ncurses and try again."
    mkdir -p "${COMPAT}" && ln -sf "${tinfo}" "${COMPAT}/libtinfo.so.5"
fi

export PATH="${CLANG}/bin:${GCC}/bin:${PATH}"
export LD_LIBRARY_PATH="${COMPAT}:${CLANG}/lib:${CLANG}/lib64:${LD_LIBRARY_PATH:-}"

CC_BIN=clang
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_COMPRESS=1
    export CCACHE_SLOPPINESS="time_macros,include_file_ctime,include_file_mtime"
    CC_BIN="ccache clang"
fi

BUILD_OPTIONS=(
    -j"$(nproc)"
    ARCH=arm64
    "CC=${CC_BIN}"
    CROSS_COMPILE=aarch64-none-linux-gnu-
    CLANG_TRIPLE=aarch64-linux-gnu-
    KSU_VERSION=35144
    "LOCALVERSION="
    LD=ld.lld
    AR=llvm-ar
    NM=llvm-nm
    OBJCOPY=llvm-objcopy
    OBJDUMP=llvm-objdump
    STRIP=llvm-strip
)

if [ "${USE_OUT_DIR}" = 1 ]; then
    BUILD_OPTIONS+=(O="${KERNEL_ROOT}/out")
    BOOT_DIR="${KERNEL_ROOT}/out/arch/arm64/boot"
else
    BOOT_DIR="${KERNEL_ROOT}/arch/arm64/boot"
fi

build_kernel(){
    info "Kernel $(make kernelversion) | defconfig: ${DEFCONFIG}"

    make "${BUILD_OPTIONS[@]}" "${DEFCONFIG}" "${EXTRA_CONFIGS[@]}" || die "Failed to write .config"

    if [ "${MENUCONFIG}" = 1 ]; then
        make "${BUILD_OPTIONS[@]}" menuconfig
    fi

    make "${BUILD_OPTIONS[@]}" "${KERNEL_IMAGE}" || die "Build failed"

    mkdir -p "${KERNEL_ROOT}/build"
    cp "${BOOT_DIR}/${KERNEL_IMAGE}" "${KERNEL_ROOT}/build/"
    info "Done -> build/${KERNEL_IMAGE}"
}

build_kernel
