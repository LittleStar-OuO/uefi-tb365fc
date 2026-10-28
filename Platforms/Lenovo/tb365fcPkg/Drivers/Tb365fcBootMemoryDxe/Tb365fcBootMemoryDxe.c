/** @file
  Device tree, initrd and framebuffer memory, from the device tree the previous
  stage handed over.

  The previous stage (LK) passes the physical address of a flattened device tree
  in x0, which SEC keeps in a HOB. This driver:

  * maps the tree and the initrd it names into the GCD as reserved memory, so
    every access is translated and no allocator can hand the ranges out;
  * publishes the initrd as a read-only block device, so a FAT image in the boot
    image's ramdisk shows up as a normal volume for UniXNU and DarwinBoot;
  * publishes the framebuffer LK left scanning out as the SimpleFbDxe handoff,
    so the UEFI console and BDS have a display. The base comes from
    /chosen atag,videolfb-fb_base_l/_h; when the tree has no /chosen the
    measured mblock-11-framebuffer carveout is used instead;
  * paints the scanout directly, before GOP exists, with bars that prove the
    base and pitch and 7-segment hex of the watchdog registers and the
    published base. This makes a display or reset failure diagnosable from a
    photograph without a working console.

  The device tree, initrd and framebuffer ranges come from the DT. No additional
  SystemMemory window is published yet: /proc/iomem shows the reserved carveouts
  (TEE 0x70000000, WiFi-EMI, the LK framebuffer at 0xFCF2B000 and the high-DDR
  windows) inside the placeholder range this driver used to hand out, so the DXE
  heap remains the only pool until that inventory is reconciled.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SdMmcPassThru.h>

//
// The media the FAT driver sees. The block device exposes 512-byte logical
// sectors, and the bootloader reports the initrd as a byte range, so the media
// covers whole sectors only.
//
#define RAM_BLOCK_SIZE  512

//
// The tree header is read before the total size is known. A tree header is
// smaller than two pages, and this window also covers a header that crosses a
// page boundary. The rest of the tree is mapped from its own totalsize.
//
#define FDT_HEADER_WINDOW_SIZE  (2 * SIZE_4KB)
#define FDT_MAX_SIZE            (16 * SIZE_1MB)

//
// The panel LK initializes and keeps scanning out: 2560x1600, 32 bpp. The
// scanout base rides in /chosen atag,videolfb; the geometry does not, and this
// is the mode the stock DTB describes.
//
#define PANEL_WIDTH   2560U
#define PANEL_HEIGHT  1600U
#define PANEL_DEPTH   32U
#define PANEL_PITCH   (PANEL_WIDTH * (PANEL_DEPTH / 8))

//
// The handoff layout SimpleFbDxe and FrameBufferSerialPortLib parse
// (SiliciumPkg/Include/Library/FrameBufferMap.h). The header carries static
// helpers that would trip -Wunused-function here, so the layout is repeated.
//
#define FB_HANDOFF_ADDR       0xA4000000ULL
#define FB_PAGE_MAP_ADDR      0xA4000100ULL
#define FB_HANDOFF_MAGIC      0x3130424649464555ULL
#define FB_PAGE_MAP_MAGIC     0x314D504649465545ULL
#define FB_PAGE_MAP_MAX_RUNS  100U

//
// The scanout carveout LK reserves on this device (mblock-11-framebuffer,
// 0xFCF2B000 + 0x30D4000, matching /proc/iomem). Used only when the tree has
// no /chosen videolfb properties, which happens when the image carries a
// board DTB that LK does not extend; the carveout itself is fixed by the
// device DTBO, not by the boot image.
//
#define FB_FALLBACK_BASE  0xFCF2B000ULL

//
// Watchdog and its status register, from the stock watchdog@1c007000 node.
//
#define WDT_MODE_REGISTER    0x1C007000U
#define WDT_STATUS_REGISTER  0x1C00700CU
#define WDT_MODE_ENABLE      0x00000001U

#pragma pack(1)
typedef struct {
  UINT64  Magic;
  UINT64  Base;
  UINT32  Width;
  UINT32  Height;
  UINT32  Pitch;
  UINT32  Depth;
} FB_HANDOFF;

typedef struct {
  UINT64  Physical;
  UINT32  Pages;
  UINT32  Reserved;
} FB_PAGE_MAP_RUN;

typedef struct {
  UINT64          Magic;
  UINT64          LogicalSize;
  UINT32          PageSize;
  UINT32          RunCount;
  FB_PAGE_MAP_RUN Runs[FB_PAGE_MAP_MAX_RUNS];
} FB_PAGE_MAP;
#pragma pack()
//
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

/**
  Add a range to the GCD as reserved memory with the requested attributes.

  Reserved keeps the firmware from allocating over it while every access still
  works; the page tables follow the GCD, so a range that is not added here is
  not necessarily reachable. A range the platform already added is left alone,
  and the attributes are only requested for the part that exists.

  @param[in] Start       First byte of the range.
  @param[in] End         End of the range, exclusive.
  @param[in] Attributes  EFI_MEMORY_WB for cacheable data, EFI_MEMORY_WT for a
                         scanout buffer.
  @param[in] Name        Range name for the log.
**/
STATIC
EFI_STATUS
MapReserved (
  IN UINT64       Start,
  IN UINT64       End,
  IN UINT64       Attributes,
  IN CONST CHAR8  *Name
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_PHYSICAL_ADDRESS             Allocation;
  UINT64                           Base;
  UINT64                           Limit;
  UINT64                           Length;
  UINTN                            Pages;
  BOOLEAN                          Added;
  BOOLEAN                          Allocated;

  if (End <= Start) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Start > ((UINT64)-1 - (SIZE_4KB - 1))) || (End > ((UINT64)-1 - (SIZE_4KB - 1)))) {
    return EFI_INVALID_PARAMETER;
  }

  Base  = Start & ~((UINT64)SIZE_4KB - 1);
  Limit = ALIGN_VALUE (End, SIZE_4KB);
  if ((Limit <= Base) || ((Limit - Base) < SIZE_4KB)) {
    return EFI_INVALID_PARAMETER;
  }

  Length = Limit - Base;
  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx: no descriptor %r\n", Name, Base, Limit, Status));
    return Status;
  }

  if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx is MMIO\n", Name, Base, Limit));
    return EFI_UNSUPPORTED;
  }

  Added     = FALSE;
  Allocated = FALSE;
  Pages     = 0;

  if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeReserved,
                    Base,
                    Length,
                    EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx add failed %r\n", Name, Base, Limit, Status));
      return Status;
    }

    Added = TRUE;
  } else if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeSystemMemory) {
    //
    // The platform already put this range in the system memory pool. Keep the
    // allocator away from it before anything can allocate over the initrd.
    //
    Pages      = EFI_SIZE_TO_PAGES (Length);
    Allocation = Base;
    Status     = gBS->AllocatePages (AllocateAddress, EfiReservedMemoryType, Pages, &Allocation);
    if (EFI_ERROR (Status) || (Allocation != Base)) {
      if (!EFI_ERROR (Status)) {
        gBS->FreePages (Allocation, Pages);
        Status = EFI_OUT_OF_RESOURCES;
      }

      DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx reserve failed %r\n", Name, Base, Limit, Status));
      return Status;
    }

    Allocated = TRUE;
  } else if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeReserved) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx has unsupported GCD type %u\n", Name, Base, Limit, Descriptor.GcdMemoryType));
    return EFI_UNSUPPORTED;
  }

  Status = gDS->SetMemorySpaceAttributes (Base, Length, Attributes);
  if (EFI_ERROR (Status)) {
    if (Added) {
      gDS->RemoveMemorySpace (Base, Length);
    }

    if (Allocated) {
      gBS->FreePages (Base, Pages);
    }

    DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx attributes 0x%lx failed %r\n", Name, Base, Limit, Attributes, Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx mapped attributes 0x%lx\n", Name, Base, Limit - 1, Attributes));
  return EFI_SUCCESS;
}

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x09F3212E, 0x61B0, 0x4296, { 0xA8, 0xD2, 0x07, 0x28, 0x62, 0xC8, 0x50, 0x03 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                   *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA      mMedia;

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

#ifndef MDEPKG_NDEBUG
STATIC EFI_EVENT  mBlockIoProbeEvent;
STATIC VOID       *mBlockIoProbeRegistration;
STATIC BOOLEAN    mBlockIoProbeDone;

STATIC
VOID
EFIAPI
ProbeBlockIo (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE            *Handles;
  UINTN                 HandleCount;
  UINTN                 Index;
  UINTN                 WritableCount;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru;
  EFI_DEVICE_PATH_PROTOCOL *PassThruPath;
  VENDOR_DEVICE_PATH    *VendorPath;
  EFI_HANDLE            *PassThruHandles;
  UINTN                 PassThruCount;
  UINTN                 PassThruIndex;
  UINT8                 Slot;
  UINT64                MaxLastBlock;
  UINT64                MaxWritable;

  if (mBlockIoProbeDone) {
    return;
  }

  PassThruHandles = NULL;
  PassThruCount   = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSdMmcPassThruProtocolGuid, NULL, &PassThruCount, &PassThruHandles);
  DEBUG ((DEBUG_ERROR, "TB365FCMEM: SdMmcPassThru handles %lu, %r\n", (UINT64)PassThruCount, Status));
  if (!EFI_ERROR (Status)) {
    for (PassThruIndex = 0; PassThruIndex < PassThruCount; PassThruIndex++) {
      if (!EFI_ERROR (gBS->HandleProtocol (PassThruHandles[PassThruIndex], &gEfiSdMmcPassThruProtocolGuid, (VOID **)&PassThru))) {
        PassThruPath = NULL;
        VendorPath   = NULL;
        if (!EFI_ERROR (gBS->HandleProtocol (PassThruHandles[PassThruIndex], &gEfiDevicePathProtocolGuid, (VOID **)&PassThruPath)) &&
            (PassThruPath != NULL) && (PassThruPath->Type == HARDWARE_DEVICE_PATH) && (PassThruPath->SubType == HW_VENDOR_DP)) {
          VendorPath = (VENDOR_DEVICE_PATH *)PassThruPath;
        }
        Slot = 0xFF;
        Status = PassThru->GetNextSlot (PassThru, &Slot);
        DEBUG ((DEBUG_ERROR, "TB365FCMEM: SdMmcPassThru %u host=%u slot=%u %r\n",
                (UINT32)PassThruIndex, (VendorPath != NULL) ? (UINT32)VendorPath->Guid.Data4[7] : 0xFF,
                Slot, Status));
        Status = gBS->ConnectController (PassThruHandles[PassThruIndex], NULL, NULL, TRUE);
        DEBUG ((DEBUG_ERROR, "TB365FCMEM: SdMmcPassThru %u connect %r\n", (UINT32)PassThruIndex, Status));
      }
    }
    gBS->FreePool (PassThruHandles);
  }

  Handles       = NULL;
  HandleCount   = 0;
  WritableCount = 0;
  MaxLastBlock  = 0;
  MaxWritable   = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  DEBUG ((DEBUG_ERROR, "TB365FCMEM: BlockIo after connect %lu, %r\n", (UINT64)HandleCount, Status));
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
        if ((UINT64)BlockIo->Media->LastBlock > MaxLastBlock) {
          MaxLastBlock = (UINT64)BlockIo->Media->LastBlock;
        }
        if (!BlockIo->Media->ReadOnly) {
          WritableCount++;
          if ((UINT64)BlockIo->Media->LastBlock > MaxWritable) {
            MaxWritable = (UINT64)BlockIo->Media->LastBlock;
          }
        }
      }
    }
    gBS->FreePool (Handles);
  }
  DEBUG ((DEBUG_ERROR, "TB365FCMEM: BlockIo summary count=%lu max=0x%lx writable=%lu maxWritable=0x%lx\n",
          (UINT64)HandleCount, MaxLastBlock, (UINT64)WritableCount, MaxWritable));

  if (HandleCount > 0) {
    mBlockIoProbeDone = TRUE;
    gBS->Stall (5 * 1000 * 1000);
  }
}
#endif

