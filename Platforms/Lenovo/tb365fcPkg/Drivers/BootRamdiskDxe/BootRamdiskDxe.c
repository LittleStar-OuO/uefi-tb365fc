/** @file
  UniXNU payload of Lenovo TB365FC as a read-only Block I/O device.

  UniXNU ships its payload (configuration, kernel, ADT, trust cache) as a FAT
  volume. This driver publishes it as a read-only, 512-byte-sector Block I/O
  device, so Partition/Fat drivers mount the payload volume.

  Primary transport: the payload is the vendor_boot vendor_ramdisk. LK loads
  it to ramdisk_addr 0x66F00000, appends the init_boot ramdisk after it, and
  exposes both through linux,initrd-start/-end in /chosen, pointing at the
  merged range. SEC keeps x0 (the DT copied to tags_addr 0x47C80000) in the
  gSiliciumEntryArgHobGuid HOB. The initrd must lie inside the reserved "Boot
  Ramdisk" region of the tb365fc memory map. The trailing init_boot ramdisk
  makes the total size unaligned; the published range is rounded up to a
  sector (it is still far below the 64 MiB region).

  Fallback: an older transport appended a compressed payload to the UEFI FD
  inside the boot image kernel gzip stream. It lands in the reserved "Boot
  Payload" window at 0x40400000 and is only used when /chosen carries no
  initrd, so a stale window cannot mask the ramdisk transport.

  In both cases the payload is a FAT image; the compressed fallback adds a
  16-byte wrapper (magic 'XNUZ', uncompressed size, compressed size,
  reserved) followed by a UEFI-compressed FAT image.

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
#include <Library/UefiDecompressLib.h>
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

//
// Compressed payload: 16-byte wrapper (magic, uncompressed size, compressed
// size, reserved) followed by a UEFI-compressed FAT image. LK may append the
// vendor_boot ramdisk after this stream, so only the wrapper's compressed
// size is consumed.
//
#define COMPRESSED_PAYLOAD_MAGIC         SIGNATURE_32 ('X', 'N', 'U', 'Z')
#define COMPRESSED_PAYLOAD_HEADER_SIZE   16

//
// Bring-up diagnostic (diag71): what the payload holds at the prelink record
// page and at the entry instruction, before the FAT driver reads it. The
// offsets come from the payload walk in build-entry-hunt-71.sh, which refuses
// to build if the geometry moves. The variable transport failed silently on
// diag70, so the block is copied to a fixed page instead: the last page of
// the 16 MiB boot payload window. DarwinBoot reads it into the entry-hunt
// readback block. Remove once settled.
//
#define PAYLOAD_PROBE_MAGIC       0x3037505244ULL  /* "DRP70" */
#define PAYLOAD_PROBE_VOL_REC     0x01917A00ULL
#define PAYLOAD_PROBE_VOL_ENTRY   0x028B5688ULL
#define PAYLOAD_PROBE_NAME_OFF    0x168
#define PAYLOAD_PROBE_FLAGS_OFF   0x180
#define PAYLOAD_PROBE_PAGE        0x413FF000ULL    /* 0x40400000 + 0x01000000 - 0x1000 */

#pragma pack(1)
typedef struct {
  UINT64  Magic;
  UINT64  Destination;
  UINT64  Size;
  UINT64  RecSig;
  UINT64  RecName;
  UINT64  RecFlags;
  UINT64  Entry;
} PAYLOAD_PROBE_BLOCK;

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
EFI_STATUS
PublishBuffer (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

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
  return Status;
}

