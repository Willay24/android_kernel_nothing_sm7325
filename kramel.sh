#!/usr/bin/env bash
# YAKB v2.0 - Yet Another Kernel Builder
# Original author: cyberknight777 (GPLv3) - https://github.com/cyberknight777/YAKB
# Customised for: Nothing Phone (1) [Spacewar] - lahaina-qgki
#
#			GNU GENERAL PUBLIC LICENSE
#			 Version 3, 29 June 2007
#
# Some Placeholders: [!] [*] [✓] [✗]

# ---------------------------------------------------------------------------
# Device / kernel identity (values taken from build.sh)
# ---------------------------------------------------------------------------

# Default defconfig to use for builds.
export CONFIG=phone1_defconfig

# Default directory where kernel is located in.
KDIR=$(pwd)
export KDIR

# Device name.
export DEVICE="Nothing Phone (1)"

# Device codename.
export CODENAME="spacewar"

# Value passed as TARGET_PRODUCT to kbuild.
export TARGET_PRODUCT="lahaina-qgki"

# Kernel name used for CONFIG_LOCALVERSION (-<name>-<version>) and the zip name.
export KERNEL_NAME="Spacewar"

# Builder name.
export BUILDER="Willay24"

# Build user/host shown in `uname -a` (override with env vars if you like).
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-Willay24}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-Kali_GNU/Linux-2026.2}"

# Time zone used for build dates / zip names.
export TZ=Europe/Kiev

# Date of build.
DATE=$(date +"%Y-%m-%d")
export DATE
BUILD_TIME=$(date +"%Y%m%d-%H%M")
export BUILD_TIME

# Kernel repository URL (derived from your git remote, override with REPO_URL=...).
REPO_URL="${REPO_URL:-$(git config --get remote.origin.url 2>/dev/null |
	sed -e 's#^git@github.com:#https://github.com/#' -e 's#\.git$##')}"
export REPO_URL

# Commit hash of HEAD.
COMMIT_HASH=$(git rev-parse --short HEAD)
export COMMIT_HASH

# Debug variable to exempt stripping debug symbols from modules.
export DEBUG=0

# Build status. Run with RELEASE=1 for release builds.
if [ "${RELEASE}" == 1 ]; then
	export STATUS="Release"
else
	export STATUS="Bleeding-Edge"
fi

# ---------------------------------------------------------------------------
# Telegram (credentials come from the environment, never hardcoded)
#   TOKEN  / TELEGRAM_BOT_TOKEN
#   CHATID / TELEGRAM_CHAT_ID
# Notifications are enabled automatically only when both are set.
# ---------------------------------------------------------------------------
TOKEN="${TOKEN:-${TELEGRAM_BOT_TOKEN:-}}"
CHATID="${CHATID:-${TELEGRAM_CHAT_ID:-}}"
export TOKEN CHATID
if [ -n "${TOKEN}" ] && [ -n "${CHATID}" ]; then
	export TGI=1
else
	export TGI=0
fi

# A function to send message(s) via Telegram's BOT api.
tg() {
	local response
	response=$(curl -sX POST https://api.telegram.org/bot"${TOKEN}"/sendMessage \
		-d chat_id="${CHATID}" \
		-d parse_mode=Markdown \
		-d disable_web_page_preview=true \
		-d text="$1")

	if ! echo "$response" | grep -q '"ok":true'; then
		local err
		err=$(echo "$response" | sed -n 's/.*"description":"\([^"]*\)".*/\1/p')
		echo -e "\n\e[1;31m[✗] tg(): Failed to send message: ${err:-Unknown error}\e[0m" >&2
		exit 1
	fi
}

# A function to send file(s) via Telegram's BOT api.
tgs() {
	local MD5 response
	MD5=$(md5sum "$1" | cut -d' ' -f1)

	response=$(curl -sX POST -F document=@"$1" https://api.telegram.org/bot"${TOKEN}"/sendDocument \
		-F "chat_id=${CHATID}" \
		-F "parse_mode=Markdown" \
		--form-string "caption=$2 | *MD5*: \`$MD5\`")

	if ! echo "$response" | grep -q '"ok":true'; then
		local err
		err=$(echo "$response" | sed -n 's/.*"description":"\([^"]*\)".*/\1/p')
		echo -e "\n\e[1;31m[✗] tgs(): Failed to send file '$1': ${err:-Unknown error}\e[0m" >&2
		exit 1
	fi
}

# A function to handle notifying errors.
abort() {

	if [[ ${TGI} == "1" ]]; then
		if [ -z "${2}" ]; then
			tg "*${1}*"
		else
			tgs "${2}" "*${1}*"
		fi
	fi

	echo -e "\n\e[1;31m[✗] ${1}\e[0m"
	exit 1
}

# ---------------------------------------------------------------------------
# Common setup
# ---------------------------------------------------------------------------

# Number of jobs to run.
PROCS=$(nproc --all)
export PROCS