/**
  Map the device tree header, then map the rest of the tree from its totalsize.

  The header is read from a small window because the total size is not known
  until the header is available. The rest of the tree is mapped separately so a
  tree does not pull an unrelated LK or ATF range into the GCD.

  @param[in]  Dtb        Device tree address from the previous stage.
  @param[out] Fdt        Mapped device tree.
  @param[out] TotalSize  Device tree size from its header.
**/
STATIC
EFI_STATUS
MapDeviceTree (
  IN  UINT64  Dtb,
  OUT VOID    **Fdt,
  OUT UINT32  *TotalSize
  )
{
  EFI_STATUS  Status;
  UINT64      HeaderBase;
  UINT64      HeaderEnd;
  UINT64      ActualEnd;
  UINT64      FdtEnd;
  UINT32      Total;

  HeaderBase = Dtb & ~((UINT64)SIZE_4KB - 1);
  if (HeaderBase > ((UINT64)-1 - FDT_HEADER_WINDOW_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }

  HeaderEnd = HeaderBase + FDT_HEADER_WINDOW_SIZE;
  Status    = MapReserved (HeaderBase, HeaderEnd, EFI_MEMORY_WB, "device tree header");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: x0 0x%lx is not a device tree\n", Dtb));
    return EFI_INVALID_PARAMETER;
  }

  Total = FdtTotalSize ((VOID *)(UINTN)Dtb);
  if ((Total < sizeof (FDT_HEADER)) || (Total > FDT_MAX_SIZE)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: device tree totalsize 0x%x is invalid\n", Total));
    return EFI_INVALID_PARAMETER;
  }

  FdtEnd = (UINT64)Dtb + (UINT64)Total;
  if ((FdtEnd < (UINT64)Dtb) || (FdtEnd > ((UINT64)-1 - (SIZE_4KB - 1)))) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: device tree 0x%lx + 0x%x overflows\n", Dtb, Total));
    return EFI_INVALID_PARAMETER;
  }

  ActualEnd = ALIGN_VALUE (FdtEnd, SIZE_4KB);
  if (ActualEnd > HeaderEnd) {
    Status = MapReserved (HeaderEnd, ActualEnd, EFI_MEMORY_WB, "device tree");
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  *Fdt       = (VOID *)(UINTN)Dtb;
  *TotalSize = Total;
  return EFI_SUCCESS;
}