STATIC
VOID
WritePayloadProbe (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  PAYLOAD_PROBE_BLOCK  ProbeBlock;

  ZeroMem (&ProbeBlock, sizeof (ProbeBlock));
  ProbeBlock.Magic       = PAYLOAD_PROBE_MAGIC;
  ProbeBlock.Destination = Start;
  ProbeBlock.Size        = End - Start;
  if (End - Start >= PAYLOAD_PROBE_VOL_REC + PAYLOAD_PROBE_FLAGS_OFF + 8) {
    ProbeBlock.RecSig   = ReadUnaligned64 ((VOID *)(UINTN)(Start + PAYLOAD_PROBE_VOL_REC));
    ProbeBlock.RecName  = ReadUnaligned64 ((VOID *)(UINTN)(Start + PAYLOAD_PROBE_VOL_REC + PAYLOAD_PROBE_NAME_OFF));
    ProbeBlock.RecFlags = ReadUnaligned64 ((VOID *)(UINTN)(Start + PAYLOAD_PROBE_VOL_REC + PAYLOAD_PROBE_FLAGS_OFF));
  }
  if (End - Start >= PAYLOAD_PROBE_VOL_ENTRY + 8) {
    ProbeBlock.Entry = ReadUnaligned64 ((VOID *)(UINTN)(Start + PAYLOAD_PROBE_VOL_ENTRY));
  }
  CopyMem ((VOID *)(UINTN)PAYLOAD_PROBE_PAGE, &ProbeBlock, sizeof (ProbeBlock));
  DiagInfo (
    "volume probe %lx/%lx/%lx entry %lx",
    ProbeBlock.RecSig,
    ProbeBlock.RecName,
    ProbeBlock.RecFlags,
    ProbeBlock.Entry
    );
}

STATIC
EFI_STATUS
PublishPayload (
  IN UINT64   Start,
  IN UINT64   End,
  IN BOOLEAN  AllowRaw
  )
{
  EFI_STATUS            Status;
  UINT32                CompressedSize;
  UINT32                UncompressedSize;
  UINT32                DestinationSize;
  UINT32                ScratchSize;
  EFI_PHYSICAL_ADDRESS  Destination;
  EFI_PHYSICAL_ADDRESS  Scratch;

  if ((End - Start >= COMPRESSED_PAYLOAD_HEADER_SIZE) &&
      (ReadUnaligned32 ((VOID *)(UINTN)Start) == COMPRESSED_PAYLOAD_MAGIC))
  {
    UncompressedSize = ReadUnaligned32 ((VOID *)(UINTN)(Start + 4));
    CompressedSize   = ReadUnaligned32 ((VOID *)(UINTN)(Start + 8));
    if ((CompressedSize == 0) ||
        ((UINT64)CompressedSize > End - Start - COMPRESSED_PAYLOAD_HEADER_SIZE))
    {
      DiagInfo ("compressed payload size %x bad", CompressedSize);
      return EFI_INVALID_PARAMETER;
    }

    Status = UefiDecompressGetInfo (
               (VOID *)(UINTN)(Start + COMPRESSED_PAYLOAD_HEADER_SIZE),
               CompressedSize,
               &DestinationSize,
               &ScratchSize
               );
    if (EFI_ERROR (Status) || (DestinationSize == 0) ||
        (DestinationSize != UncompressedSize))
    {
      DiagInfo (
        "compressed payload info %r dest=%x want=%x",
        Status,
        DestinationSize,
        UncompressedSize
        );
      return EFI_INVALID_PARAMETER;
    }

    if ((DestinationSize % RAM_BLOCK_SIZE) != 0) {
      DiagInfo ("compressed payload %x not sector aligned", DestinationSize);
      return EFI_INVALID_PARAMETER;
    }

    Destination = 0;
    Status      = gBS->AllocatePages (
                         AllocateAnyPages,
                         EfiBootServicesData,
                         EFI_SIZE_TO_PAGES (DestinationSize),
                         &Destination
                         );
    if (EFI_ERROR (Status)) {
      DiagInfo ("compressed payload alloc %r", Status);
      return Status;
    }

    Scratch = 0;
    if (ScratchSize != 0) {
      Status = gBS->AllocatePages (
                      AllocateAnyPages,
                      EfiBootServicesData,
                      EFI_SIZE_TO_PAGES (ScratchSize),
                      &Scratch
                      );
      if (EFI_ERROR (Status)) {
        gBS->FreePages (Destination, EFI_SIZE_TO_PAGES (DestinationSize));
        DiagInfo ("compressed payload scratch %r", Status);
        return Status;
      }
    }

    Status = UefiDecompress (
               (VOID *)(UINTN)(Start + COMPRESSED_PAYLOAD_HEADER_SIZE),
               (VOID *)(UINTN)Destination,
               (VOID *)(UINTN)Scratch
               );
    if (ScratchSize != 0) {
      gBS->FreePages (Scratch, EFI_SIZE_TO_PAGES (ScratchSize));
    }

    if (EFI_ERROR (Status)) {
      gBS->FreePages (Destination, EFI_SIZE_TO_PAGES (DestinationSize));
      DiagInfo ("compressed payload decompress %r", Status);
      return Status;
    }

    WritePayloadProbe (Destination, Destination + DestinationSize);

    DiagInfo (
      "compressed payload %x -> %x at %lx",
      CompressedSize,
      DestinationSize,
      (UINT64)Destination
      );
    Start = Destination;
    End   = Start + DestinationSize;
  } else if (!AllowRaw) {
    return EFI_NOT_FOUND;
  } else {
    // LK appends the init_boot ramdisk after the vendor ramdisk, so the
    // /chosen range is not necessarily sector aligned. Round up within the
    // reserved region; the FAT volume ends at its own BPB.
    UINT64  AlignedEnd = ALIGN_VALUE (End, RAM_BLOCK_SIZE);

    if (!InsideRegion ("Boot Ramdisk", Start, AlignedEnd - Start)) {
      DiagInfo ("initrd %lx-%lx overflows Boot Ramdisk", Start, AlignedEnd);
      return EFI_INVALID_PARAMETER;
    }

    if (AlignedEnd != End) {
      DiagInfo ("initrd %lx-%lx rounded to %lx", Start, End, AlignedEnd);
      End = AlignedEnd;
    }

    WritePayloadProbe (Start, End);
  }

  return PublishBuffer (Start, End);
}

