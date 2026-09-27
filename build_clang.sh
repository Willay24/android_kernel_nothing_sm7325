#!/usr/bin/env bash
# Copyright (c) 2021-2025, Tashfin Shakeer Rhythm <tashfinshakeerrhythm@gmail.com>.
# Copyright (c) 2023, Divyanshu-Modi <divyan.m05@gmail.com>.
# Version: 12.1
# Revision: 09-09-2025
# shellcheck disable=SC2312
# shellcheck disable=SC1091
# shellcheck disable=SC2154

## Install some packages needed for compilation
#sudo apt update && sudo apt-get -y install zstd bc lld libarchive-tools flex bison kmod ccache

## CONFIGURATION
# User details
KBUILD_USER=""
KBUILD_HOST=""
CORES=$(nproc --all)

## DIRECTORY PATHS

# Kernel Directory
KERNEL_DIR=$(pwd)

# Propriatary Directory (default paths may not work!)
PRO_PATH="$KERNEL_DIR/.."

# Toolchain Directory
TLDR="$PRO_PATH/toolchains"

# Anykernel Directories
AK3_DIR="$PRO_PATH/AnyKernel3"
AKVDR="$AK3_DIR/modules/vendor/lib/modules"

# Device Tree Blob Directory
DTB_PATH="$KERNEL_DIR/work/arch/arm64/boot/dts/vendor/qcom"

# Telegram Chat ID Info
TOKEN="${TELEGRAM_BOT_TOKEN:-}"
CHATID="${TELEGRAM_CHAT_ID:-}"
BOT_MSG_URL="https://api.telegram.org/bot${TOKEN}/sendMessage"
BOT_BUILD_URL="https://api.telegram.org/bot${TOKEN}/sendDocument"

# EVA GCC Names
EVA_DATE='07092025'
EVA_ARM64="eva-gcc-arm64-${EVA_DATE}.xz"
EVA_ARM="eva-gcc-arm-${EVA_DATE}.xz"

## Clone AnyKernel3
if [[ ! -d ${AK3_DIR} ]]; then
	git clone --depth=1 -q https://github.com/Willay24/AnyKernel3.git -b master "${AK3_DIR}"
fi

## AnyKernel3 packaging helpers
#
# AnyKernel3 (this fork included) ships no Makefile - it has never had
# `make clean` / `make zip` targets. Packaging has always just been
# `cd AnyKernel3 && zip -r9 out.zip *`. These two helpers replace the old
# make-based calls that were failing with "No rule to make target".