/**
  Publish the initrd /chosen names as a read-only block device.

  The FAT volume of a boot payload lives in the boot image's ramdisk, which the
  bootloader places at linux,initrd-start and reports the end of in
  linux,initrd-end. Both properties are cells, either one or two each.

  @param[in] Fdt  Flattened device tree, already mapped.
**/
STATIC
EFI_STATUS
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
  UINT64      Size;
  EFI_HANDLE  Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: no /chosen\n"));
    return EFI_NOT_FOUND;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: no initrd in /chosen\n"));
    return EFI_NOT_FOUND;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if ((Start == 0) || (End <= Start)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd 0x%lx-0x%lx is empty\n", Start, End));
    return EFI_NOT_FOUND;
  }

  Size = End - Start;
  if ((Size % RAM_BLOCK_SIZE) != 0) {
    //
    // The bootloader reports the file size, not the sector count. Keep the
    // whole sectors and say how many bytes the media loses; bootimg pads the
    // ramdisk so this does not happen with an image the build driver packed.
    //
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd 0x%lx-0x%lx drops %lu bytes, not a whole number of sectors\n",
            Start, End, Size % RAM_BLOCK_SIZE));
    Size &= ~((UINT64)RAM_BLOCK_SIZE - 1);
  }

  if (Size < RAM_BLOCK_SIZE) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd 0x%lx-0x%lx is shorter than one sector\n", Start, End));
    return EFI_NOT_FOUND;
  }

  Status = MapReserved (Start, Start + Size, EFI_MEMORY_WB, "initrd");
  if (EFI_ERROR (Status)) {
    return Status;
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
  mMedia.LastBlock        = (Size / RAM_BLOCK_SIZE) - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd block device failed %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd block device 0x%lx size 0x%lx\n", Start, Size));
  return EFI_SUCCESS;
}

