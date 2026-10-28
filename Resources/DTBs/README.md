# Device tree blobs

`tb365fc.dts` and `mt6835.dtsi` are the firmware-side copies of the sources
in `UniXNU-tb365fc/dts/` and must stay identical to them. The shared CPU, GIC,
timer and UART values are measured from the stock `vendor_boot` DTB; the board
memory map and peripheral wiring are still skeletons (see `PORTING.md`).

No compiled `.dtb` is committed. Generate it on a host with `dtc`:

```bash
dtc -I dts -O dtb -o tb365fc.dtb tb365fc.dts
```

`Resources/Configs/tb365fc.toml` points at this file. The UniXNU boot image
step takes the DTB from `--dtb` (or `UNIXNU_DTB`) instead, so the same
compiled blob can live in `out/<device>/tree/build/`.