# Compiler to use for builds: clang (Neutron Clang) or gcc (cyberknight777 GCC).
export COMPILER="${COMPILER:-clang}"

# Common directories setup.
OUT_DIR="${KDIR}/out"
DIST_DIR="${OUT_DIR}/dist"
AK3="${KDIR}/anykernel3-${CODENAME}"
ZIP_DIR="${OUT_DIR}/AnyKernel3"

# AnyKernel3 repo/branch (from build.sh). Override with AK3_URL / AK3_BRANCH
# e.g. AK3_BRANCH=<your modules branch> if you keep a separate one.
AK3_URL="${AK3_URL:-https://github.com/William24hmar/AnyKernel3.git}"
AK3_BRANCH="${AK3_BRANCH:-master}"

# Toolchains are kept next to the kernel tree, the same place build.sh uses,
# so an existing ../Nothing-Built is reused instead of downloaded again.
TC_DIR="${TC_DIR:-$(cd "${KDIR}/.." && pwd)/Nothing-Built}"

# Pinned toolchain releases (from build.sh).
NEUTRON_RELEASE="06092026"
NEUTRON_SHA256="531083928c8e37f6b31e324dbfd0b5a8323f989b1c57ff2c1368b889b5b1bd30"

# Kernel build outputs.
IMAGE="${OUT_DIR}/arch/arm64/boot/Image"
BOOT_DIR="${OUT_DIR}/arch/arm64/boot"

# Modules that must live in vendor_ramdisk (everything else goes to vendor_dlkm).
VENDOR_RAMDISK_MODULES=(
	fts_tp.ko
	q6_pdr_dlkm.ko
	q6_notifier_dlkm.ko
	snd_event_dlkm.ko
	apr_dlkm.ko
	adsp_loader_dlkm.ko
)

# Modules removed from modules.load and blocklisted in vendor_dlkm.
BLOCKLIST_MODULES=(hlcan slcan can327 8188eu 88XXau 8814au)

# Requirements
if [ "${CI:-0}" != 1 ]; then
	if ! hash make curl git zip find tar zstd 2>/dev/null; then
		abort "Failed to find make, curl, git, zip, find, tar and zstd!"
	fi
fi

# A function to download + verify + install a toolchain.
# usage: fetch_toolchain <dirname> <release> <sha256> <base-url> <asset> <probe-binary> [tar-flags]
fetch_toolchain() {
	local name="$1" release="$2" sha="$3" url="$4" asset="$5" probe="$6" tarflag="${7:-}"
	local dest="${TC_DIR}/${name}" stage

	if [ -x "${dest}/bin/${probe}" ] && [ "$(cat "${dest}/.release" 2>/dev/null)" = "${release}" ]; then
		return 0
	fi

	echo -e "\n\e[1;93m[*] Downloading ${name} ${release}! \e[0m"
	mkdir -p "${TC_DIR}" || return 1
	stage=$(mktemp -d "${TC_DIR}/.${name}.XXXXXX") || return 1

	curl -fL --retry 3 "${url}/${asset}" -o "${stage}/${asset}" || {
		rm -rf "${stage}"
		return 1
	}

	printf '%s  %s\n' "${sha}" "${stage}/${asset}" | sha256sum --check --status || {
		echo -e "\e[1;31m[✗] ${name} archive checksum mismatch!\e[0m"
		rm -rf "${stage}"
		return 1
	}

	mkdir -p "${stage}/install" || {
		rm -rf "${stage}"
		return 1
	}
	# shellcheck disable=SC2086
	tar ${tarflag} -xf "${stage}/${asset}" -C "${stage}/install" --strip-components=1 &&
		[ -x "${stage}/install/bin/${probe}" ] || {
		echo -e "\e[1;31m[✗] Failed to extract or validate ${name}!\e[0m"
		rm -rf "${stage}"
		return 1
	}
	printf '%s\n' "${release}" >"${stage}/install/.release" || {
		rm -rf "${stage}"
		return 1
	}

	# Keep the existing toolchain until the replacement is in place.
	[ ! -e "${dest}" ] || mv "${dest}" "${stage}/previous" || {
		rm -rf "${stage}"
		return 1
	}
	mv "${stage}/install" "${dest}" || {
		[ ! -e "${stage}/previous" ] || mv "${stage}/previous" "${dest}"
		rm -rf "${stage}"
		return 1
	}
	rm -rf "${stage}"
	echo -e "\n\e[1;32m[✓] ${name} ${release} is ready! \e[0m"
}

# Arguments common to both compilers.
MAKE=(
	O="${OUT_DIR}"
	ARCH=arm64
	SUBARCH=arm64
	DTC_FLAGS=-q
	TARGET_PRODUCT="${TARGET_PRODUCT}"
	KCFLAGS=-Wno-error=unused-function
	KBUILD_BUILD_USER="${KBUILD_BUILD_USER}"
	KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST}"
)