#define FB_COLOR(Red, Green, Blue) \
  ((UINT32)((((UINT32)(Red)) << 16) | (((UINT32)(Green)) << 8) | ((UINT32)(Blue))))

STATIC
VOID
PaintFill (
  IN UINT32  *Fb,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Width,
  IN UINT32  Height,
  IN UINT32  Color
  )
{
  UINT32  Line;
  UINT32  Column;

  for (Line = 0; Line < Height; Line++) {
    for (Column = 0; Column < Width; Column++) {
      Fb[(Y + Line) * PANEL_WIDTH + X + Column] = Color;
    }
  }
}

//
// Segment masks for 0-F: bit0 top, bit1 top-right, bit2 bottom-right,
// bit3 bottom, bit4 bottom-left, bit5 top-left, bit6 middle.
//
STATIC CONST UINT8  mHexSegments[16] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
  0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71
};

STATIC
VOID
PaintDigit (
  IN UINT32  *Fb,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Unit,
  IN UINT8   Segments,
  IN UINT32  Color
  )
{
  if ((Segments & 0x01) != 0) {
    PaintFill (Fb, X + Unit, Y, 3 * Unit, Unit, Color);
  }

  if ((Segments & 0x02) != 0) {
    PaintFill (Fb, X + 4 * Unit, Y + Unit, Unit, 2 * Unit, Color);
  }

  if ((Segments & 0x04) != 0) {
    PaintFill (Fb, X + 4 * Unit, Y + 4 * Unit, Unit, 2 * Unit, Color);
  }

  if ((Segments & 0x08) != 0) {
    PaintFill (Fb, X + Unit, Y + 6 * Unit, 3 * Unit, Unit, Color);
  }

  if ((Segments & 0x10) != 0) {
    PaintFill (Fb, X, Y + 4 * Unit, Unit, 2 * Unit, Color);
  }

  if ((Segments & 0x20) != 0) {
    PaintFill (Fb, X, Y + Unit, Unit, 2 * Unit, Color);
  }

  if ((Segments & 0x40) != 0) {
    PaintFill (Fb, X + Unit, Y + 3 * Unit, 3 * Unit, Unit, Color);
  }
}