STATIC
EFI_STATUS
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DiagInfo ("no /chosen in dtb");
    return EFI_NOT_FOUND;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DiagInfo ("no initrd in chosen (sl=%d el=%d)", StartLength, EndLength);
    return EFI_NOT_FOUND;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if ((End <= Start) || !InsideRegion ("Boot Ramdisk", Start, End - Start)) {
    DiagInfo ("initrd %lx-%lx outside Boot Ramdisk", Start, End);
    return EFI_NOT_FOUND;
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

  return PublishPayload (Start, End, TRUE);
}

STATIC
EFI_STATUS
PublishEmbeddedPayload (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  if (EFI_ERROR (LocateMemoryRegionByName ("Boot Payload", &Region))) {
    return EFI_NOT_FOUND;
  }

  if (ReadUnaligned32 ((VOID *)(UINTN)Region.Address) != COMPRESSED_PAYLOAD_MAGIC) {
    return EFI_NOT_FOUND;
  }

  DiagInfo ("embedded payload at %lx", (UINT64)Region.Address);
  return PublishPayload (Region.Address, Region.Address + Region.Length, FALSE);
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
  } else {
    Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);

    // Only touch x0 when it points into the reserved LK DTB window.
    if ((Dtb & 7) != 0 || !InsideRegion ("LK DTB", Dtb, sizeof (FDT_HEADER))) {
      DiagInfo ("x0 %lx not in LK DTB", Dtb);
    } else if ((FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) ||
               !InsideRegion ("LK DTB", Dtb, FdtTotalSize ((VOID *)(UINTN)Dtb)))
    {
      DiagInfo ("x0 %lx not an fdt", Dtb);
    } else {
      DiagInfo ("dtb %lx size %x", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb));
      if (PublishInitrd ((VOID *)(UINTN)Dtb) != EFI_NOT_FOUND) {
        return EFI_SUCCESS;
      }
    }
  }

  // Fallback for kernels whose payload still rides in the FD gzip stream.
  if (PublishEmbeddedPayload () != EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}