if [[ ${COMPILER} == gcc ]]; then
	# cyberknight777's GCC toolchains (arm64 + arm), cloned next to the kernel tree.
	if [ ! -d "${KDIR}/${COMPILER}64" ]; then
		git clone https://github.com/cyberknight777/gcc-arm64 --depth=1 ${COMPILER}64 ||
			abort "Failed to download GCC64 toolchain!"
	fi

	if [ ! -d "${KDIR}/${COMPILER}32" ]; then
		git clone https://github.com/cyberknight777/gcc-arm --depth=1 ${COMPILER}32 ||
			abort "Failed to download GCC32 toolchain!"
	fi

	KBUILD_COMPILER_STRING=$("${KDIR}"/${COMPILER}64/bin/aarch64-elf-gcc --version | head -n 1)
	export KBUILD_COMPILER_STRING
	export PATH="${KDIR}"/"${COMPILER}"32/bin:"${KDIR}"/"${COMPILER}"64/bin:/usr/bin/:"${PATH}"
	LINKER="${KDIR}/${COMPILER}64/bin/aarch64-elf-ld"
	MAKE+=(
		KBUILD_COMPILER_STRING="${KBUILD_COMPILER_STRING}"
		CROSS_COMPILE=aarch64-elf-
		CROSS_COMPILE_ARM32=arm-eabi-
		CROSS_COMPILE_COMPAT=arm-linux-gnueabi-
		LD="${LINKER}"
		HOSTLD=aarch64-elf-ld
		AR=aarch64-elf-ar
		AS=aarch64-elf-as
		NM=aarch64-elf-nm
		OBJDUMP=aarch64-elf-objdump
		OBJCOPY=aarch64-elf-objcopy
		CC=aarch64-elf-gcc
		GCC_LTO=1
		GRAPHITE=1
	)

elif [[ ${COMPILER} == clang ]]; then
	# Neutron Clang
	fetch_toolchain NEUTRON "${NEUTRON_RELEASE}" "${NEUTRON_SHA256}" \
		"https://github.com/Neutron-Toolchains/clang-build-catalogue/releases/download/${NEUTRON_RELEASE}" \
		"neutron-clang-${NEUTRON_RELEASE}.tar.zst" clang --zstd || abort "Failed to set up Neutron Clang!"

	export PATH="${TC_DIR}/NEUTRON/bin:${PATH}"

	KBUILD_COMPILER_STRING=$(clang -v 2>&1 | head -n 1 | sed 's/(https..*//' | sed 's/ version//')
	export KBUILD_COMPILER_STRING
	LINKER="${TC_DIR}/NEUTRON/bin/ld.lld"
	MAKE+=(
		KBUILD_COMPILER_STRING="${KBUILD_COMPILER_STRING}"
		LLVM=1
		LLVM_IAS=1
		CC=clang
		HOSTLD=ld.lld
		CROSS_COMPILE=aarch64-linux-gnu-
		CROSS_COMPILE_ARM32=arm-linux-gnueabi-
		CROSS_COMPILE_COMPAT=arm-linux-gnueabi-
		CLANG_TRIPLE=aarch64-linux-gnu-
	)
else
	abort "Unknown COMPILER '${COMPILER}'! Use COMPILER=clang or COMPILER=gcc."
fi

if [ ! -d "${AK3}" ]; then
	git clone --depth=1 "${AK3_URL}" -b "${AK3_BRANCH}" "${AK3}" ||
		abort "Failed to download AnyKernel3!"
fi

# 'version' file: holds the release name and build number.
if [ ! -f "${KDIR}/version" ]; then
	printf 'ver=r1\nnum=1\n' >"${KDIR}/version"
	echo -e "\n\e[1;93m[!] No 'version' file found, created a default one (ver=r1, num=1).\e[0m"
fi

KBUILD_BUILD_VERSION=$(grep '^num=' version | cut -d= -f2)
export KBUILD_BUILD_VERSION
VERSION=$(grep '^ver=' version | cut -d= -f2)
kver="${KBUILD_BUILD_VERSION}"
zipn="${KERNEL_NAME}-${VERSION}-${BUILD_TIME}"

# A function to exit on SIGINT.
exit_on_signal_SIGINT() {
	abort "Operation cancelled by user!"
}
trap exit_on_signal_SIGINT SIGINT

# ---------------------------------------------------------------------------
# Functions
# ---------------------------------------------------------------------------

# A function to clean kernel source prior building.
clean() {
	echo -e "\n\e[1;93m[*] Removing ${OUT_DIR} directory! \e[0m"
	make mrproper >/dev/null 2>&1 || true
	rm -rf "${OUT_DIR}" || abort "Failed to remove output directory!"
	echo -e "\n\e[1;32m[✓] Removed ${OUT_DIR}! \e[0m"
}

# A function to generate out/.config from the defconfig (does NOT touch the source tree).
mkcfg() {
	mkdir -p "${DIST_DIR}" || abort "Failed to create distribution output directory!"
	echo -e "\n\e[1;93m[*] Generating .config from ${CONFIG}! \e[0m"
	make "${MAKE[@]}" "${CONFIG}" || abort "Failed to generate .config from ${CONFIG}!"
}