STATIC
VOID
PaintHexRow (
  IN UINT32  *Fb,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Unit,
  IN UINT32  Value,
  IN UINT32  Color
  )
{
  UINT32  Index;

  for (Index = 0; Index < 8; Index++) {
    PaintDigit (
      Fb,
      X + Index * 6 * Unit,
      Y,
      Unit,
      mHexSegments[(Value >> (28 - 4 * Index)) & 0xF],
      Color
      );
  }
}

/**
  Paint the scanout directly, without GOP or the DXE console.

  Eight vertical bars prove the base address and pitch are the ones the panel
  scans. A black panel then shows four rows of 7-segment hex: WDT_MODE at
  0x1C007000, WDT_STATUS at 0x1C00700C, the low and high halves of the
  published frame buffer base. A green square after the WDT_MODE row means the
  watchdog reads back disabled; red means the enable bit survived the SEC
  write. The bars are held for five seconds, long enough to photograph, then
  BDS paints its own graphics over them.

  @param[in] Base       Framebuffer physical base, already mapped.
  @param[in] WdtMode    WDT_MODE readback.
  @param[in] WdtStatus  WDT_STATUS readback.
**/
STATIC
VOID
PaintDiagnostics (
  IN UINT64  Base,
  IN UINT32  WdtMode,
  IN UINT32  WdtStatus
  )
{
  UINT32  *Fb;
  UINT32  Bars[8];
  UINT32  Index;
  UINT32  Unit;
  UINT32  Y;
  UINT32  White;

  Fb    = (UINT32 *)(UINTN)Base;
  White = FB_COLOR (0xFF, 0xFF, 0xFF);

  Bars[0] = FB_COLOR (0xE0, 0x00, 0x00);
  Bars[1] = FB_COLOR (0x00, 0xE0, 0x00);
  Bars[2] = FB_COLOR (0x00, 0x00, 0xE0);
  Bars[3] = FB_COLOR (0xE0, 0xE0, 0x00);
  Bars[4] = FB_COLOR (0x00, 0xE0, 0xE0);
  Bars[5] = FB_COLOR (0xE0, 0x00, 0xE0);
  Bars[6] = White;
  Bars[7] = FB_COLOR (0x00, 0x00, 0x00);

  for (Index = 0; Index < ARRAY_SIZE (Bars); Index++) {
    PaintFill (Fb, Index * (PANEL_WIDTH / 8), 0, PANEL_WIDTH / 8, PANEL_HEIGHT, Bars[Index]);
  }

  Unit = 8;
  PaintFill (Fb, 16, 16, 560, 304, FB_COLOR (0x00, 0x00, 0x00));

  Y = 32;
  PaintFill (Fb, 32, Y + 8, 4 * Unit, 4 * Unit, FB_COLOR (0xE0, 0x00, 0x00));
  PaintHexRow (Fb, 96, Y, Unit, WdtMode, White);
  PaintFill (
    Fb,
    528,
    Y + 8,
    4 * Unit,
    4 * Unit,
    ((WdtMode & WDT_MODE_ENABLE) != 0) ? FB_COLOR (0xE0, 0x00, 0x00) : FB_COLOR (0x00, 0xC0, 0x00)
    );

  Y = 100;
  PaintFill (Fb, 32, Y + 8, 4 * Unit, 4 * Unit, FB_COLOR (0xE0, 0xE0, 0x00));
  PaintHexRow (Fb, 96, Y, Unit, WdtStatus, White);

  Y = 168;
  PaintFill (Fb, 32, Y + 8, 4 * Unit, 4 * Unit, FB_COLOR (0x00, 0xE0, 0xE0));
  PaintHexRow (Fb, 96, Y, Unit, (UINT32)Base, White);

  Y = 236;
  PaintFill (Fb, 32, Y + 8, 4 * Unit, 4 * Unit, FB_COLOR (0xE0, 0x00, 0xE0));
  PaintHexRow (Fb, 96, Y, Unit, (UINT32)(Base >> 32), White);

  WriteBackDataCacheRange ((VOID *)Fb, (UINTN)PANEL_PITCH * PANEL_HEIGHT);
  gBS->Stall (5 * 1000 * 1000);
}

