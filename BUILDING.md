# Building the redwood 6.18 kernel

`build.sh` builds everything that makes up this kernel on any x86_64 Linux
machine, from a clean clone:

- the kernel `Image` and the 238 in-tree modules,
- the 73 vendor modules from
  [redwood-6.18-modules](https://github.com/suvojeet-sengupta/redwood-6.18-modules),
- `dtbo.img` from
  [redwood-6.18-devicetrees](https://github.com/suvojeet-sengupta/redwood-6.18-devicetrees),
- a flashable AnyKernel3 zip with all of the above.

It downloads its own toolchain, pinned to the exact versions the published
kernel is built with, so nothing has to be installed system-wide apart from a
few common build packages.

## What you need

- x86_64 Linux (any distribution).
- About 25 GB of free disk: 4 GB toolchain, 15 GB build output.
- 8 GB of RAM or more. With less, lower `JOBS` (see below).
- These packages:

  ```sh
  # Debian / Ubuntu
  sudo apt install git make bc bison flex python3 perl rsync cpio curl zip unzip \
      build-essential kmod libssl-dev libelf-dev zlib1g-dev

  # Arch
  sudo pacman -S --needed git make bc bison flex python perl rsync cpio curl zip unzip \
      base-devel kmod openssl libelf zlib

  # Fedora
  sudo dnf install git make bc bison flex python3 perl rsync cpio curl zip unzip \
      gcc kmod openssl-devel elfutils-libelf-devel zlib-devel
  ```

`build.sh` checks for all of them first and tells you what is missing.

## Quick start

```sh
git clone --depth=1 https://github.com/suvojeet-sengupta/redwood-6.18-kernel
cd redwood-6.18-kernel
./build.sh
```

The first run downloads the toolchain and takes the longest. When it's done,
the output is in `../redwood-618-build/dist`:

| File | What it is |
|---|---|
| `Suvkernel-<version>-redwood-<date>.zip` | Flashable zip: kernel, all modules, dtbo |
| `Image` | The kernel |
| `dtbo.img` | redwood's board overlay |
| `modules/` | All 311 modules, stripped, with `modules.load` and the `depmod` files |

## Steps

`./build.sh` with no arguments runs every step in order. Each step can also
run on its own, for example to rebuild only the vendor modules after
changing one:

| Step | What it does |
|---|---|
| `tools` | Downloads the toolchain into `toolchain/`. Skips what is already there |
| `kernel` | Configures with `gki_defconfig` + `vendor/redwood.config`, builds `Image` and the in-tree modules |
| `modules` | Clones redwood-6.18-modules (once), builds the vendor modules, collects all 311 into `dist/modules` |
| `dtbo` | Clones redwood-6.18-devicetrees (once), builds `dist/dtbo.img` |
| `zip` | Packs `Image`, `dtbo.img` and the modules into the AnyKernel3 zip |
| `clean` | Deletes the build output. The toolchain and the cloned sources stay |

```sh
./build.sh kernel modules zip   # several steps, in this order
./build.sh clean                # start over without downloading again
```

`make` only rebuilds what changed, so running `./build.sh` again after an
edit is quick.

## Options

Set these as environment variables, for example
`JOBS=4 ./build.sh kernel`.

| Variable | Default | What it sets |
|---|---|---|
| `WORK` | `../redwood-618-build` | Where everything goes: toolchain, sources, build output |
| `TC` | `$WORK/toolchain` | Toolchain directory. Point several work directories at one to share it |
| `JOBS` | number of CPUs | Parallel build jobs. Use 4 on 8 GB of RAM |
| `BUILD_NUMBER` | `1` | The `#N` in `uname -v` |
| `BUILD_USER`, `BUILD_HOST` | `build-user`, `build-host` | The `user@host` in `/proc/version` |
| `MODULES_REF`, `DT_REF` | `main` | Branch of the modules and devicetrees repositories to clone |
| `CLANG_DIR` | downloaded | Use an existing `clang-r596125` directory instead of downloading one |
| `PAHOLE` | downloaded | Use an existing pahole instead of downloading one |

The vendor modules and devicetrees are cloned into
`$WORK/redwood-6.18-modules` and `$WORK/redwood-6.18-devicetrees`. If those
directories already exist, `build.sh` uses them as they are and never pulls
or resets them, so you can work on them there.

## Layout of the work directory

```
redwood-618-build/
  toolchain/                 clang, rust, bindgen, pahole, mkdtboimg, AnyKernel3
  out/                       kernel build (O=)
  redwood-6.18-kernel  ->    symlink to this kernel tree
  redwood-6.18-modules/      vendor module sources
  redwood-6.18-devicetrees/  devicetree sources
  vb/                        vendor module builds, one directory and log per tree
  out-dt/                    devicetree build
  logs/                      kernel.log, dt.log, depmod.log
  dist/                      the results
```

## Toolchain

All pinned, all downloaded by `./build.sh tools`:

| Tool | Version | From |
|---|---|---|
| Clang | r596125 (22.0.2) | AOSP `platform/prebuilts/clang/host/linux-x86` |
| Rust | 1.88.0 with `rust-src` and `rustfmt` | rustup, installed inside `toolchain/` |
| bindgen | 0.72.1 | `cargo install bindgen-cli` |
| pahole | v1.25 | AOSP `kernel/prebuilts/build-tools` |
| mkdtboimg | — | AOSP `platform/system/libufdt` |
| AnyKernel3 | — | osm0sis/AnyKernel3 |

Why these exact versions:

- **Rust** must be available when the kernel is configured. Without it,
  Kconfig silently drops `CONFIG_RUST`, and ashmem and the Rust Binder go
  with it, which leaves Android without `/dev/ashmem`. `build.sh` stops with
  an error if that happens.
- **pahole 1.25 from Android's build-tools**. Upstream pahole 1.25 fails to
  generate this kernel's BTF ("per-CPU variables limit").
- **Clang r596125** is what Android builds `android17-6.18` with, and the
  prebuilt kernel was built with it.

Your own rustup or `~/.cargo` aren't touched: Rust lives in
`toolchain/rustup` and `toolchain/cargo`.

## Flashing the zip

The zip is for a ROM that already runs a 6.18 kernel (one built with the
[prebuilt](https://github.com/suvojeet-sengupta/device_xiaomi_redwood-kernel-6.18)).
It refuses to install on a 5.4 or 6.12 ROM.

1. Copy the zip to the phone.
2. Flash it in recovery, or with an app that flashes AnyKernel3 zips.
3. Reboot.

It replaces the kernel in `boot` and puts the matching modules in the boot
ramdisk, keeping root, then writes `dtbo`. Keep a backup of your `boot` and
`dtbo` before you flash a kernel you built yourself.

## Building by hand

What `build.sh kernel` runs, if you want to do it yourself:

```sh
export PATH=<clang-r596125>/bin:$HOME/.cargo/bin:$PATH
export LIBCLANG_PATH=<clang-r596125>/lib
M="make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 LOCALVERSION= PAHOLE=<pahole>"

$M gki_defconfig vendor/redwood.config
$M -j$(nproc) Image modules
```

The vendor module scripts are in `redwood-6.18-modules/bringup`, see its
README.

## Things to know

- **Rebuild every module with the kernel.** The vermagic includes the exact
  kernel version, and every sublevel update (6.18.x) changes it. A new
  `Image` with old modules doesn't boot.
- **Build `Image` and the in-tree modules together.** pahole's BTF isn't
  the same from one link to the next, and the in-tree modules carry BTF split
  against that exact `vmlinux`. `build.sh kernel` always builds both in one
  go. The vendor modules use distilled BTF and don't care.
- **The modules aren't signed.** Same as the published prebuilt.
- **`sync_fence` and `hw_fence` fail to build.** They're in mm-drivers but
  yupik doesn't use them, so that's expected.
- `modules.list` and `modules.load` in [`redwood/`](redwood) say which
  modules ship and in which order they load. If you add a module, add it to
  both.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Kconfig turned CONFIG_RUST off` | Run `./build.sh tools` again. Check with `make O=<work>/out LLVM=1 rustavailable` |
| `N modules were not built: …` | Open the logs in `<work>/vb/*.log` and `<work>/vb/rw/*.log` and look for `error:` |
| `modules with unresolved symbols` | A vendor module needs something the kernel doesn't export. Usually the modules repo is on another branch than the kernel |
| Killed while linking, or the machine freezes | Not enough RAM: `JOBS=2 ./build.sh` |
| Clang download stalls | Delete `toolchain/clang` and run `./build.sh tools` again |
| Phone doesn't boot | Flash your backed up `boot` and `dtbo` from recovery or fastboot |
