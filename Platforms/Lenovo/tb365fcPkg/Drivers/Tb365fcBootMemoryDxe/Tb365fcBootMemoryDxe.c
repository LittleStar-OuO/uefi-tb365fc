/** @file
  Device tree and initrd memory, from the device tree the previous stage handed over.

  The previous stage (LK) passes the physical address of a flattened device tree
  in x0, which SEC keeps in a HOB. This driver:

  * maps the tree and the initrd it names into the GCD as reserved memory, so
    every access is translated and no allocator can hand the ranges out;
  * publishes the initrd as a read-only block device, so a FAT image in the boot
    image's ramdisk shows up as a normal volume for UniXNU and DarwinBoot.

  The device tree and initrd ranges come from the DT; mAvailableMemory is a
  static list that still needs the device inventory (see below).

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
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
// The start is derived from this package's own windows: stack 0x40000000 +
// 0x40000, FD 0x40080000 + 0x300000 and DXE heap 0x40380000 + 0x03C00000.
// The end is 0x80000 below the measured iomem end of 0x23fffffff; that last
// window stays out until its owner is known.
//
// REQUIRED: this single range still contains the stock reserved-memory
// carveouts (TEE, modem/DSP/SMEM, framebuffer, ramoops) that /proc/iomem
// reports as reserved. DarwinBoot receives this list, so the carveouts must be
// reconciled with the device tree before the image is used on hardware; until
// then the image MUST NOT be flashed.
//
// Keep the entries in ascending order.
//
typedef struct {
  UINT64       Base;
  UINT64       Length;
  CONST CHAR8  *Name;
} AVAILABLE_MEMORY_RANGE;

STATIC CONST AVAILABLE_MEMORY_RANGE  mAvailableMemory[] = {
  { 0x043F80000, 0x1FC080000, "placeholder-8gib" },
};

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
  Add a range to the GCD as reserved memory and map it write-back.

  Reserved keeps the firmware from allocating over it while every access still
  works; the page tables follow the GCD, so a range that is not added here is
  not necessarily reachable. A range the platform already added is left alone,
  and the attributes are only requested for the part that exists.

  @param[in] Start  First byte of the range.
  @param[in] End    End of the range, exclusive.
  @param[in] Name   Range name for the log.
**/
STATIC
EFI_STATUS
MapReserved (
  IN UINT64       Start,
  IN UINT64       End,
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

  Status = gDS->SetMemorySpaceAttributes (Base, Length, EFI_MEMORY_WB);
  if (EFI_ERROR (Status)) {
    if (Added) {
      gDS->RemoveMemorySpace (Base, Length);
    }

    if (Allocated) {
      gBS->FreePages (Base, Pages);
    }

    DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx write-back failed %r\n", Name, Base, Limit, Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "TB365FCMEM: %a 0x%lx-0x%lx mapped write-back\n", Name, Base, Limit - 1));
  return EFI_SUCCESS;
}

STATIC
VOID
AddAvailableMemory (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < ARRAY_SIZE (mAvailableMemory); Index++) {
    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeSystemMemory,
                    mAvailableMemory[Index].Base,
                    mAvailableMemory[Index].Length,
                    EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                    );
    if (!EFI_ERROR (Status)) {
      Status = gDS->SetMemorySpaceAttributes (
                      mAvailableMemory[Index].Base,
                      mAvailableMemory[Index].Length,
                      EFI_MEMORY_WB
                      );
      if (EFI_ERROR (Status)) {
        gDS->RemoveMemorySpace (mAvailableMemory[Index].Base, mAvailableMemory[Index].Length);
      }
    }

    DEBUG ((
      DEBUG_ERROR,
      "TB365FCMEM: RAM %a 0x%lx-0x%lx %r\n",
      mAvailableMemory[Index].Name,
      mAvailableMemory[Index].Base,
      mAvailableMemory[Index].Base + mAvailableMemory[Index].Length - 1,
      Status
      ));
  }
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
  Status    = MapReserved (HeaderBase, HeaderEnd, "device tree header");
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
    Status = MapReserved (HeaderEnd, ActualEnd, "device tree");
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

  Status = MapReserved (Start, Start + Size, "initrd");
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

  AddAvailableMemory ();

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