/**
  Publish the framebuffer LK left scanning out as the SimpleFbDxe handoff.

  LK records the scanout base in /chosen atag,videolfb-fb_base_l and
  atag,videolfb-fb_base_h; the geometry is the panel mode. SimpleFbDxe and the
  debug console read the handoff page directly, so MemoryMapLib reserves it as
  well. A board DTB that LK does not extend carries no videolfb properties;
  the measured carveout is used then, and the diagnostic on screen reports
  which base was published.

  @param[in] Fdt  Flattened device tree, already mapped.
**/
STATIC
EFI_STATUS
PublishFramebuffer (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS   Status;
  INT32        Chosen;
  CONST VOID   *BaseLow;
  CONST VOID   *BaseHigh;
  CONST VOID   *Vram;
  INT32        BaseLowLength;
  INT32        BaseHighLength;
  INT32        VramLength;
  UINT64       Base;
  UINT64       VramSize;
  UINT64       LogicalSize;
  UINT32       WdtMode;
  UINT32       WdtStatus;
  BOOLEAN      FromDt;
  FB_HANDOFF   *Handoff;
  FB_PAGE_MAP  *Map;

  Base     = 0;
  VramSize = 0;
  FromDt   = FALSE;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen >= 0) {
    BaseLow = FdtGetProp (Fdt, Chosen, "atag,videolfb-fb_base_l", &BaseLowLength);
    if ((BaseLow != NULL) && (BaseLowLength == 4)) {
      Base = Fdt32ToCpu (ReadUnaligned32 (BaseLow));

      BaseHigh = FdtGetProp (Fdt, Chosen, "atag,videolfb-fb_base_h", &BaseHighLength);
      if ((BaseHigh != NULL) && (BaseHighLength == 4)) {
        Base |= LShiftU64 ((UINT64)Fdt32ToCpu (ReadUnaligned32 (BaseHigh)), 32);
      }

      FromDt = TRUE;
    }

    Vram = FdtGetProp (Fdt, Chosen, "atag,videolfb-vramSize", &VramLength);
    if ((Vram != NULL) && (VramLength == 4)) {
      VramSize = Fdt32ToCpu (ReadUnaligned32 (Vram));
    }
  }

  if (!FromDt) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: no atag,videolfb in /chosen, using measured carveout\n"));
    Base = FB_FALLBACK_BASE;
  }

  if ((Base == 0) || ((Base & (SIZE_4KB - 1)) != 0) ||
      (Base < 0x40000000ULL) || (Base >= 0x240000000ULL)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: invalid framebuffer base 0x%lx\n", Base));
    return EFI_INVALID_PARAMETER;
  }

  LogicalSize = (UINT64)PANEL_PITCH * PANEL_HEIGHT;
  if ((Base + LogicalSize) > 0x240000000ULL) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: framebuffer 0x%lx + 0x%lx leaves DRAM\n", Base, LogicalSize));
    return EFI_INVALID_PARAMETER;
  }

  if ((VramSize != 0) && (LogicalSize > VramSize)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: framebuffer 0x%lx size 0x%lx exceeds vramSize 0x%lx\n", Base, LogicalSize, VramSize));
    return EFI_INVALID_PARAMETER;
  }

  Status = MapReserved (Base, Base + LogicalSize, EFI_MEMORY_WT, "framebuffer");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handoff = (FB_HANDOFF *)(UINTN)FB_HANDOFF_ADDR;
  ZeroMem (Handoff, sizeof (*Handoff));
  Handoff->Magic  = FB_HANDOFF_MAGIC;
  Handoff->Base   = Base;
  Handoff->Width  = PANEL_WIDTH;
  Handoff->Height = PANEL_HEIGHT;
  Handoff->Pitch  = PANEL_PITCH;
  Handoff->Depth  = PANEL_DEPTH;

  Map = (FB_PAGE_MAP *)(UINTN)FB_PAGE_MAP_ADDR;
  ZeroMem (Map, sizeof (*Map));
  Map->Magic            = FB_PAGE_MAP_MAGIC;
  Map->LogicalSize      = LogicalSize;
  Map->PageSize         = (UINT32)SIZE_4KB;
  Map->RunCount         = 1;
  Map->Runs[0].Physical = Base;
  Map->Runs[0].Pages    = (UINT32)EFI_SIZE_TO_PAGES (LogicalSize);

  WdtMode   = MmioRead32 (WDT_MODE_REGISTER);
  WdtStatus = MmioRead32 (WDT_STATUS_REGISTER);

  DEBUG ((
    DEBUG_ERROR,
    "TB365FCMEM: framebuffer 0x%lx %ux%u pitch %u from %a, wdt mode 0x%x status 0x%x\n",
    Base,
    PANEL_WIDTH,
    PANEL_HEIGHT,
    PANEL_PITCH,
    FromDt ? "DT" : "carveout",
    WdtMode,
    WdtStatus
    ));

  PaintDiagnostics (Base, WdtMode, WdtStatus);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Tb365fcBootMemoryDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  EFI_STATUS         Status;
  UINT64             Dtb;
  UINT32             TotalSize;
  VOID               *Fdt;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: no entry argument HOB\n"));
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Dtb == 0) || ((Dtb & 3) != 0)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: x0 0x%lx is not a device tree address\n", Dtb));
    return EFI_SUCCESS;
  }

  Status = MapDeviceTree (Dtb, &Fdt, &TotalSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: device tree mapping failed %r\n", Status));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "TB365FCMEM: device tree at 0x%lx, %u bytes\n", Dtb, TotalSize));

  Status = PublishInitrd (Fdt);
  DEBUG ((DEBUG_ERROR, "TB365FCMEM: initrd publish %r\n", Status));

  Status = PublishFramebuffer (Fdt);
  DEBUG ((DEBUG_ERROR, "TB365FCMEM: framebuffer publish %r\n", Status));

#ifndef MDEPKG_NDEBUG
  ProbeBlockIo (NULL, NULL);
  if (!mBlockIoProbeDone) {
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    ProbeBlockIo,
                    NULL,
                    &mBlockIoProbeEvent
                    );
    if (!EFI_ERROR (Status)) {
      Status = gBS->RegisterProtocolNotify (
                      &gEfiBlockIoProtocolGuid,
                      mBlockIoProbeEvent,
                      &mBlockIoProbeRegistration
                      );
    }
    DEBUG ((DEBUG_ERROR, "TB365FCMEM: BlockIo notify %r\n", Status));
  }

  //
  // The boot menu clears the panel later. Keep the memory summary visible in
  // debug images long enough to read it.
  //
  gBS->Stall (3 * 1000 * 1000);
#endif

  return EFI_SUCCESS;
}
