# Plan B hot-jump reference (camellia)

`uefi_jump/` and `drm_linear/` are copied unmodified from the camellia
(MT6833) reference tree, together with `BootImage/` one level up. They belong
to the Linux preboot hot-jump route, where the vendor kernel hands control to
a second copy of UEFI at `0xA0000000` with the CPU stack at `0xA0200000`.

They are **not** built by the TB365FC DSC and have not been adapted to this
tablet. Treat them as a starting point only if Plan A (direct LK boot)
proves impossible. The related EDK II patch is
`Resources/MuPatches/Camellia-Essential.patch`; the UniXNU firmware step does
not apply it.

Build requirements for the module are documented in `uefi_jump/README.md`
and `uefi_jump/Makefile` (out-of-tree kernel build, `KDIR` pointing at the
matching Android kernel output).