# A function to regenerate the defconfig (minimal savedefconfig written back to the tree).
rgn() {
	mkcfg
	echo -e "\n\e[1;93m[*] Regenerating defconfig! \e[0m"
	make "${MAKE[@]}" savedefconfig || abort "Failed to run 'make savedefconfig'!"
	cp "${OUT_DIR}"/defconfig "${KDIR}"/arch/arm64/configs/"${CONFIG}" ||
		abort "Failed to copy defconfig file!"
	echo -e "\n\e[1;32m[✓] Defconfig regenerated! \e[0m"
}

# A function to open a menu based program to update current config.
mcfg() {
	mkcfg
	echo -e "\n\e[1;93m[*] Making menuconfig! \e[0m"
	make "${MAKE[@]}" menuconfig || abort "Failed to run 'make menuconfig'!"
	make "${MAKE[@]}" savedefconfig || abort "Failed to run 'make savedefconfig'!"
	cp "${OUT_DIR}"/defconfig "${KDIR}"/arch/arm64/configs/"${CONFIG}" ||
		abort "Failed to copy defconfig file!"
	echo -e "\n\e[1;32m[✓] Saved modifications! \e[0m"
}

# KernelSU's preinit runner needs sanitised UAPI headers.
ksu_headers() {
	grep -q '^CONFIG_KSU=[ym]$' "${OUT_DIR}/.config" || return 0
	echo -e "\n\e[1;93m[*] Preparing UAPI headers for KernelSU preinit! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" headers || abort "Failed to prepare KernelSU UAPI headers!"
	[ -f "${OUT_DIR}/usr/include/linux/unistd.h" ] ||
		abort "KernelSU preinit UAPI headers were not generated!"
}

# Concatenate every built .dtb into a single blob.
link_dtbs() {
	echo -e "\n\e[1;93m[*] Linking all dtb files! \e[0m"
	mkdir -p "${BOOT_DIR}"
	find "${BOOT_DIR}/dts" -name '*.dtb' -print0 | sort -z | xargs -0 cat >"${BOOT_DIR}/dtb" ||
		abort "Failed to link dtb files!"
	[ -s "${BOOT_DIR}/dtb" ] || abort "No dtb files were produced!"
	echo -e "\n\e[1;32m[✓] Linked dtb files! \e[0m"
}

# Copy a built file into DIST_DIR (mandatory = 1 aborts when missing, otherwise only warns).
dist_copy() {
	if [ -f "${1}" ]; then
		cp -p "${1}" "${DIST_DIR}"/ || abort "Failed to copy $(basename "${1}")!"
	elif [ "${2}" == 1 ]; then
		abort "Missing build output: ${1}"
	else
		echo -e "\e[1;93m[!] $(basename "${1}") was not produced, skipping.\e[0m"
	fi
}

