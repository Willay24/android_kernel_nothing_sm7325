### AnyKernel3 Ramdisk Mod Script
## Suvkernel @KVER@ for redwood, for ROMs already on the 6.18 kernel
## build.sh fills in @KREL@, @KVER@ and @DATE@

### AnyKernel setup
properties() { '
kernel.string=@KREL@ for redwood (6.18 ROMs) @DATE@
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=redwood
device.name2=redwoodin
supported.versions=
supported.patchlevels=
'; }

BLOCK=boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;
NO_BLOCK_DISPLAY=1;

. tools/ak3-core.sh;

REL=@KREL@;

ui_print " " "@KREL@ for redwood (@DATE@)" " ";
ui_print "- Reading the current boot image";
dump_boot;

if ! grep -aq "Linux version 6\.18\." $SPLITIMG/kernel 2>/dev/null; then
  abort "! This ROM is not on the 6.18 kernel. Flash a 6.18 ROM first.";
fi;

# magiskboot takes an FDT-looking blob inside the Image for an appended dtb
# and would append it again after the new kernel; the dtb is in vendor_boot.
rm -f $SPLITIMG/kernel_dtb;

ui_print "- Installing $REL modules into the boot ramdisk";
rm -rf $RAMDISK/lib/modules/$REL;
mkdir -p $RAMDISK/lib/modules/$REL;
cp -f $AKHOME/modules618/* $RAMDISK/lib/modules/$REL/;
chmod 755 $RAMDISK/lib $RAMDISK/lib/modules $RAMDISK/lib/modules/$REL;
chmod 644 $RAMDISK/lib/modules/$REL/*;

ui_print "- Writing the kernel to boot";
repack_ramdisk;
flash_boot;

ui_print "- Writing dtbo";
flash_generic dtbo;

ui_print " " "Done. Root (KernelSU/Magisk) in this boot is kept.";
