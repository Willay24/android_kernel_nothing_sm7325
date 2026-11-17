#!/usr/bin/env bash
# Build the redwood 6.18 kernel, its vendor modules, dtbo.img and a
# flashable AnyKernel3 zip. See BUILDING.md.
#
#   ./build.sh                 everything: tools kernel modules dtbo zip
#   ./build.sh <step> [...]    only these steps, in the order given
#   ./build.sh clean           remove build output, keep toolchain and sources

set -euo pipefail

KERNEL=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=${WORK:-$(dirname "$KERNEL")/redwood-618-build}
TC=${TC:-$WORK/toolchain}
JOBS=${JOBS:-$(nproc)}
BUILD_NUMBER=${BUILD_NUMBER:-1}
BUILD_USER=${BUILD_USER:-build-user}
BUILD_HOST=${BUILD_HOST:-build-host}
MODULES_REPO=${MODULES_REPO:-https://github.com/suvojeet-sengupta/redwood-6.18-modules}
MODULES_REF=${MODULES_REF:-main}
DT_REPO=${DT_REPO:-https://github.com/suvojeet-sengupta/redwood-6.18-devicetrees}
DT_REF=${DT_REF:-main}

CLANG_VER=clang-r596125
CLANG_REPO=https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86
CLANG_SHA=dc706f7fe714bc3cd89f7e42c025a161581ad6be
BUILDTOOLS_REPO=https://android.googlesource.com/kernel/prebuilts/build-tools
BUILDTOOLS_SHA=b7de4bc0a9de92dcaed4382c8a3180bb4e2485a4
LIBUFDT_REPO=https://android.googlesource.com/platform/system/libufdt
LIBUFDT_SHA=9b70f594cf149c7c73989c78f03d210f8aa1c1f6
AK3_REPO=https://github.com/osm0sis/AnyKernel3
AK3_SHA=41f04bdb4ee30a44e429c2c1131d5e8bb32b15f1
RUST_VER=1.88.0
BINDGEN_VER=0.72.1

OUT=$WORK/out
DIST=$WORK/dist
LOGS=$WORK/logs
MODSRC=$WORK/redwood-6.18-modules
DTSRC=$WORK/redwood-6.18-devicetrees
CLANG_DIR=${CLANG_DIR:-$TC/clang/$CLANG_VER}
PAHOLE=${PAHOLE:-$TC/pahole}
export RUSTUP_HOME=$TC/rustup CARGO_HOME=$TC/cargo RUSTUP_TOOLCHAIN=$RUST_VER
export PATH=$CLANG_DIR/bin:$CARGO_HOME/bin:$PATH
export LIBCLANG_PATH=$CLANG_DIR/lib

log() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

kmake() {
	make -C "$KERNEL" O="$OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 LOCALVERSION= \
		PAHOLE="$PAHOLE" KBUILD_BUILD_USER="$BUILD_USER" \
		KBUILD_BUILD_HOST="$BUILD_HOST" KBUILD_BUILD_VERSION="$BUILD_NUMBER" "$@"
}

check_host() {
	local missing=() c
	for c in git make bc bison flex python3 perl rsync cpio curl zip unzip tar cc; do
		command -v "$c" >/dev/null || missing+=("$c")
	done
	command -v depmod >/dev/null || [ -x /sbin/depmod ] || [ -x /usr/sbin/depmod ] || missing+=(depmod)
	if ((${#missing[@]})); then
		die "missing host tools: ${missing[*]}
  Debian/Ubuntu: sudo apt install git make bc bison flex python3 perl rsync cpio curl zip unzip build-essential kmod libssl-dev libelf-dev zlib1g-dev
  Arch:          sudo pacman -S --needed git make bc bison flex python perl rsync cpio curl zip unzip base-devel kmod openssl libelf zlib
  Fedora:        sudo dnf install git make bc bison flex python3 perl rsync cpio curl zip unzip gcc kmod openssl-devel elfutils-libelf-devel zlib-devel"
	fi
	printf '#include <openssl/bio.h>\n#include <gelf.h>\n#include <zlib.h>\n' |
		cc -E -x c - >/dev/null 2>&1 ||
		die "missing OpenSSL, libelf or zlib headers (libssl-dev libelf-dev zlib1g-dev on Debian/Ubuntu)"
	[ "$(uname -m)" = x86_64 ] || die "the Android toolchain only runs on x86_64 Linux"
}

# Fetch only the given paths of one commit of a git repository.
sparse_fetch() {
	local dir=$1 url=$2 sha=$3; shift 3
	[ -f "$dir/.fetched-$sha" ] && return
	rm -rf "$dir"
	git init -q "$dir"
	git -C "$dir" remote add origin "$url"
	git -C "$dir" config core.sparseCheckout true
	printf '%s\n' "$@" > "$dir/.git/info/sparse-checkout"
	git -C "$dir" fetch -q --depth=1 --filter=blob:none origin "$sha"
	git -C "$dir" checkout -q FETCH_HEAD
	touch "$dir/.fetched-$sha"
}

step_tools() {
	check_host
	mkdir -p "$TC"

	if [ "$CLANG_DIR" = "$TC/clang/$CLANG_VER" ] && [ ! -f "$TC/clang/.fetched-$CLANG_SHA" ]; then
		log "Fetching Android Clang $CLANG_VER (about 3.5 GB)"
		sparse_fetch "$TC/clang" "$CLANG_REPO" "$CLANG_SHA" "/$CLANG_VER/"
	fi
	[ -x "$CLANG_DIR/bin/clang" ] || die "no clang in $CLANG_DIR/bin"

	if [ "$PAHOLE" = "$TC/pahole" ] && [ ! -f "$TC/build-tools/.fetched-$BUILDTOOLS_SHA" ]; then
		log "Fetching pahole v1.25 from Android build-tools"
		sparse_fetch "$TC/build-tools" "$BUILDTOOLS_REPO" "$BUILDTOOLS_SHA" /linux-x86/bin/pahole
		cat > "$TC/pahole" <<-EOF
		#!/bin/sh
		LD_LIBRARY_PATH="$CLANG_DIR/lib/x86_64-unknown-linux-gnu" exec "$TC/build-tools/linux-x86/bin/pahole" "\$@"
		EOF
		chmod +x "$TC/pahole"
	fi
	"$PAHOLE" --version >/dev/null || die "$PAHOLE does not run"

	if [ ! -x "$CARGO_HOME/bin/rustup" ]; then
		log "Installing rustup into $TC"
		curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs |
			env -u RUSTUP_TOOLCHAIN RUSTUP_INIT_SKIP_PATH_CHECK=yes sh -s -- -y -q --no-modify-path --profile minimal --default-toolchain none
	fi
	if ! rustc --version 2>/dev/null | grep -q "^rustc $RUST_VER " || ! rustfmt --version >/dev/null 2>&1; then
		log "Installing Rust $RUST_VER with rust-src"
		env -u RUSTUP_TOOLCHAIN rustup toolchain install "$RUST_VER" --profile minimal -c rust-src -c rustfmt
	fi
	if ! bindgen --version 2>/dev/null | grep -q " $BINDGEN_VER\$"; then
		log "Building bindgen $BINDGEN_VER"
		cargo install -q --locked bindgen-cli --version "$BINDGEN_VER"
	fi

	if [ ! -f "$TC/mkdtboimg.py" ]; then
		curl -sSf "$LIBUFDT_REPO/+/$LIBUFDT_SHA/utils/src/mkdtboimg.py?format=TEXT" |
			base64 -d > "$TC/mkdtboimg.py.tmp"
		mv "$TC/mkdtboimg.py.tmp" "$TC/mkdtboimg.py"
	fi

	if [ ! -f "$TC/AnyKernel3/.fetched-$AK3_SHA" ]; then
		rm -rf "$TC/AnyKernel3"
		git init -q "$TC/AnyKernel3"
		git -C "$TC/AnyKernel3" fetch -q --depth=1 "$AK3_REPO" "$AK3_SHA"
		git -C "$TC/AnyKernel3" checkout -q FETCH_HEAD
		touch "$TC/AnyKernel3/.fetched-$AK3_SHA"
	fi

	log "Toolchain ready"
	clang --version | head -1
	rustc --version
	bindgen --version
	echo "pahole $("$PAHOLE" --version)"
}

need_tools() {
	[ -x "$CLANG_DIR/bin/clang" ] && command -v rustc >/dev/null && command -v bindgen >/dev/null &&
		"$PAHOLE" --version >/dev/null 2>&1 || die "toolchain missing, run: ./build.sh tools"
}

need_kernel() {
	[ -f "$OUT/arch/arm64/boot/Image" ] && [ -f "$OUT/Module.symvers" ] ||
		die "no kernel build in $OUT, run: ./build.sh kernel"
}

step_kernel() {
	need_tools
	mkdir -p "$OUT" "$LOGS"
	log "Configuring: gki_defconfig + vendor/redwood.config"
	kmake -s gki_defconfig vendor/redwood.config
	grep -q '^CONFIG_RUST=y' "$OUT/.config" ||
		die "Kconfig turned CONFIG_RUST off (no ashmem, no Rust Binder). Check: make O=$OUT LLVM=1 rustavailable"
	log "Building Image and in-tree modules with $JOBS jobs (log: $LOGS/kernel.log)"
	kmake -j"$JOBS" Image modules scripts_dtc 2>&1 | tee "$LOGS/kernel.log"
	log "Kernel $(kmake -s kernelrelease) built: $OUT/arch/arm64/boot/Image"
}

# The module build scripts in redwood-6.18-modules/bringup expect
# $R/redwood-6.18-kernel, $R/out and $R/redwood-6.18-modules.
link_kernel() {
	local l=$WORK/redwood-6.18-kernel
	[ "$(readlink -f "$l")" = "$KERNEL" ] && return
	[ -e "$l" ] && [ ! -L "$l" ] && die "$l exists and is not this kernel tree"
	ln -sfn "$KERNEL" "$l"
}

get_repo() {
	local dir=$1 url=$2 ref=$3
	if [ -d "$dir/.git" ]; then
		echo "Using $dir as it is ($(git -C "$dir" log -1 --format='%h %s'))"
	else
		git clone -q --depth=1 -b "$ref" "$url" "$dir"
		echo "Cloned $url $ref ($(git -C "$dir" log -1 --format='%h'))"
	fi
}

step_modules() {
	need_tools
	need_kernel
	link_kernel
	log "Vendor module sources"
	get_repo "$MODSRC" "$MODULES_REPO" "$MODULES_REF"
	local b=$MODSRC/bringup s
	export R=$WORK CLANG=$CLANG_DIR/bin PAHOLE
	for s in vendor618 data618 wlan618 misc618 redwood618; do
		log "build-$s.sh (logs in $WORK/vb)"
		bash "$b/build-$s.sh"
	done
	collect_modules
}

collect_modules() {
	local list=$KERNEL/redwood/modules.list stage=$WORK/stage krel f p n=0
	local -A ko=()
	krel=$(kmake -s kernelrelease)
	log "Collecting the $(wc -l < "$list") shipped modules"
	# Vendor builds win over an in-tree module of the same name.
	while IFS= read -r p; do ko[${p##*/}]=$p; done < <(find "$OUT" -name '*.ko')
	while IFS= read -r p; do ko[${p##*/}]=$p; done < <(find "$WORK/vb" -name '*.ko')
	local missing=()
	while IFS= read -r f; do [ -n "${ko[$f]:-}" ] || missing+=("$f"); done < "$list"
	((${#missing[@]} == 0)) ||
		die "${#missing[@]} modules were not built: ${missing[*]}
  Look for errors in $WORK/vb/*.log and $WORK/vb/rw/*.log"

	rm -rf "$DIST/modules" "$stage"
	mkdir -p "$DIST/modules" "$stage/lib/modules/$krel"
	while IFS= read -r f; do
		llvm-strip --strip-debug -o "$DIST/modules/$f" "${ko[$f]}"
		n=$((n + 1))
	done < "$list"
	cp "$KERNEL/redwood/modules.load" "$DIST/modules/"
	cp "$DIST/modules/"*.ko "$stage/lib/modules/$krel/"
	local depmod
	depmod=$(command -v depmod || echo /sbin/depmod)
	"$depmod" -e -F "$OUT/System.map" -b "$stage" "$krel" 2> "$LOGS/depmod.log" || true
	if grep -q 'unknown symbol' "$LOGS/depmod.log"; then
		cat "$LOGS/depmod.log" >&2
		die "modules with unresolved symbols, see above"
	fi
	for f in modules.dep modules.alias modules.softdep; do
		cp "$stage/lib/modules/$krel/$f" "$DIST/modules/"
	done
	: > "$DIST/modules/modules.blocklist"
	: > "$DIST/modules/modules.options"
	rm -rf "$stage"
	echo "$n modules in $DIST/modules"
}

step_dtbo() {
	need_tools
	need_kernel
	link_kernel
	log "Building dtbo.img"
	get_repo "$DTSRC" "$DT_REPO" "$DT_REF"
	get_repo "$MODSRC" "$MODULES_REPO" "$MODULES_REF"
	R=$WORK CLANG=$CLANG_DIR/bin bash "$MODSRC/bringup/build-dt.sh" > "$LOGS/dt.log" 2>&1 ||
		{ cat "$LOGS/dt.log" >&2; die "devicetree build failed"; }
	mkdir -p "$DIST"
	python3 "$TC/mkdtboimg.py" create "$DIST/dtbo.img" "$WORK/out-dt/redwood-overlay.dtbo"
	echo "$DIST/dtbo.img"
}

step_zip() {
	need_kernel
	[ -f "$DIST/modules/modules.load" ] || die "no modules in $DIST/modules, run: ./build.sh modules"
	[ -f "$DIST/dtbo.img" ] || die "no $DIST/dtbo.img, run: ./build.sh dtbo"
	[ -f "$TC/AnyKernel3/.fetched-$AK3_SHA" ] || die "AnyKernel3 missing, run: ./build.sh tools"
	local krel kver date z=$WORK/zip zipfile
	krel=$(kmake -s kernelrelease)
	kver=$(kmake -s kernelversion)
	date=$(date +%Y%m%d)
	zipfile=$DIST/Suvkernel-$kver-redwood-$date.zip
	log "Packing $zipfile"
	cp "$OUT/arch/arm64/boot/Image" "$DIST/Image"
	rm -rf "$z"
	mkdir -p "$z"
	rsync -a --exclude=.git --exclude=.github --exclude='.fetched-*' --exclude=README.md \
		--exclude=anykernel.sh --exclude=modules --exclude=patch --exclude=ramdisk \
		"$TC/AnyKernel3/" "$z/"
	sed -e "s/@KREL@/$krel/g" -e "s/@KVER@/$kver/g" -e "s/@DATE@/$date/g" \
		"$KERNEL/redwood/anykernel.sh" > "$z/anykernel.sh"
	cp "$DIST/Image" "$DIST/dtbo.img" "$z/"
	cp -r "$DIST/modules" "$z/modules618"
	rm -f "$zipfile"
	(cd "$z" && zip -qr9 "$zipfile" .)
	rm -rf "$z"
	sha256sum "$zipfile"
}

step_clean() {
	log "Removing build output in $WORK (toolchain and sources stay)"
	rm -rf "$OUT" "$WORK/vb" "$WORK/out-dt" "$WORK/zip" "$WORK/stage" "$DIST" "$LOGS"
}

steps=("$@")
((${#steps[@]})) || steps=(tools kernel modules dtbo zip)
for s in "${steps[@]}"; do
	case $s in
	tools | kernel | modules | dtbo | zip | clean) ;;
	*) die "unknown step '$s' (tools, kernel, modules, dtbo, zip, clean)" ;;
	esac
done
mkdir -p "$WORK" "$LOGS"
echo "Kernel source: $KERNEL"
echo "Work dir:      $WORK"
echo "Toolchain:     $TC"
start=$SECONDS
for s in "${steps[@]}"; do "step_$s"; done
log "Done in $(((SECONDS - start) / 60)) min. Output: $DIST"
