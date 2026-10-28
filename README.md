# uefi-tb365fc

UEFI firmware for the **Lenovo Xiaoxin Pad 12.1 (2025), Lenovo TB365FC**
(MediaTek MT6835 / Dimensity 6300 family).

This is a **porting skeleton**, not a bootable firmware yet. The boot-image
addresses were measured from the stock ZUXOS 1.1.10.122 package, but every
memory window, panel mode, storage decision and the FD window itself that is
marked `REQUIRED` in the source still comes from the MT6833 family and has to
be confirmed on this tablet before the image is flashed.

The tree is derived from
[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
through the local `uefi-camellia` reference checkout. The platform package
under `Platforms/Lenovo/tb365fcPkg` is based on `uefi-merlinx`
(`merlinxPkg`, MT6768 direct boot), and `Silicon/MediaTek/MT6835Pkg` is a
renamed copy of `MT6833Pkg` with the hardware tables still unverified.

## State

| Area | State |
| --- | --- |
| EDK II package / DSC / FDF | present, builds from the shared MT6835 profile |
| Memory map (DXE heap, FD, stack) | placeholder, `MemoryMapLib.c` (FD window 0x40080000/0x300000 is the Plan A choice) |
| Usable DRAM ranges | one 8 GiB placeholder; the stock DTB only carries a 1 GiB LK stub, so the size is unverified |
| Display | SimpleFb placeholder; panel is BOE HX83122B or Tianma NT36532 (video mode), resolution unverified |
| Storage | MSDC path present but disabled (`PcdStorageIsEMMC = FALSE`); the stock tree confirms eMMC on MSDC_0 |
| PMIC / keypad | MT6377 per the stock overlay; the driver is still the MT6358-era placeholder |
| Device tree | `Resources/DTBs/tb365fc.dts` + `mt6835.dtsi`; CPU/GIC/timer measured, `.dtb` not generated |

## Build

The supported path is the UniXNU driver, which reads `board.toml`:

```bash
cd /path/to/UniXNU-tb365fc
./unixnu firmware --uefi ../uefi-tb365fc
```

`[firmware]` in `board.toml` supplies `PACKAGES_PATH`, the DSC and the FD
size check. The DSC is also buildable by hand from the EDK II shell:

```bash
git submodule update --init
bash ./setup_env.sh -p apt
build -p Platforms/Lenovo/tb365fcPkg/tb365fc.dsc -a AARCH64 -b DEBUG -t CLANGPDB \
  -D FD_BASE=0x40080000 -D FD_SIZE=0x300000 -D FD_BLOCKS=0x300
```

`build_uefi.py` is kept as a standalone helper; it additionally applies the
Mu patches under `Resources/MuPatches/` and can pack a boot image for a
given `-d tb365fc` once `Resources/DTBs/tb365fc.dtb` exists.

## Boot image

```bash
cd /path/to/UniXNU-tb365fc
dtc -I dts -O dtb -o build/tb365fc.dtb dts/tb365fc.dts
./unixnu bootimg --uefi ../uefi-tb365fc --dtb build/tb365fc.dtb
```

`Resources/Configs/tb365fc.toml` holds the FD base/size and the Android boot
image geometry. The stock image is header v4, so the addresses come from the
stock `vendor_boot` header (tags and DTB at `0x47C80000`, ramdisk at
`0x66F00000`, cmdline `bootopt=64S3,32N2,64N2`); `base`/`kernel_offset` place
the self-built v2 image at the FD base `0x40080000`. Update the file with
`tools/unpack_boot_image.py` in the device repository if the package changes.

## Plan A and Plan B

Plan A (primary) boots the UEFI FD directly from LK, the merlinx route:
FD at `0x40080000`, Android boot header v2, DTB appended to the kernel.

Plan B (fallback) is the Linux preboot hot-jump used by camellia. The
reference userland for it is kept unmodified under `BootImage/` and
`module/` (see `module/README.md`). It is **not** part of the default build
and has not been adapted or validated for the TB365FC kernel.

## Layout

```text
Platforms/Lenovo/tb365fcPkg/   platform DSC/FDF, memory map, boot memory, keypad
Silicon/MediaTek/MT6835Pkg/    MT6835 clock/GPIO/MSDC/PMIC/PlatformSec libraries
Silicon/MediaTek/MediaTekPkg/  shared MediaTek DXE drivers (PMIC, MSDC, GPIO)
Silicon/Silicium/              Mu-Silicium core packages
Resources/Configs/tb365fc.toml FD and boot image geometry
Resources/DTBs/                DTS sources for the .dtb the boot image needs
Resources/MuPatches/           Mu compatibility patches (Camellia-Essential is Plan B)
BootShim/                      direct-boot shim used by unixnu bootimg
module/, BootImage/            camellia Plan B reference assets (not built)
```

## Safety

- `example = true` in the device repository blocks `unixnu build` and
  `unixnu bootimg` until the placeholders are replaced.
- Never flash the produced FD to the tablet while any `REQUIRED` value is
  still in the tree: `Tb365fcBootMemoryDxe` and `MemoryMapLib` currently
  describe a memory map that was never measured on this hardware.
- The stock firmware dump, partition images and any device-identifying data
  (serial, IMEI, MAC) must not be committed here.

## Upstream and pinning

Submodules follow `.gitmodules` (Mu `release/202608`, OpenSSL
`67b5686b...`). The upstream Mu-Silicium baseline, toolchain and the exact
submodule commits still have to be pinned in this repository before
publication; see `PORTING.md`.
