/** @file
  Boot image ramdisk of Lenovo TB365FC as a read-only Block I/O device.

  UniXNU ships its payload (configuration, kernel, ADT, trust cache) as a FAT
  volume in the ramdisk of the Android boot image.

  MTK LK strips the MediaTek "ROOTFS" header (magic 0x58881688, payload size
  at +4, name at +8) when it reads the boot image, copies the payload to the
  header ramdisk_addr and publishes that range in /chosen as
  linux,initrd-start/-end. tb365fc.toml uses base 0x40000000 and
  ramdisk_offset 0x26F00000, so the payload lands at 0x66F00000, the start of
  the "Boot Ramdisk" region of MemoryMapLib.

  SEC keeps x0 (the DT copied to tags_addr 0x47C80000) in the
  gSiliciumEntryArgHobGuid HOB. This driver takes the initrd range from
  /chosen of that device tree, the address the loader actually used, and
  publishes it as a read-only, 512-byte-sector Block I/O device, so
  Partition/Fat drivers mount the payload volume. If the range still starts
  with a MediaTek header (a loader that does not strip it), the 512-byte
  header is skipped.

  Both the device tree and the initrd must lie inside the "LK DTB" and
  "Boot Ramdisk" regions of the tb365fc memory map; these are reserved, so no
  firmware allocation can overwrite them. A range elsewhere is not published:
  firmware allocations may already have overwritten it.

  Derived from UniXNU/uefi-sheng shengPkg ShengBootMemoryDxe (initrd part).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE  512

//
// Bring-up diagnostic: the outcome of this driver as a variable, so the BDS
// report on the panel can show it (there is no reachable serial console).
// Removed once the payload volume is verified on hardware.
//
STATIC CONST EFI_GUID  mDiagGuid = {
  0x5b5c2f01, 0x88a3, 0x4a55, { 0xb6, 0x77, 0x19, 0x2c, 0xd3, 0x41, 0xab, 0x9e }
};

STATIC
VOID
DiagInfo (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Text[120];

  VA_START (Args, Format);
  AsciiVSPrint (Text, sizeof (Text), Format, Args);
  VA_END (Args);
  gRT->SetVariable (
         L"UniXNURamdisk",
         (EFI_GUID *)&mDiagGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
         AsciiStrLen (Text) + 1,
         Text
         );
  DEBUG ((DEBUG_ERROR, "TB365FCRD: %a\n", Text));
}

//
// MediaTek image header in front of the boot image ramdisk: checked by LK
// FUN_41e03880, written by build_uefi.py mtk_image_section (magic, payload
// size, 32-byte name "ROOTFS", 0xFF padding to 512 bytes).
//
#define MTK_HEADER_MAGIC  0x58881688U
#define MTK_HEADER_SIZE   512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x5D3A0B2C, 0x8167, 0x4A04, { 0xB3, 0x0D, 0x52, 0x41, 0x4D, 0x44, 0x49, 0x53 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
BOOLEAN
InsideRegion (
  IN CHAR8   *RegionName,
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  if (EFI_ERROR (LocateMemoryRegionByName (RegionName, &Region))) {
    return FALSE;
  }

  return (Size != 0) && (Base >= Region.Address) && (Base + Size > Base) &&
         (Base + Size <= Region.Address + Region.Length);
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  EFI_HANDLE  Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DiagInfo ("no /chosen in dtb");
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DiagInfo ("no initrd in chosen (sl=%d el=%d)", StartLength, EndLength);
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if ((End <= Start) || !InsideRegion ("Boot Ramdisk", Start, End - Start)) {
    DiagInfo ("initrd %lx-%lx outside Boot Ramdisk", Start, End);
    return;
  }

  // The stock LK already strips the header; skip one left in place.
  if ((End - Start > MTK_HEADER_SIZE) &&
      (ReadUnaligned32 ((VOID *)(UINTN)Start) == MTK_HEADER_MAGIC))
  {
    UINT32  PayloadSize;

    PayloadSize = ReadUnaligned32 ((VOID *)(UINTN)(Start + 4));
    DiagInfo ("mtk header at %lx, payload %x", Start, PayloadSize);
    Start += MTK_HEADER_SIZE;
    if ((PayloadSize != 0) && (PayloadSize < End - Start)) {
      End = Start + PayloadSize;
    }
  }

  if (((End - Start) % RAM_BLOCK_SIZE) != 0) {
    DiagInfo ("initrd %lx-%lx not sector aligned", Start, End);
    return;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DiagInfo (
    "installed %lx-%lx last=%lx %r",
    Start,
    End,
    (UINT64)mMedia.LastBlock,
    Status
    );
}

EFI_STATUS
EFIAPI
BootRamdiskDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DiagInfo ("no entry argument hob");
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);

  // Only touch x0 when it points into the reserved LK DTB window.
  if ((Dtb & 7) != 0 || !InsideRegion ("LK DTB", Dtb, sizeof (FDT_HEADER))) {
    DiagInfo ("x0 %lx not in LK DTB", Dtb);
    return EFI_SUCCESS;
  }

  if ((FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) ||
      !InsideRegion ("LK DTB", Dtb, FdtTotalSize ((VOID *)(UINTN)Dtb)))
  {
    DiagInfo ("x0 %lx not an fdt", Dtb);
    return EFI_SUCCESS;
  }

  DiagInfo ("dtb %lx size %x", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb));

  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