# Reset AK3_DIR to a clean checkout before dropping new build output into it.
ak3_reset()
{
	cd "${AK3_DIR}" || exit
	if [[ -d .git ]]; then
		git clean -fdx >/dev/null 2>&1
		git checkout -q -- . >/dev/null 2>&1
	else
		rm -f ./*.zip Image dtb dtbo.img
	fi
	cd "${KERNEL_DIR}" || exit
}

# Zip up whatever is currently in AK3_DIR and drop the result in $KERNEL_DIR/out.
ak3_pack()
{
	local ZIP_NAME="$1"

	[[ -d "$KERNEL_DIR/out" ]] || mkdir -p "$KERNEL_DIR/out"

	cd "${AK3_DIR}" || exit
	rm -f ./*.zip
	zip -r9 "${ZIP_NAME}.zip" * -x '.git*' -x '.github*' -x 'README.md' \
		|| error 'AnyKernel3 zip packaging failed'
	cp ./*.zip "${KERNEL_DIR}"/out
	cd "${KERNEL_DIR}" || exit
}

## Make directory for toolchains
if [[ ! -d ${TLDR} ]]; then
	mkdir ${TLDR}
fi

## COLORS
R='\033[1;31m'
G='\033[1;32m'
B='\033[1;34m'
W='\033[1;37m'

## MISC

# functions
error()
{
	echo -e ""
	echo -e "$R ${FUNCNAME[0]}: $W" "$@"
	echo -e ""
	exit 1
}

success()
{
	echo -e ""
	echo -e "$G ${FUNCNAME[1]}: $W" "$@"
	echo -e ""
	exit 0
}

inform()
{
	echo -e ""
	echo -e "$B ${FUNCNAME[1]}: $W" "$@" "$G"
	echo -e ""
}

muke()
{
	make "$@" "${MAKE_ARGS[@]}"
}

usage()
{
	inform " ./build_clang.sh [ARG]
		--compiler   Sets the compiler to be used.
		--compiler32 Sets the 32bit compiler to be used,
					 (defaults to clang).
		--device     Sets the device for kernel build.
		--dtbs       Builds dtbs, dtbo & dtbo.img.
		--dtb_zip    Builds flashable zip with dtbs, dtbo.
		--obj        Builds specified objects.
		--regen      Regenerates defconfig (savedefconfig).
		--repack     Repacks the already-built Image/dtb/dtbo into an
					 AnyKernel3 zip and sends it to Telegram (no rebuild,
					 no modules/vendor_dlkm). Pass --device= first.
	"
	exit 2
}

# Function to send message(s) via Telegram's BOT api
tg_post_msg() {
	curl -s -X POST "${BOT_MSG_URL}" \
		-d chat_id="${CHATID}" \
		-d "disable_web_page_preview=true" \
		-d "parse_mode=html" \
		-d text="$1"
}

# Function to send file(s) via Telegram's BOT api
tg_post_build() {
	#Post MD5Checksum alongwith for easeness
	MD5CHECK=$(md5sum "$1" | cut -d' ' -f1)

	#Show the Checksum alongwith caption
	curl --progress-bar -F document=@"$1" "${BOT_BUILD_URL}" \
		-F chat_id="${CHATID}" \
		-F "disable_web_page_preview=true" \
		-F "parse_mode=html" \
		-F caption="$2 | <b>MD5 Checksum : </b><code>${MD5CHECK}</code>"
}

compiler_setup()
{
	# default to clang
	if [[ $COMPILER == clang ]]; then
		CC='clang'
		if [[ -n "$CLANG_DIR" && -x "$CLANG_DIR/bin/clang" ]]; then
			echo -e "\nUsing pre-installed toolchain at $CLANG_DIR"
			C_PATH="$CLANG_DIR"
		else
			C_PATH="$TLDR/clang"
			if [[ ! -d ${C_PATH} ]]; then
				echo -e "\nClang not found! Cloning Neutron-clang..."
				mkdir "${C_PATH}" && cd "${C_PATH}"
				bash <(curl -s https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman) -S
				bash <(curl -s https://raw.githubusercontent.com/Neutron-Toolchains/antman/main/antman) --patch=glibc
				cd ${KERNEL_DIR}
			fi
		fi
		C_NAME="$(${C_PATH}/bin/clang -v 2>&1 | head -n 1 | sed 's/(https..*//' | sed 's/ version//')"
		MAKE_ARGS=(
			"LLVM=1"
			"LLVM_IAS=1"
			"HOSTLD=ld.lld"
			"CROSS_COMPILE=aarch64-linux-gnu-"
			"CROSS_COMPILE_COMPAT=arm-linux-gnueabi-"
		)
	fi

	if [[ $COMPILER == slim-clang ]]; then
		if [[ -n "$CLANG_DIR" && -x "$CLANG_DIR/bin/clang" ]]; then
			echo -e "\nUsing pre-installed Slim LLVM toolchain at $CLANG_DIR"
			LLVM_PATH="$CLANG_DIR/bin"
		else
			LLVM_PATH="$TLDR/llvm-21.1.1-x86_64/bin"
		fi
		CC="$LLVM_PATH/clang"
		C_NAME="$(${CC} -v 2>&1 | head -n 1 | sed 's/(https..*//' | sed 's/ version//')"
		C_PATH="$TLDR/aosp-clang/bin"
		MAKE_ARGS=(
			"LLVM=1"
			"LLVM_IAS=1"
			"HOSTCC=ccache $LLVM_PATH/clang"
			"HOSTCXX=ccache $LLVM_PATH/clang++"
			"LD=$LLVM_PATH/ld.lld"
			"HOSTLD=$LLVM_PATH/ld.lld"
			"AR=$LLVM_PATH/llvm-ar"
			"NM=$LLVM_PATH/llvm-nm"
			"OBJCOPY=$LLVM_PATH/llvm-objcopy"
			"OBJDUMP=$LLVM_PATH/llvm-objdump"
			"READELF=$LLVM_PATH/llvm-readelf"
			"STRIP=$LLVM_PATH/llvm-strip"
			"CROSS_COMPILE=$C_PATH/aarch64-linux-gnu-"
			"CROSS_COMPILE_COMPAT=$C_PATH/arm-linux-gnueabi-"
		)
	fi

	if [[ $COMPILER == gcc ]]; then
		CC='aarch64-elf-gcc'
		C_PATH="$TLDR/gcc-arm64"
		if [[ ! -d ${C_PATH} ]]; then
			echo -e "\n${YELLOW}gcc-arm64 not found! Cloning EVA gcc-arm64..."
			cd $TLDR
			wget https://github.com/mvaisakh/gcc-build/releases/download/"${EVA_DATE}"/"${EVA_ARM64}"
			tar -xf "${EVA_ARM64}"
			rm -rf "${EVA_ARM64}"
			cd -
		fi
		C_NAME="$(${C_PATH}/bin/aarch64-elf-gcc --version | head -n 1)"
		MAKE_ARGS=(
			"CROSS_COMPILE=aarch64-elf-"
			"LD_LIBRARY_PATH=$C_PATH/lib:$LD_LIBRARY_PATH"
		)
	fi

	if [[ "$COMPILER32" == "gcc" ]]; then
		if [[ ! -d ${TLDR}/gcc-arm ]]; then
			echo -e "\n${YELLOW}gcc-arm not found! Cloning EVA gcc-arm..."
			cd $TLDR
			wget https://github.com/mvaisakh/gcc-build/releases/download/"${EVA_DATE}"/"${EVA_ARM}"
			tar -xf "${EVA_ARM}"
			rm -rf "${EVA_ARM}"
			cd -
		fi
		MAKE_ARGS+=(
			"CROSS_COMPILE_COMPAT=$TLDR/gcc-arm/bin/arm-eabi-"
		)
	fi

	MAKE_ARGS+=(
		"O=work"
		"ARCH=arm64"
		"-j"$CORES""
		"CC=ccache $CC"
		"PATH=$C_PATH/bin:$PATH"
		"KBUILD_BUILD_USER=$KBUILD_USER"
		"KBUILD_BUILD_HOST=$KBUILD_HOST"
	)
}

