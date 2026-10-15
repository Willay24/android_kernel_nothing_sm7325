#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Regenerate the lahaina (SM7325) defconfigs from the Qualcomm fragments.
# and from scripts/gki/{envsetup,generate_defconfig}.sh.
#
# Usage :
#   scripts/update_defconfig.sh                 # regenerates lahaina-qgki_defconfig
#   scripts/update_defconfig.sh gki qgki        # several variants
#   scripts/update_defconfig.sh all             # every variant whose fragments exist
#   scripts/update_defconfig.sh -u qgki         # also merge debugfs.config ("user" build)
#   scripts/update_defconfig.sh -v qgki         # show merge_config.sh warnings
#
# Variants:  gki | qgki | qgki-consolidate | qgki-debug
#
# Toolchain (override from the environment if needed):
#   CLANG_BIN=$HOME/clang/bin   CROSS_COMPILE=aarch64-linux-gnu-
#   CLANG_TRIPLE=aarch64-linux-gnu-   LLVM=1

set -u
set -o pipefail

PLATFORM="lahaina"
ARCH="arm64"
BASE_NAME="gki_defconfig"
ALL_VARIANTS="gki qgki qgki-consolidate qgki-debug"

# Fork-specific, used only for the commit message.
REPO_URL="https://github.com/Willay24/android_kernel_nothing_sm7325"

USER_VARIANT=0
VERBOSE=0

# ---------------------------------------------------------------- environment
KERNEL_TOP=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$KERNEL_TOP" || exit 1

CONFIGS="arch/${ARCH}/configs"
VENDOR="${CONFIGS}/vendor"

CLANG_BIN="${CLANG_BIN:-$HOME/clang/bin}"
export PATH="${CLANG_BIN}:${PATH}"
export ARCH
export LLVM="${LLVM:-1}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
CLANG_TRIPLE="${CLANG_TRIPLE:-aarch64-linux-gnu-}"

# Fragments (same names envsetup.sh derives from PLATFORM_NAME)
BASE_DEFCONFIG="${CONFIGS}/${BASE_NAME}"
FRAG_GKI="${VENDOR}/${PLATFORM}_GKI.config"
FRAG_QGKI="${VENDOR}/${PLATFORM}_QGKI.config"
FRAG_CONSOLIDATE="${VENDOR}/${PLATFORM}_consolidate.config"
FRAG_DEBUG="${VENDOR}/${PLATFORM}_debug.config"
FRAG_DEBUGFS="${VENDOR}/debugfs.config"

# ----------------------------------------------------------------------- args
while getopts ":uvh" opt; do
	case "$opt" in
		u) USER_VARIANT=1 ;;
		v) VERBOSE=1 ;;
		h) sed -n '5,19p' "$0"; exit 0 ;;
		*) echo "Unknown option: -$OPTARG"; exit 1 ;;
	esac
done
shift $((OPTIND - 1))

VARIANTS="${*:-qgki}"
[ "$VARIANTS" = "all" ] && VARIANTS="$ALL_VARIANTS"

# ------------------------------------------------------------------ preflight
die() { echo "ERROR: $*" >&2; exit 1; }

[ -f Makefile ] && [ -d scripts/kconfig ] || die "not a kernel tree: $KERNEL_TOP"
[ -x scripts/kconfig/merge_config.sh ] || die "scripts/kconfig/merge_config.sh missing or not executable"
[ -f "$BASE_DEFCONFIG" ] || die "$BASE_DEFCONFIG not found (base config for the GKI blend)"
[ -f "$FRAG_GKI" ]       || die "$FRAG_GKI not found"
[ -f .config ] && die "stray .config in the source tree breaks O= builds; remove it (or run 'make mrproper')"

KERNEL_HEAD=$(git rev-parse HEAD 2>/dev/null || echo unknown)

WORK=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$WORK"' EXIT

# Same transformation as scripts/gki/fragment_allyesconfig.sh: =m -> =y
ALLYES_FRAG="${WORK}/${PLATFORM}_ALLYES_GKI.config"
sed 's/=m$/=y/g' "$FRAG_GKI" > "$ALLYES_FRAG"