# A function to build the kernel.
img() {
	if [[ ${TGI} == "1" ]]; then
		tg "
*Build Number*: \`${kver}\`
*Status*: \`${STATUS}\`
*Builder*: \`${BUILDER}\`
*Core count*: \`$(nproc --all)\`
*Device*: \`${DEVICE} [${CODENAME}]\`
*Kernel Version*: \`$(make kernelversion 2>/dev/null)\`
*Date*: \`$(date)\`
*Zip Name*: \`${zipn}\`
*Compiler*: \`${KBUILD_COMPILER_STRING}\`
*Linker*: \`$("${LINKER}" -v | head -n 1 | sed 's/([^)]*)//g' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')\`
*Branch*: \`$(git rev-parse --abbrev-ref HEAD)\`
*Last Commit*: [${COMMIT_HASH}](${REPO_URL}/commit/${COMMIT_HASH})
"
	fi
	mkcfg
	ksu_headers
	echo -e "\n\e[1;93m[*] Building kernel! \e[0m"
	local BUILD_START BUILD_END DIFF rc
	BUILD_START=$(date +"%s")
	make -j"$PROCS" "${MAKE[@]}" 2>&1 | tee "${KDIR}/log.txt"
	rc=${PIPESTATUS[0]}
	BUILD_END=$(date +"%s")
	DIFF=$((BUILD_END - BUILD_START))
	if [ "${rc}" -eq 0 ] && [ -f "${IMAGE}" ]; then
		if [[ ${TGI} == "1" ]]; then
			tg "*Kernel built after $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)*!"
		fi
		echo -e "\n\e[1;32m[✓] Kernel built after $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)! \e[0m"
		link_dtbs
		echo -e "\n\e[1;93m[*] Copying built files! \e[0m"
		dist_copy "${IMAGE}" 1
		dist_copy "${BOOT_DIR}/dtb" 1
		dist_copy "${BOOT_DIR}/dtb.img" 0
		dist_copy "${BOOT_DIR}/dtbo.img" 0
		echo -e "\n\e[1;32m[✓] Copied built files! \e[0m"
	else
		abort "Failed to build kernel!" "${KDIR}/log.txt"
	fi
}

# A function to build DTBs.
dtb() {
	mkcfg
	echo -e "\n\e[1;93m[*] Building DTBS and DTBO! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" dtbs || abort "Failed to build dtbs!"
	make -j"$PROCS" "${MAKE[@]}" dtbo.img ||
		echo -e "\e[1;93m[!] dtbo.img target not available, skipping.\e[0m"
	link_dtbs
	echo -e "\n\e[1;93m[*] Copying DTBS and DTBO images! \e[0m"
	dist_copy "${BOOT_DIR}/dtb" 1
	dist_copy "${BOOT_DIR}/dtb.img" 0
	dist_copy "${BOOT_DIR}/dtbo.img" 0
	echo -e "\n\e[1;32m[✓] Copied DTBS and DTBO images! \e[0m"
}

# A function to build + install kernel modules (only when CONFIG_MODULES=y).
mod() {
	if [[ ${TGI} == "1" ]]; then
		tg "*Building modules!*"
	fi
	mkcfg
	if ! grep -q '^CONFIG_MODULES=y$' "${OUT_DIR}/.config"; then
		echo -e "\n\e[1;93m[!] CONFIG_MODULES is not set, everything is built-in. Nothing to do.\e[0m"
		return 0
	fi
	echo -e "\n\e[1;93m[*] Building modules! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" modules || abort "Failed to build modules!"
	rm -rf "${OUT_DIR}/modules_inst"
	local strip=()
	[[ ${DEBUG} == '0' ]] && strip=(INSTALL_MOD_STRIP=1)
	make "${MAKE[@]}" "${strip[@]}" INSTALL_MOD_PATH="${OUT_DIR}/modules_inst" modules_install ||
		abort "Failed to install modules!"
	echo -e "\n\e[1;32m[✓] Built modules! \e[0m"
}

# A function to build kernel UAPI kernel headers.
hdr() {
	if [[ ${TGI} == "1" ]]; then
		tg "*Building UAPI kernel headers!*"
	fi
	mkcfg
	echo -e "\n\e[1;93m[*] Building UAPI kernel headers! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" INSTALL_HDR_PATH="${OUT_DIR}"/kernel_uapi_headers/usr headers_install ||
		abort "Failed to build UAPI kernel headers!"
	find "${OUT_DIR}"/kernel_uapi_headers '(' -name ..install.cmd -o -name .install ')' -exec rm '{}' +
	tar -czf "${OUT_DIR}"/kernel-uapi-headers.tar.gz --directory="${OUT_DIR}"/kernel_uapi_headers usr/ ||
		abort "Failed to create a GZ-compressed tarball for UAPI kernel headers!"
	cp -p "${OUT_DIR}"/kernel-uapi-headers.tar.gz "${DIST_DIR}"/ || abort "Failed to copy UAPI kernel headers tarball!"
	echo -e "\n\e[1;32m[✓] Built UAPI kernel headers! \e[0m"
}

# A function to modify LTO mode (Clang only).
lto() {
	if [[ ${COMPILER} != clang ]]; then
		abort "LTO mode can only be modified when COMPILER=clang!"
	fi
	if ! grep -qs "config LTO_CLANG_THIN" "${KDIR}/arch/Kconfig" "${KDIR}/init/Kconfig"; then
		abort "This kernel has no LTO_CLANG_THIN/LTO_CLANG_FULL options (older 5.4 trees use CONFIG_THINLTO)!"
	fi

	echo -e "\n\e[1;93m[*] Modifying LTO mode to ${1}! \e[0m"

	if [[ ${1} == "full" ]]; then
		"${KDIR}"/scripts/config --file "${KDIR}"/arch/arm64/configs/"${CONFIG}" \
			-e LTO_CLANG_FULL \
			-d LTO_CLANG_THIN || abort "Failed to modify LTO mode to ${1}"
	elif [[ ${1} == "thin" ]]; then
		"${KDIR}"/scripts/config --file "${KDIR}"/arch/arm64/configs/"${CONFIG}" \
			-d LTO_CLANG_FULL \
			-e LTO_CLANG_THIN || abort "Failed to modify LTO mode to ${1}"
	else
		abort "Failed to set LTO mode! Expected: 'full' or 'thin'"
	fi

	echo -e "\n\e[1;32m[✓] Modified LTO mode to ${1}! \e[0m"
}

# A sub-function that stages modules into vendor_ramdisk / vendor_dlkm.img inside ${ZIP_DIR}.
pack_modules() {
	local MI="${OUT_DIR}/modules_inst"
	local RD="${ZIP_DIR}/vendor_ramdisk/lib/modules"
	local DL="${ZIP_DIR}/vendor_dlkm/lib/modules"
	local TEMP_DIR="${OUT_DIR}/vendor_dlkm_files"
	local MODULE_DIR="${TEMP_DIR}/lib/modules"
	local m path cfg dep mod_inst_path blk_re

	# ---- vendor_ramdisk modules ----
	echo -e "\n\e[1;93m[*] Copying ${#VENDOR_RAMDISK_MODULES[@]} specific modules to vendor_ramdisk! \e[0m"
	mkdir -p "${RD}" || abort "Failed to create vendor_ramdisk module directory!"
	for m in "${VENDOR_RAMDISK_MODULES[@]}"; do
		path=$(find "${MI}" -type f -name "${m}" -print -quit)
		if [ -n "${path}" ]; then
			cp "${path}" "${RD}"/ && echo -e "  \e[1;32m+ ${m}\e[0m"
		else
			echo -e "  \e[1;31m- Missing: ${m}\e[0m"
		fi
	done

	# ---- vendor_dlkm configs ----
	echo -e "\n\e[1;93m[*] Copying modules and configs to vendor_dlkm! \e[0m"
	mkdir -p "${DL}" || abort "Failed to create vendor_dlkm module directory!"
	for cfg in modules.alias modules.dep modules.softdep; do
		path=$(find "${MI}" -type f -name "${cfg}" -print -quit)
		if [ -n "${path}" ]; then
			cp -f "${path}" "${DL}"/ && echo -e "  \e[1;32m+ ${cfg}\e[0m"
		else
			echo -e "  \e[1;31m- Missing config: ${cfg}\e[0m"
		fi
	done

	dep=$(find "${MI}" -name modules.dep -print -quit)
	[ -n "${dep}" ] && mod_inst_path=$(dirname "${dep}")

	blk_re=$(
		IFS='|'
		echo "${BLOCKLIST_MODULES[*]}"
	)

	if [ -n "${mod_inst_path}" ] && [ -f "${mod_inst_path}/modules.order" ]; then
		sed 's/.*\///g' "${mod_inst_path}/modules.order" | sed 's/^wlan.ko$/qca_cld3_wlan.ko/g' >"${DL}/modules.load"
		sed -i -E "/^(${blk_re})\.ko$/d" "${DL}/modules.load"
		echo -e "  \e[1;32m+ modules.load\e[0m"
	else
		echo -e "  \e[1;31m- Missing modules.order (cannot create modules.load)\e[0m"
	fi

	# ---- vendor_dlkm.img ----
	echo -e "\n\e[1;93m[*] Creating vendor_dlkm.img! \e[0m"
	rm -rf "${TEMP_DIR}"
	mkdir -p "${MODULE_DIR}" || abort "Failed to create vendor_dlkm staging directory!"
	find "${MI}" -type f -name "*.ko" -exec cp {} "${MODULE_DIR}"/ \;

	if [ -f "${MODULE_DIR}/wlan.ko" ]; then
		mv "${MODULE_DIR}/wlan.ko" "${MODULE_DIR}/qca_cld3_wlan.ko" &&
			echo -e "  \e[1;32m+ Renamed wlan.ko -> qca_cld3_wlan.ko\e[0m"
	else
		echo -e "  \e[1;35m~ wlan.ko not found, skipping rename\e[0m"
	fi

	# Extra vendor_dlkm files shipped by AnyKernel3 (etc/lib).
	if [ -d "${ZIP_DIR}/vendor_dlkm/etc" ]; then
		mkdir -p "${TEMP_DIR}/etc"
		cp -r "${ZIP_DIR}/vendor_dlkm/etc/"* "${TEMP_DIR}/etc/"
	fi
	if [ -d "${ZIP_DIR}/vendor_dlkm/lib" ]; then
		mkdir -p "${TEMP_DIR}/lib"
		cp -r "${ZIP_DIR}/vendor_dlkm/lib/"* "${TEMP_DIR}/lib/"
	fi
	rm -f "${TEMP_DIR}/lib/placeholder"

	# Fix paths in modules.dep for mounting in /vendor.
	if [ -f "${MODULE_DIR}/modules.dep" ]; then
		sed -i 's/wlan.ko/qca_cld3_wlan.ko/g' "${MODULE_DIR}/modules.dep"
		sed -i 's@\(^kernel/[^: ]*/\)\([^: ]*\.ko\)@/vendor/lib/modules/\2@g' "${MODULE_DIR}/modules.dep"
	fi

	# modules.blocklist
	{
		echo "# The module will be loaded from the target RC after"
		echo "# cold boot calibration."
		echo "blocklist qca_cld3_wlan"
		for m in "${BLOCKLIST_MODULES[@]}"; do
			echo "blocklist ${m}"
		done
	} >"${MODULE_DIR}/modules.blocklist"
	echo -e "  \e[1;32m+ modules.blocklist\e[0m"

	# Create vendor_dlkm.img (ext4), then shrink it.
	dd if=/dev/zero of="${ZIP_DIR}/vendor_dlkm.img" bs=1M count=512 status=none ||
		abort "Failed to allocate vendor_dlkm.img!"
	mke2fs -F -O "extent,huge_file" -T largefile -L vendor_dlkm \
		-d "${TEMP_DIR}" "${ZIP_DIR}/vendor_dlkm.img" ||
		abort "Failed to create vendor_dlkm.img!"
	e2fsck -fy "${ZIP_DIR}/vendor_dlkm.img" || true
	resize2fs -M "${ZIP_DIR}/vendor_dlkm.img" || abort "Failed to shrink vendor_dlkm.img!"

	rm -rf "${TEMP_DIR}" "${ZIP_DIR}/vendor_dlkm"
	echo -e "\n\e[1;32m[✓] Created vendor_dlkm.img! \e[0m"
}

# A function to build an AnyKernel3 zip.
mkzip() {
	if [[ ${TGI} == "1" ]]; then
		tg "*Building zip!*"
	fi
	echo -e "\n\e[1;93m[*] Building zip! \e[0m"

	[ -f "${DIST_DIR}/Image" ] || abort "Missing ${DIST_DIR}/Image! Run 'img' first."
	[ -f "${DIST_DIR}/dtb" ] || abort "Missing ${DIST_DIR}/dtb! Run 'img' or 'dtb' first."

	# Work on a throw-away copy so the AnyKernel3 clone stays pristine.
	rm -rf "${ZIP_DIR}"
	cp -r "${AK3}" "${ZIP_DIR}" || abort "Failed to stage AnyKernel3 directory!"
	rm -rf "${ZIP_DIR}/.git"

	cp -p "${DIST_DIR}/Image" "${ZIP_DIR}"/ || abort "Failed to copy Image to AnyKernel3 directory!"
	cp -p "${DIST_DIR}/dtb" "${ZIP_DIR}"/ || abort "Failed to copy dtb to AnyKernel3 directory!"
	[ -f "${DIST_DIR}/dtb.img" ] && cp -p "${DIST_DIR}/dtb.img" "${ZIP_DIR}"/
	[ -f "${DIST_DIR}/dtbo.img" ] && cp -p "${DIST_DIR}/dtbo.img" "${ZIP_DIR}"/

	if grep -q '^CONFIG_MODULES=y$' "${OUT_DIR}/.config" 2>/dev/null; then
		[ -d "${OUT_DIR}/modules_inst" ] || mod
		for t in mke2fs e2fsck resize2fs; do
			hash "${t}" 2>/dev/null || abort "${t} is not installed (needed for vendor_dlkm.img)!"
		done
		pack_modules
	else
		echo -e "\n\e[1;35m[!] CONFIG_MODULES is not set, skipping all module logic.\e[0m"
		rm -rf "${ZIP_DIR}/vendor_dlkm" "${ZIP_DIR}/vendor_ramdisk" "${ZIP_DIR}/ramdisk"
		rm -f "${ZIP_DIR}/vendor_dlkm.img"
	fi

	cp "${OUT_DIR}/.config" "${ZIP_DIR}/defconfig" 2>/dev/null

	cd "${ZIP_DIR}" || abort "Failed to cd into AnyKernel3 directory!"
	zip -r9 "${DIST_DIR}/${zipn}.zip" . \
		-x ".git*" -x "*.zip" -x README.md -x changelog.txt \
		-x defconfig -x kernel-changelog.txt -x build.log ||
		abort "Failed to package and compress AnyKernel3 directory!"
	cd "${KDIR}" || abort "Failed to cd into root kernel directory!"
	echo -e "\n\e[1;32m[✓] Built zip: ${DIST_DIR}/${zipn}.zip \e[0m"

	if [[ ${TGI} == "1" ]]; then
		tgs "${DIST_DIR}/${zipn}.zip" "*#${kver} ${KBUILD_COMPILER_STRING}*"
	fi
}

# A function to build specific objects.
obj() {
	mkcfg
	echo -e "\n\e[1;93m[*] Building ${1}! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" "${1}" || abort "Failed to build specific objects!"
	echo -e "\n\e[1;32m[✓] Built ${1}! \e[0m"
}

# A function to build external modules.
extmod() {
	mkcfg
	echo -e "\n\e[1;93m[*] Building ${1} as an external module! \e[0m"
	make -j"$PROCS" "${MAKE[@]}" modules_prepare || abort "Failed to set up build for external modules!"
	make -j"$PROCS" "${MAKE[@]}" M="${1}" modules || abort "Failed to build external modules!"
	echo -e "\n\e[1;32m[✓] Built ${1} as an external module! \e[0m"
}

# A function to uprev localversion in defconfig.
upr() {
	echo -e "\n\e[1;93m[*] Bumping localversion to -${KERNEL_NAME}-${1}! \e[0m"
	"${KDIR}"/scripts/config --file "${KDIR}"/arch/arm64/configs/"${CONFIG}" --set-str CONFIG_LOCALVERSION "-${KERNEL_NAME}-${1}" || abort "Failed to uprev localversion!"
	rgn
	echo -e "\n\e[1;32m[✓] Bumped localversion to -${KERNEL_NAME}-${1}! \e[0m"
}

# A function to showcase the options provided for args-based usage.
helpmenu() {
	echo -e "\n\e[1m
usage: bash $0 <arg>

example: bash $0 mcfg
example: bash $0 img
example: bash $0 img mkzip
example: COMPILER=gcc bash $0 img mkzip
example: bash $0 --obj=drivers/android/binder.o
example: bash $0 --obj=kernel/sched/
example: bash $0 --extmod=path/to/modulesource
example: bash $0 --upr=r16
example: bash $0 --lto=thin

	 mcfg   Runs make menuconfig
	 img    Builds Kernel (Image + dtb + dtbo.img)
	 dtb    Builds dtb(o).img
	 mod    Builds + installs kernel modules
	 hdr    Builds kernel UAPI headers
	 --lto  Modify LTO mode
	 mkzip  Builds anykernel3 zip
	 --obj  Builds specific driver/subsystem
	 --extmod Builds external modules
	 rgn    Regenerates defconfig
	 --upr  Uprevs kernel version in defconfig
	 clean  Removes the out directory

env: COMPILER=clang|gcc  RELEASE=1  CI=1  TOKEN/CHATID (or TELEGRAM_BOT_TOKEN/TELEGRAM_CHAT_ID)
     AK3_URL  AK3_BRANCH  TC_DIR  KBUILD_BUILD_USER  KBUILD_BUILD_HOST
\e[0m"
}

# A helper to ask for a value in menu mode.
ask() {
	ANSWER=$(dialog --inputbox "$1" 10 50 2>&1 >/dev/tty)
	clear >/dev/tty
	[ -n "${ANSWER}" ] || abort "No input detected!"
}

# A function to setup menu-based usage.
ndialog() {
	hash dialog 2>/dev/null || abort "Failed to find dialog! Install it or pass arguments (see: bash $0 help)."
	local choice a1
	while true; do
		choice=$(dialog --clear \
			--backtitle "Yet Another Kernel Builder" \
			--title "YAKB v2.0 - ${KERNEL_NAME} (${COMPILER})" \
			--menu "Choose one of the following options: " 20 50 13 \
			1 "Build kernel" \
			2 "Build DTBs" \
			3 "Build modules" \
			4 "Build UAPI kernel headers" \
			5 "Modify LTO mode" \
			6 "Open menuconfig" \
			7 "Regenerate defconfig" \
			8 "Uprev localversion" \
			9 "Build AnyKernel3 zip" \
			10 "Build a specific object" \
			11 "Build an external module" \
			12 "Clean + build kernel" \
			13 "Exit" \
			2>&1 >/dev/tty) || {
			clear
			exit 0
		}
		clear
		case "${choice}" in
		1) img ;;
		2) dtb ;;
		3) mod ;;
		4) hdr ;;
		5)
			ask "Enter LTO mode (thin|full): "
			lto "${ANSWER}"
			;;
		6) mcfg ;;
		7) rgn ;;
		8)
			ask "Enter version number: "
			upr "${ANSWER}"
			;;
		9) mkzip ;;
		10)
			ask "Enter object path: "
			obj "${ANSWER}"
			;;
		11)
			ask "Enter module source path: "
			extmod "${ANSWER}"
			;;
		12)
			clean
			img
			;;
		13)
			echo -e "\n\e[1m Exiting YAKB...\e[0m"
			sleep 1
			exit 0
			;;
		esac
		echo -ne "\e[1mPress enter to continue or 0 to exit! \e[0m"
		read -r a1
		if [ "${a1}" == "0" ]; then
			exit 0
		fi
		clear
	done
}

if [ "${CI}" == 1 ]; then
	upr "${VERSION}"
fi

if [[ -z $* ]]; then
	ndialog
fi

for arg in "$@"; do
	case "${arg}" in
	"mcfg")
		mcfg
		;;
	"img")
		img
		;;
	"dtb")
		dtb
		;;
	"mod")
		mod
		;;
	"hdr")
		hdr
		;;
	"--lto="*)
		ltom="${arg#*=}"
		if [[ -z ${ltom} ]]; then
			echo "Use --lto=(thin|full)"
			exit 1
		else
			lto "${ltom}"
		fi
		;;
	"mkzip")
		mkzip
		;;
	"--obj="*)
		object="${arg#*=}"
		if [[ -z ${object} ]]; then
			echo "Use --obj=filename.o"
			exit 1
		else
			obj "${object}"
		fi
		;;
	"--extmod="*)
		module="${arg#*=}"
		if [[ -z ${module} ]]; then
			echo "Use --extmod=/path/to/modulesource"
			exit 1
		else
			extmod "${module}"
		fi
		;;
	"rgn")
		rgn
		;;
	"--upr="*)
		vers="${arg#*=}"
		if [[ -z ${vers} ]]; then
			echo "Use --upr=version"
			exit 1
		else
			upr "${vers}"
		fi
		;;
	"clean")
		clean
		;;
	"help")
		helpmenu
		exit 1
		;;
	*)
		helpmenu
		exit 1
		;;
	esac
done