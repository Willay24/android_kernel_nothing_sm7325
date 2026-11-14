# Linux 6.18 for redwood (SM7325)

Kernel source for the **Redmi Note 12 Pro 5G / POCO X5 Pro 5G** (`redwood`,
Snapdragon 778G, SM7325 "yupik"). It brings Qualcomm's lahaina/yupik
platform from the 6.12 CLO kernel onto Android Common Kernel 6.18.

| | |
|---|---|
| Version | Linux 6.18.39 (`6.18.39-Suvkernel`) |
| Base | ACK `android17-6.18`, with `android17-6.18-lts` merged in |
| Platform | Qualcomm common kernel delta and soc-repo drivers, carried over from 6.12 |
| Layout | GKI: generic code built in, SoC and board drivers as modules |
| Config | `gki_defconfig` + `vendor/redwood.config` |
| Toolchain | Android Clang 22.0.2 (r596125), Rust 1.88.0, bindgen 0.72.1 |
| Runs | PixelOS 17 and AOSPA, as a daily driver |

## Repositories

| Repository | Contents |
|---|---|
| **redwood-6.18-kernel** (this one) | The kernel |
| [redwood-6.18-modules](https://github.com/suvojeet-sengupta/redwood-6.18-modules) | Vendor and out-of-tree modules: display, GPU, audio, camera, video, Wi-Fi, data, securemsm, redwood drivers, and their build scripts |
| [redwood-6.18-devicetrees](https://github.com/suvojeet-sengupta/redwood-6.18-devicetrees) | The SoC and redwood devicetrees (dtb and dtbo), and the porting status |
| [device_xiaomi_redwood-kernel-6.18](https://github.com/suvojeet-sengupta/device_xiaomi_redwood-kernel-6.18) | Ready-made Image, modules, dtb and dtbo for ROM builds |
| [device_xiaomi_redwood](https://github.com/suvojeet-sengupta/device_xiaomi_redwood/tree/seventeen) | Android device tree, PixelOS 17 (`seventeen`) |
| [device_xiaomi_redwood_clo](https://github.com/suvojeet-sengupta/device_xiaomi_redwood_clo/tree/6.18) | Android device tree, AOSPA (`6.18`) |
| [vendor_xiaomi_redwood_pixelos](https://github.com/suvojeet-sengupta/vendor_xiaomi_redwood_pixelos/tree/seventeen) | Vendor blobs for the PixelOS tree |

## What's on top of ACK

**Platform**
- Qualcomm's common kernel changes and soc-repo drivers from 6.12, merged into 6.18's build
- Generic drivers restored where Qualcomm's tree replaced them
- `vendor/redwood.config`, built from Qualcomm's `lahaina_perf` config
- The `android17-6.18` GKI KMI kept, so KernelSU's LKM loads

**Boot, memory and IOMMU**
- 6.18 early boot fixed for yupik
- arm-smmu: domain page sizes set for 6.18, and the TLB invalidated on unmap
- Reserved IOVA regions translated the 6.12 way again. Without that, the WPSS firmware crashed on its first access
- Qualcomm fastmap DMA ops. Without them, UFS reads returned stale data
- zram recompresses idle pages with zstd

**Power and scheduling**
- WALT removed: plain EAS with schedutil
- Fast PELT and the TEO cpuidle governor by default
- RT tasks get a 25% uclamp floor
- `msm_performance` sets its frequency floors under EAS
- The CPU cluster power domain syncs (`qcom,lpm-cluster-dev`), so the SoC reaches deep sleep
- UCSI handles connector changes outside the RX worker, so USB doesn't drop on plug-in

**Audio, data and Android**
- The `snd_pcm_check_no_buffer` vendor hook, dropped in 6.18, is back. Voice calls need it
- The ASoC "no BE" check moved back from open to prepare. The sound trigger HAL opens its PCM before routing it
- qrtr: `vendor_qrtr` can bind the control ports
- SELinux: optional fallback to the pre-xperm netlink check (`redwood.nlmsg_legacy`), for vendor policy that lacks the 6.18 `allowxperm` rules
- Rust enabled for ashmem and the Rust Binder driver (`binder.impl=rust`)
- BTF included

**Bring-up aids**
- `redwood_splashcon`: an optional console on the boot splash (`redwood_splashcon.enable=1`)

`git log --author=suvojeet` lists every one of these commits.

## Building

Requirements:
- Android Clang r596125
- Rust 1.88.0 with `rust-src`, plus bindgen 0.72.1
- pahole 1.25 or newer

Rust has to be in `PATH` when the config is made. Otherwise Kconfig turns
`CONFIG_RUST` off without a warning, and ashmem and the Rust Binder go with
it, which leaves Android with no `/dev/ashmem`.

```sh
export PATH=<clang>/bin:$HOME/.cargo/bin:$PATH
export LIBCLANG_PATH=<clang>/lib
M="make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 LOCALVERSION= PAHOLE=<pahole>"

$M gki_defconfig vendor/redwood.config
$M -j$(nproc) Image modules
$M INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
```

The defconfig gives exactly the configuration of the published prebuilt.
`out/arch/arm64/boot/Image` is the kernel. Build the vendor modules against
the same `out/` with
[redwood-6.18-modules](https://github.com/suvojeet-sengupta/redwood-6.18-modules),
and the dtb and dtbo with
[redwood-6.18-devicetrees](https://github.com/suvojeet-sengupta/redwood-6.18-devicetrees).

To build a ROM, use the
[prebuilt](https://github.com/suvojeet-sengupta/device_xiaomi_redwood-kernel-6.18)
and nothing has to be compiled.

## Updating

`main` follows `android17-6.18` and `android17-6.18-lts` from
`https://android.googlesource.com/kernel/common`. The stable sublevels
(6.18.x) arrive through `-lts`.

Every new sublevel changes the module vermagic, so all of the modules (in-tree
and vendor) have to be rebuilt along with the Image. A new Image with old
modules won't boot.

## Status

What works and what doesn't is tracked in
[redwood-6.18-devicetrees](https://github.com/suvojeet-sengupta/redwood-6.18-devicetrees#status).
In short:
- Working: display, GPU, touch, audio and calls, camera, video, Wi-Fi, Bluetooth, mobile data on both SIMs, sensors, fingerprint, charging, deep sleep
- Kept off: CoreSight, the Qualcomm watchdog and `tz_log`

## Credits

- Google, for the Android Common Kernel
- Qualcomm (CodeLinaro), for the lahaina/yupik platform
- The Linux kernel community

The original ACK contribution guide is in [README.ACK.md](README.ACK.md).

## License

GPL-2.0, like the Linux kernel. See [COPYING](COPYING).

Ported and maintained by Suvojeet Sengupta.