config_generator()
{
	if [[ -z $CODENAME ]]; then
		error 'Codename not present connot proceed'
		exit 1
	fi
	if [[ -z $BASE ]]; then
		DFCF="vendor/${CODENAME}-${SUFFIX}_defconfig"
	else
		DFCF="vendor/${BASE}-${SUFFIX}_defconfig"
	fi

	if [[ ! -f arch/arm64/configs/$DFCF ]]; then
		# Cleanup work directory
		rm -rf work out log.txt

		inform "Generating defconfig"

		export "${MAKE_ARGS[@]}" "TARGET_BUILD_VARIANT=user"

		if [[ -z $BASE ]]; then
			bash scripts/gki/generate_defconfig.sh "${CODENAME}-${SUFFIX}_defconfig"
			muke $DFCF vendor/lahaina_QGKI.config savedefconfig
		else
			bash scripts/gki/generate_defconfig.sh "${BASE}-${SUFFIX}_defconfig"
		fi
		muke $DFCF vendor/lahaina_QGKI.config savedefconfig
		rm -rf arch/arm64/configs/$DFCF
		mv work/defconfig arch/arm64/configs/$DFCF

		# Cleanup work directory
		rm -rf work out log.txt
	fi
	inform "Generating .config"

	# Make .config
	muke "$DFCF"

	if [[ ! -z $BASE && $GENERATE != 1 ]]; then
		if [[ -f "arch/arm64/configs/vendor/${CODENAME,,}.config" ]]; then
			muke "$DFCF" "vendor/${CODENAME,,}.config"
		else
			inform "vendor/${CODENAME,,}.config not found, skipping device config fragment merge"
		fi
	fi
}

config_regenerator()
{
	config_generator

	inform "Regenerating defconfig"

	muke savedefconfig

	cat work/defconfig >arch/arm64/configs/"$DFCF"

	success "Regeneration completed"
}

mod_builder()
{
	if [[ $EMOD == "" ]]; then
		error "obj not defined"
	fi

	config_generator

	inform "Building $EMOD"
	cd $EMOD
	muke -C $KERNEL_DIR M="$EMOD" INSTALL_MOD_PATH="$KERNEL_DIR/work/modules" modules
	muke -C $KERNEL_DIR M="$EMOD" INSTALL_MOD_PATH="$KERNEL_DIR/work/modules" modules_install
	cd $KERNEL_DIR

	exit 0
}