# ----------------------------------------------------- blend order, low -> high
# Mirrors the case statement + "reverse the order" step of generate_defconfig.sh:
#   base, (allyes GKI), QGKI, debugfs, consolidate, debug
blend_for() {
	local variant=$1
	BLEND=("$BASE_DEFCONFIG")
	case "$variant" in
		gki)
			BLEND+=("$FRAG_GKI")
			;;
		qgki|qgki-consolidate|qgki-debug)
			BLEND+=("$ALLYES_FRAG")
			[ -f "$FRAG_QGKI" ] || { echo "missing $FRAG_QGKI"; return 1; }
			BLEND+=("$FRAG_QGKI")
			if [ "$USER_VARIANT" -eq 1 ]; then
				[ -f "$FRAG_DEBUGFS" ] || { echo "missing $FRAG_DEBUGFS"; return 1; }
				BLEND+=("$FRAG_DEBUGFS")
			fi
			if [ "$variant" != "qgki" ]; then
				[ -f "$FRAG_CONSOLIDATE" ] || { echo "missing $FRAG_CONSOLIDATE"; return 1; }
				BLEND+=("$FRAG_CONSOLIDATE")
			fi
			if [ "$variant" = "qgki-debug" ]; then
				[ -f "$FRAG_DEBUG" ] || { echo "missing $FRAG_DEBUG"; return 1; }
				BLEND+=("$FRAG_DEBUG")
			fi
			;;
		*)
			echo "unknown variant '$variant' (valid: $ALL_VARIANTS)"
			return 1
			;;
	esac
}

make_args=(
	ARCH="$ARCH"
	LLVM="$LLVM"
	CROSS_COMPILE="$CROSS_COMPILE"
	CLANG_TRIPLE="$CLANG_TRIPLE"
	-j"$(nproc --all)"
)

# ------------------------------------------------------------------- generate
DONE=()
for variant in $VARIANTS; do
	out="${WORK}/out-${variant}"
	target="${VENDOR}/${PLATFORM}-${variant}_defconfig"

	echo "================================================="
	echo "Platform : ${PLATFORM}"
	echo "Variant  : ${variant}$([ "$USER_VARIANT" -eq 1 ] && echo ' (+debugfs)')"

	if ! msg=$(blend_for "$variant"); then
		if [ "$ALL_VARIANTS" = "$VARIANTS" ]; then
			echo "SKIP: $msg"
			continue
		fi
		die "$msg"
	fi

	echo "Blend    : ${BLEND[*]##*/}"
	mkdir -p "$out"

	echo "Running scripts/kconfig/merge_config.sh ..."
	if ! ret=$(MAKE_ARGS="${make_args[*]}" scripts/kconfig/merge_config.sh \
			-O "$out" "${BLEND[@]}" 2>&1); then
		echo "$ret"
		die "merge_config.sh failed for ${variant}"
	fi
	[ "$VERBOSE" -eq 1 ] && echo "$ret"

	echo "Building new defconfig ..."
	if ! ret=$(make O="$out" "${make_args[@]}" savedefconfig 2>&1); then
		echo "$ret"
		die "savedefconfig failed for ${variant}"
	fi

	[ -f "$out/defconfig" ] || die "no defconfig produced for ${variant}"
	mv "$out/defconfig" "$target"
	echo "Wrote    : $target"
	DONE+=("$variant")
done

[ "${#DONE[@]}" -gt 0 ] || die "nothing generated"

# --------------------------------------------------------------------- summary
echo "================================================="
echo "Done!"
git --no-pager diff --stat -- "$VENDOR" 2>/dev/null || true

names=$(printf '%s, ' "${DONE[@]}"); names=${names%, }
cat <<EOM

Suggested commit message:

arm64: configs: vendor: ${PLATFORM}: update auto-generated defconfig (${names})

This update is generated automatically by using the update_defconfig script.
HEAD of the project used to prepare this commit:
${REPO_URL}/tree/${KERNEL_HEAD}
EOM
