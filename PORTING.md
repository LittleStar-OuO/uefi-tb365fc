# TB365FC firmware porting record

This file tracks what is still invented in the firmware tree. The device
repository (`UniXNU-tb365fc/porting.toml`) carries the stage-by-stage record;
this file only lists the firmware-side values that must change together.

## Source of every value

The stock ZUXOS 1.1.10.122 SP Flash Tool package was unpacked with the tools
in `UniXNU-tb365fc/tools`; nothing has been read from the tablet itself yet.

| Data | Origin | Status |
| --- | --- | --- |
| CPU MPIDRs, GIC frames, timer, UART | stock `vendor_boot` base DTB | recorded in `mt6835.dtsi` |
| DRAM base and size | `MemTotal` 7,991,068 kB and `/proc/iomem` ending at 0x23fffffff | measured; the stock DTB carries a 1 GiB LK stub |
| Android header geometry | stock `vendor_boot` header | measured (tags/DTB 0x47C80000, ramdisk 0x66F00000, cmdline `bootopt=64S3,32N2,64N2`, pages 4096) |
| FD base/size | Plan A convention (merlinx/camellia) | unverified on this LK |
| Panel | stock `dtbo` overlay | BOE HX83122B or Tianma NT36532, video mode, 16:10; resolution unverified |
| PMIC | stock `dtbo` overlay | MT6377 confirmed; driver is still the MT6358-era placeholder |
| Storage | stock base DTB and scatter | UFS host enabled, both MSDC hosts disabled; scatter has EMMC and UFS layouts; type unverified; publication still disabled |
| Device tree | stock DTBs plus hand-written skeleton | CPU/GIC/timer/touch/panel/PMIC recorded |

## Values that must be replaced

1. `Resources/Configs/tb365fc.toml`
   - `[uefi_fd] base` must equal `[boot_image] base + kernel_offset`.
   - `size` must equal the FDF FV size (`FD_SIZE` / `FD_BLOCKS`).
   - `pagesize`, `cmdline`, `dtb_offset`, `ramdisk_offset` and `tags_offset` are
     measured from the stock `vendor_boot` header; `header_version` is
     deliberately 2 because the stock v4 header carries no load addresses.
     Re-check all of them if the firmware package changes.
2. `Platforms/Lenovo/tb365fcPkg/tb365fc.dsc`
   - `PcdSystemMemoryBase/Size` now match the measured 8 GiB; the stack
     base/size, SMBIOS strings, framebuffer mode and `PcdStorageIsEMMC` still
     need measurement.
3. `Platforms/Lenovo/tb365fcPkg/Library/MemoryMapLib/MemoryMapLib.c`
   - Stack, FD and DXE heap carve-outs plus every MMIO window. GIC, MSDC-0 and
     MSDC Top-0 now match the stock DTB; the other windows are still the MT6833
     reference set and the TB365FC may move or resize any of them.
4. `Platforms/Lenovo/tb365fcPkg/Drivers/Tb365fcBootMemoryDxe/Tb365fcBootMemoryDxe.c`
   - `mAvailableMemory` is one contiguous range after the firmware windows and
     ends 0x80000 below the measured iomem end. The reserved-memory carveouts
     inside it are still not described. DarwinBoot receives this list, so an
     error here is fatal for the payload.
5. `Silicon/MediaTek/MT6835Pkg/Library/*`
   - Clock tree, GPIO numbering, PMIC wrapper and MSDC tables are still the
     MT6833 tables renamed to MT6835. Validate each one against the stock DTB
     before enabling the corresponding DXE driver.
6. `Resources/DTBs/*.dts`
   - CPU MPIDRs, `timebase-frequency` and GIC frames now match the stock DTB;
     the memory node and every node exported with `unixnu,adt-name` still have
     to be reconciled with the live tablet inventory.

## Extraction checklist

Run these on the tablet or with the official firmware package, then fill in
the tables above (see `UniXNU-tb365fc/tools/README.md`):

1. `adb shell getprop ro.board.platform`, `ro.product.device`,
   `ro.boot.hardware` and `ro.boot.hardware.sku`.
2. Dump and unpack the stock `boot.img`/`vendor_boot.img`; record the header
   geometry and the boot cmdline. Done for ZUXOS 1.1.10.122: v4 headers,
   geometry in `Resources/Configs/tb365fc.toml`.
3. Dump `dtbo`/`dtb` partitions, decompile them with `dtc -I dtb -O dts` and
   diff CPU, GIC, timer, memory and reserved-memory nodes against
   `Resources/DTBs/mt6835.dtsi`. CPU, GIC, timer, touch, panel and PMIC are
   done; the memory node is measured (8 GiB at 0x40000000) and the
   reserved-memory carveouts still need reconciliation with `iomem`.
4. Read the stock GPT (`sgdisk --print`) for the userdata window and the
   partition that holds the boot image.
5. Capture an LK/fastboot boot log to confirm the load addresses the
   bootloader uses and whether it enforces verified boot.
6. Identify the PMIC, storage controller (UFS vs MSDC) and panel from the
   stock DTB and kernel logs. Done: MT6377, BOE HX83122B or Tianma NT36532
   (video mode, resolution unverified). The stock DTB enables ufshci@112b0000
   and disables both MSDC hosts, so the storage controller is still open.

## Plan A / Plan B

Plan A is the merlinx route: LK loads the uncompressed UEFI FD at the
boot-image kernel address and jumps to it. `BootShim/` is only needed for
the Android boot image wrapper, not for a raw `fastboot boot` of the FD.

Plan B is the camellia hot-jump: boot the stock/vendor Linux first, load
`module/uefi_jump` and `module/drm_linear` from an initramfs (`BootImage/`),
then jump to a second FD copy at `0xA0000000` with the CPU stack moved to
`0xA0200000`. Those files are camellia-specific and are kept only as a
reference; if Plan A fails, adapt and validate them before use.
`Resources/MuPatches/Camellia-Essential.patch` (GIC redistributor wake-up,
DXE ordering) belongs to this route and is not applied by the UniXNU
firmware step.

## Mu patch application

`build_uefi.py` applies the five Plan A patches (`AsmMacroLib`, `Auth-Service`,
`Boot-Manager`, `Timer`, `Usb-Bus`) with `git apply --ignore-whitespace`: the
Mu blobs are checked out with CRLF while the patch files use LF, so a plain
`git apply` rejects every hunk. `Camellia-Essential.patch` is Plan B-only and
is deliberately left out of the list; `module/README.md` says the same.

## Before publication

- Pin the Mu-Silicium baseline, the exact submodule commits and the
  toolchain in `REPRODUCIBILITY` form.
- Remove any `REQUIRED` placeholder that was actually measured, and record
  the measurement command in `porting.toml`.
- Keep `example = true` in `UniXNU-tb365fc/board.toml` until a firmware
  build has been validated on hardware.