obj_builder()
{
	if [[ $OBJ == "" ]]; then
		error "obj not defined"
	fi

	config_generator

	inform "Building $OBJ"
	muke "$OBJ"
	if [[ "$DTB_ZIP" != "1" ]]; then
		exit 0
	fi
}

dtb_zip()
{
	obj_builder
	source work/.config
	if [[ ! -d $AK3_DIR ]]; then
		error 'Anykernel is not present. Cannot zip!'
	fi
	if [[ ! -d "$KERNEL_DIR/out" ]]; then
		mkdir "$KERNEL_DIR"/out
	fi
	cp "$DTB_PATH"/*.dtb "$AK3_DIR"/dtb
	cp "$DTB_PATH"/*.img "$AK3_DIR"/
	ak3_pack "${CODENAME}-$(echo "$CONFIG_LOCALVERSION" | cut -c 2-)-dtbs-only"
	ak3_reset
	success "dtbs zip built"
}

kernel_builder()
{
	inform "Cleaning work directory, please wait...."
	muke -s clean mrproper
	rm -rf out work log.txt

	config_generator

	# Build Start
	BUILD_START=$(date +"%s")

	source work/.config
	KERNEL_VERSION="$(muke kernelrelease -s)"

	# Compile
	make "${MAKE_ARGS[@]}" 2>&1 | tee log.txt
	if [[ $CONFIG_MODULES == "y" ]]; then
		muke 'modules_install' INSTALL_MOD_STRIP=1 INSTALL_MOD_PATH="modules"
	fi

	# Build End
	BUILD_END=$(date +"%s")

	DIFF=$(("$BUILD_END" - "$BUILD_START"))

	zipper
}

zipper()
{
	TARGET="$(muke image_name -s)"

	if [[ ! -f ${KERNEL_DIR}/work/${TARGET} ]]; then
		[[ -n "$CHATID" && -n "$TOKEN" ]] && tg_post_build "${KERNEL_DIR}/log.txt" "Build failed!!"
		error 'Kernel image not found'
	else
		[[ -n "$CHATID" && -n "$TOKEN" ]] && tg_post_build "${KERNEL_DIR}/log.txt" "Compiled kernel successfully!!"
	fi

	if [[ ! -d $AK3_DIR ]]; then
		error 'Anykernel not present cannot zip'
	fi

	if [[ ! -d "$KERNEL_DIR/out" ]]; then
		mkdir "$KERNEL_DIR"/out
	fi

	# Making sure everything is ok before making zip
	ak3_reset

	cp "$KERNEL_DIR"/work/"$TARGET" "$AK3_DIR"
	cp "$DTB_PATH"/*.dtb "$AK3_DIR"/dtb
	cp "$DTB_PATH"/*.img "$AK3_DIR"/

	if [[ $CONFIG_MODULES == "y" ]]; then
		MOD_PATH="work/modules/lib/modules/${KERNEL_VERSION}"
		sed -i 's/\(kernel\/[^: ]*\/\)\([^: ]*\.ko\)/\/vendor\/lib\/modules\/\2/g' "$MOD_PATH"/modules.dep
		sed -i 's/.*\///g' "$MOD_PATH"/modules.order
		cp $(find "$MOD_PATH" -name '*.ko') "$AKVDR"/
		cp "$MOD_PATH"/modules.{alias,dep,softdep} "$AKVDR"/
		cp "$MOD_PATH"/modules.order "$AKVDR"/modules.load
	fi

	LAST_COMMIT=$(git show -s --format=%s)
	LAST_HASH=$(git rev-parse --short HEAD)

	ak3_pack "${CODENAME}-$(echo "$CONFIG_LOCALVERSION" | cut -c 2-)"

	[[ -n "$CHATID" && -n "$TOKEN" ]] && tg_post_build "${KERNEL_DIR}"/out/*.zip

	if [[ -n "$CHATID" && -n "$TOKEN" ]]; then
		tg_post_msg "
		========= aospa =========
		Compiler: <code>${C_NAME}</code>
		Linux Version: <code>${KERNEL_VERSION}</code>
		Developer: <code>${KBUILD_USER}</code>
		Device: <code>${DEVICENAME}</code>
		Codename: <code>${CODENAME}</code>
		Build Date: <code>$(date +"%Y-%m-%d %H:%M")</code>
		Build Duration: <code>$((DIFF / 60)).$((DIFF % 60)) mins</code>
		Last Commit Name: <code>${LAST_COMMIT}</code>
		Last Commit Hash: <code>${LAST_HASH}</code>
		"
	fi

	success "build completed in $((DIFF / 60)).$((DIFF % 60)) mins"
}


## Repack already-built kernel outputs (Image + dtb + dtbo) into an
## AnyKernel3 zip and push it to Telegram. This does NOT run a build and does
## NOT touch modules / vendor_dlkm - it only repacks whatever Image, *.dtb and
## dtbo.img are already sitting in the work/output dirs from a prior build.
repack_zip()
{
	if [[ ! -d $AK3_DIR ]]; then
		error 'AnyKernel3 not present. Cannot repack.'
	fi

	if [[ -z $CODENAME ]]; then
		error 'Codename not present, pass --device= before --repack'
	fi

	local KIMAGE="${TARGET:-Image}"
	local KIMAGE_PATH="${KERNEL_DIR}/work/${KIMAGE}"
	local DTBO_IMG="${KERNEL_DIR}/work/arch/arm64/boot/dtbo.img"

	[[ -f ${KIMAGE_PATH} ]] || error "Kernel image not found: ${KIMAGE_PATH} (build it first)"

	inform "Repacking exported images into an AnyKernel3 zip"

	# Reset the AnyKernel3 workdir before dropping new files into it
	ak3_reset

	# Kernel image
	cp "${KIMAGE_PATH}" "${AK3_DIR}"

	# Device tree blobs
	if compgen -G "${DTB_PATH}/*.dtb" >/dev/null; then
		cp "${DTB_PATH}"/*.dtb "${AK3_DIR}"/dtb
	else
		inform "No *.dtb found in ${DTB_PATH}, skipping dtb copy"
	fi

	# dtbo.img - check the normal build location first, fall back to DTB_PATH
	if [[ -f ${DTBO_IMG} ]]; then
		cp "${DTBO_IMG}" "${AK3_DIR}"
	elif compgen -G "${DTB_PATH}/*.img" >/dev/null; then
		cp "${DTB_PATH}"/*.img "${AK3_DIR}"
	else
		inform "No dtbo.img found, skipping dtbo copy"
	fi

	[[ -f work/.config ]] && source work/.config

	ak3_pack "${CODENAME}-$(echo "${CONFIG_LOCALVERSION:-$CODENAME}" | cut -c 2-)-repack"
	ak3_reset

	if [[ -n "$CHATID" && -n "$TOKEN" ]]; then
		tg_post_build "${KERNEL_DIR}"/out/*.zip "Repacked kernel (Image + dtb + dtbo, no modules) for ${DEVICENAME:-$CODENAME}"
	fi

	success "repack completed, zip in ${KERNEL_DIR}/out"
}

## COMMAND_MODE
if [[ -z $* ]]; then
	usage
fi
for arg in "$@"; do
	case "${arg}" in
		"--compiler="*)
			COMPILER=${arg#*=}
			COMPILER=${COMPILER,,}
			if [[ -z "$COMPILER" ]]; then
				usage
				break
			fi
			;&
		"--compiler32="*)
			COMPILER32=${arg#*=}
			COMPILER32=${COMPILER32,,}
			if [[ -z "$COMPILER32" ]]; then
				COMPILER32="clang"
			fi
			compiler_setup
			;;
		"--device="*)
			CODE_NAME=${arg#*=}
			case $CODE_NAME in
				Spacewar)
					DEVICENAME='Nothing Phone 1'
					CODENAME='Spacewar'
					BASE='lahaina'
					SUFFIX='qgki'
					TARGET='Image'
					;;
				*)
					error 'device not supported'
					;;
			esac
			;;
		"--dtb_zip")
			DTB_ZIP=1
			;&
		"--dtbs")
			OBJ=dtbs
			dtb_zip
			;;
		"--mod="*)
			EMOD=${arg#*=}
			mod_builder
			;;
		"--obj="*)
			OBJ=${arg#*=}
			obj_builder
			;;
		"--regen")
			GENERATE=1
			config_regenerator
			;;
		"--repack")
			repack_zip
			;;
	esac
done

kernel_builder