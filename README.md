# uefi-tb365fc

UEFI firmware for the **Lenovo Xiaoxin Pad 12.1 (2025), Lenovo TB365FC**
(MediaTek MT6835 / Dimensity 6300 family).

The firmware **builds and boots on the tablet**. The stock LK loads it from
`boot_a` as the kernel of an Android boot v4 image, decompresses it to
`kernel_addr` 0x40000000 and jumps into the SEC entry. On the development
unit it clears the LK scanout, shows the Project Silicium logo, then the
UniXNU menu (Return to firmware / Restart / Power off / Tools) at
1600x2560. Tools → Advanced Logger renders the in-RAM log as QR pages with
five speed gears; UART0 carries the same log.

The XNU/DarwinBoot handoff and the ADT payload are **not** implemented yet.
The device repository keeps `example = true` until the remaining inputs
(APFS Data volume UUID, unique chip ID) are measured.

The tree is derived from
[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
through the local `uefi-camellia` reference checkout. The platform package
under `Platforms/Lenovo/tb365fcPkg` is based on `uefi-merlinx`
(`merlinxPkg`, MT6768 direct boot), and `Silicon/MediaTek/MT6835Pkg` is a
renamed copy of `MT6833Pkg` whose tables were corrected against the stock
TB365FC firmware and the live device.

## State

| Area | State |
| --- | --- |
| EDK II package / DSC / FDF | builds DEBUG from the shared MT6835 profile; FD is 4 MiB at 0x40000000 |
| Boot path | Android boot v4, gzip kernel = FD; LK decompresses to kernel_addr 0x40000000; flashed to `boot_a` on the development unit |
| Memory map | FD 0x40000000-0x40400000, primary stack at 0x40400000; the LK reservations (DTB 0x47C80000, ramdisk 0x66F00000) are left alone |
| DRAM | 8 GiB from 0x40000000; `MemoryMapLib` keeps the placeholder split because the stock DTB only carries a 1 GiB LK stub |
| Display | stock LK scanout 1600x2560x32 at 0xFCF2B000 reused through `SimpleFbDxe`; logo and menu verified on hardware, panel is BOE HX83122B |
| Storage | UFS 2.2 (`PcdStorageIsEMMC = FALSE`); the UEFI does not drive the UFS yet |
| PMIC / keypad | MT6377; Volume Up/Down and Power reach the menu and the logger viewer |
| Device tree | built-in tb365fc tree published to the OS (`PcdDefaultDtPref`); ADT/device-tree handoff pending |
| Advanced Logger | RAM ring with a UART0 hardware-port mirror; QR export from the Tools menu |

## Build

The supported path is the UniXNU driver, which reads `board.toml` from the
device repository and applies `[firmware].patches` to the shared edk2
checkout for the build:

```bash
cd /path/to/UniXNU-tb365fc
./unixnu firmware --uefi ../uefi-tb365fc
```

The manual path below is the one validated on the development unit (WSL,
clang/LLD). The five Mu base-core patches are part of the build, exactly as
`build_uefi.py` applies them; a Windows checkout needs `--ignore-whitespace`
because the Mu blobs are checked out with CRLF:

```bash
git submodule update --init
bash ./setup_env.sh -p apt

for p in AsmMacroLib Auth-Service Boot-Manager Timer Usb-Bus; do
  git -C Mu_Basecore apply --ignore-whitespace Resources/MuPatches/$p.patch
done

python3 Platforms/Lenovo/tb365fcPkg/DeviceBuild.py TARGET=DEBUG ENABLE_SECUREBOOT=0 \
  FD_BASE=0x40000000 FD_SIZE=0x400000 FD_BLOCKS=0x400 DEVICE_MODEL=0
```

The FD lands in `Build/tb365fcPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`.
`build_uefi.py` is kept as a standalone helper that applies the same patches
and can pack a boot image for a given `-d tb365fc`.

## Boot image

```bash
python3 Resources/Scripts/mkbootimg.py \
  --kernel <(gzip -c Build/tb365fcPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd) \
  --ramdisk Resources/ramdisk --header_version 4 -p 4096 -o image.img
fastboot flash boot_a image.img
```

`Resources/Configs/tb365fc.toml` holds the geometry read back from the stock
`vendor_boot` header: header v4, kernel_addr 0x40000000, tags/DTB at
0x47C80000, ramdisk at 0x66F00000, cmdline `bootopt=64S3,32N2,64N2`. The
stock `init_boot` and `vendor_boot` stay untouched: LK loads their ramdisks
and DTB before jumping into the FD. `Resources/ramdisk` is a 1 MiB FAT12
volume labelled `UNIXNU` for the future payload.

## Plan A and Plan B

Plan A (implemented) boots the UEFI FD directly from LK, the merlinx route:
the FD is packed as a compressed v4 kernel with no load addresses, so LK
places it at `kernel_addr` and no shim is needed.

Plan B (fallback, unused) is the Linux preboot hot-jump used by camellia.
The reference userland is kept unmodified under `BootImage/` and `module/`
(see `module/README.md`). It is not part of the default build and has not
been adapted for the TB365FC kernel.

## Layout

```text
Platforms/Lenovo/tb365fcPkg/   platform DSC/FDF, memory map, boot memory, keypad
Silicon/MediaTek/MT6835Pkg/    MT6835 clock/GPIO/MSDC/PMIC/PlatformSec libraries
Silicon/MediaTek/MediaTekPkg/  shared MediaTek DXE drivers (PMIC, MSDC, GPIO)
Silicon/Silicium/              Mu-Silicium core packages
Resources/Configs/tb365fc.toml FD and boot image geometry
Resources/DTBs/                DTS sources for the .dtb the boot image needs
Resources/MuPatches/           Mu base-core patches applied at build time
Resources/ramdisk              1 MiB FAT12 payload volume label UNIXNU
BootShim/                      direct-boot shim, unused by the v4 image
module/, BootImage/            camellia Plan B reference assets (not built)
```

## Safety

- `example = true` in the device repository blocks `unixnu build` and
  `unixnu bootimg` until the remaining REQUIRED values are measured.
- The image was flashed to `boot_a` of the development unit only. Treat the
  memory map and panel configuration as measured for that unit; verify them
  again before flashing another device.
- The stock firmware dump, partition images and any device-identifying data
  (serial, IMEI, MAC) must not be committed here.

## Upstream and pinning

Submodules follow `.gitmodules` (Mu `release/202608`, OpenSSL
`67b5686b...`). `Mu_Basecore` is pinned to the upstream commit
`3c9c7b2e21`; the five TB365FC fixes are applied from `Resources/MuPatches`
at build time, so a fresh clone needs no private fork. See `PORTING.md` for
the bring-up notes.
