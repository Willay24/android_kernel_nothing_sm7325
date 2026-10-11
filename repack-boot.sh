#!/bin/bash
# Build flashable artifacts from a compiled kernel Image.
#
#   ./repack-boot.sh v4      -> Spacewar-boot-resukisu.img  + Spacewar-ReSukiSU-SUSFSv2-<kver>.zip
#   ./repack-boot.sh v155    -> Spacewar-boot-sukisu.img    + Spacewar-SukiSU-SUSFS-<kver>.zip
#
# Kernel source:   build/Image   (copied there by build_5.4_*.sh)
# Base ramdisk:    stock/<stock boot image>  (provide your own extraction of the
#                  Nothing OS 3.2 boot partition; it is NOT distributed here).
#                  If it is missing, the AnyKernel3 zip is still produced and the
#                  boot.img step is skipped (CI use).
# AnyKernel3:      cloned on demand from osm0sis/AnyKernel3 into build/ak3
#
# Env overrides:
#   STOCK_BOOT=/path/to/stock_boot.img
#   MAGISKBOOT=/path/to/magiskboot
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VARIANT="${1:-v4}"
IMAGE="$ROOT/build/Image"
STOCK="${STOCK_BOOT:-$ROOT/stock/Spacewar_V3.2_stock_boot.img}"
KVER="$(make -s -C "$ROOT" kernelversion 2>/dev/null || echo 5.4.289)"
WORK="$(mktemp -d /tmp/spacewar-repack.XXXXXX)"

AK3_SRC="$ROOT/build/ak3/AnyKernel3"

case "$VARIANT" in
    v4)   BOOT="$ROOT/Spacewar-boot-resukisu.img"; ZIP="$ROOT/Spacewar-ReSukiSU-SUSFSv2-${KVER}.zip" ;;
    v155) BOOT="$ROOT/Spacewar-boot-sukisu.img";   ZIP="$ROOT/Spacewar-SukiSU-SUSFS-${KVER}.zip" ;;
    *) echo "usage: $0 {v4|v155}" >&2; exit 1 ;;
esac

[ -f "$IMAGE" ] || { echo "missing $IMAGE (build first)" >&2; exit 1; }

if [ ! -d "$AK3_SRC" ]; then
    mkdir -p "$(dirname "$AK3_SRC")"
    git clone --depth 1 https://github.com/osm0sis/AnyKernel3.git "$AK3_SRC"
fi

# AnyKernel3 ships ARM magiskboot for on-device use; we need a host binary.
find_magiskboot() {
    if [ -n "${MAGISKBOOT:-}" ] && [ -x "$MAGISKBOOT" ]; then echo "$MAGISKBOOT"; return 0; fi
    if [ -x "$ROOT/tools/magiskboot" ]; then echo "$ROOT/tools/magiskboot"; return 0; fi
    if command -v magiskboot >/dev/null 2>&1; then command -v magiskboot; return 0; fi
    local apk="${MAGISK_APK:-$ROOT/build/magisk.apk}" out="$ROOT/build/tools/magiskboot"
    if [ ! -x "$out" ]; then
        mkdir -p "$(dirname "$out")"
        if [ ! -f "$apk" ]; then
            url=$(curl -s https://api.github.com/repos/topjohnwu/Magisk/releases/latest \
                | grep -o '"browser_download_url": *"[^"]*Magisk-[^"]*\.apk"' | head -n1 | cut -d'"' -f4)
            [ -n "$url" ] || { echo "cannot resolve Magisk APK URL" >&2; return 1; }
            curl -Lf -o "$apk" "$url" || return 1
        fi
        python3 - "$apk" "$out" <<'PYEOF' || return 1
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for name in ("lib/x86_64/libmagiskboot.so", "lib/arm64-v8a/libmagiskboot.so"):
    try:
        open(sys.argv[2], "wb").write(z.read(name)); break
    except KeyError:
        pass
PYEOF
        chmod +x "$out"
    fi
    echo "$out"
}

if [ -f "$STOCK" ]; then
    echo "[1/4] unpacking stock boot image"
    MAGISKBOOT="$(find_magiskboot)" || { echo "no host magiskboot available" >&2; exit 1; }
    cp "$STOCK" "$WORK/boot.img"
    ( cd "$WORK" && "$MAGISKBOOT" unpack boot.img >/dev/null )
    echo "[2/4] replacing kernel"
    cp "$IMAGE" "$WORK/kernel"
    ( cd "$WORK" && "$MAGISKBOOT" repack boot.img new-boot.img >/dev/null )
    cp "$WORK/new-boot.img" "$BOOT"
    BOOT_OUT="$BOOT"
else
    echo "[1-2/4] no stock boot image at $STOCK - skipping boot.img (AnyKernel3 only)"
    BOOT_OUT=""
fi

echo "[3/4] building AnyKernel3 zip"
cp "$IMAGE" "$AK3_SRC/Image"
cp "$ROOT/ak3/anykernel.sh" "$AK3_SRC/anykernel.sh"
( cd "$AK3_SRC" && rm -f "$ZIP" && zip -r9 "$ZIP" . -x "*.git*" >/dev/null )

rm -rf "$WORK"
echo "[4/4] done:"
[ -n "$BOOT_OUT" ] && ls -lh "$BOOT_OUT"
ls -lh "$ZIP"
