/**
  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/PrePiLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/HobLib.h>
#include <Library/StageBeacon.h>

#include "Sec.h"

// First argument (x0) of the previous stage, saved by ModuleEntryPoint.S
extern UINT64  gSecEntryArg0;

// Helper.S: clean and invalidate every data cache level by set/way
extern VOID SecCleanInvalidateDataCacheAll (VOID);

STATIC
VOID
SecTrace (
  IN CONST CHAR8  *Text
  )
{
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

#ifndef MDEPKG_NDEBUG
STATIC
VOID
ClearFrameBuffer (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Clear Memory Region
  if (FrameBufferRegion.Address != 0 && FrameBufferRegion.Length != 0) {
    // Clear everything but the console cursor stored in the last 8 bytes
    ZeroMem (
      (VOID *)FrameBufferRegion.Address,
      FrameBufferRegion.Length - 8
      );

    // Reset the console cursor stored in the last 8 bytes
    *(volatile UINT32 *)(UINTN)(FrameBufferRegion.Address + FrameBufferRegion.Length - 8) = 0;
    *(volatile UINT32 *)(UINTN)(FrameBufferRegion.Address + FrameBufferRegion.Length - 4) = 0;
  }
}

STATIC
VOID
PrintFirmwareVersion (VOID)
{
  // Clear Frame Buffer
  ClearFrameBuffer ();

  // Print UEFI Version Message
  DEBUG ((EFI_D_WARN, "\n"));
  DEBUG ((EFI_D_WARN, "Project Silicium for %a %a\n",            FixedPcdGetPtr (PcdSmbiosSystemManufacturer), FixedPcdGetPtr (PcdSmbiosSystemModel)));
  DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n", PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
  DEBUG ((EFI_D_WARN, "\n"));
}
#endif

STATIC
EFI_STATUS
InitializeMemory (VOID)
{
  EFI_STATUS                    Status;
  EFI_MEMORY_REGION_DESCRIPTOR  UefiMemoryRegion;
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;

  // Locate "DXE Heap" Memory Region
  LocateMemoryRegionByName ("DXE Heap", &UefiMemoryRegion);
  LocateMemoryRegionByName ("DXE_Heap", &UefiMemoryRegion);

  // Save UEFI Memory Details
  UINTN UefiMemoryBase = UefiMemoryRegion.Address;
  UINTN UefiMemorySize = UefiMemoryRegion.Length;

  // Verify UEFI Memory Base
  if (UefiMemoryBase == 0 && UefiMemorySize == 0) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"DXE Heap\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }

  // Initialize HOB List
  HobList = HobConstructor ((VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase, (VOID *)(UefiMemoryBase + UefiMemorySize));
  PrePeiSetHobList (HobList);

  // Initialize MMU & Memory HOBs
  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Build Stack HOB
  BuildStackHob (FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));

  return EFI_SUCCESS;
}

STATIC
VOID
SecMain (VOID)
{
  EFI_STATUS Status;
  CHAR8      TraceBuffer[128];

  // Beacon 2: C entry of SEC reached
  StageBeacon (2);

  SecTrace ("SEC 1: enter\n");

  // Do Platform Specific Initialization
  PlatformInitialize ();
  StageBeacon (3);

  SecTrace ("SEC 2: platform\n");

#ifndef MDEPKG_NDEBUG
  // Initialize Serial Port
  Status = SerialPortInitialize ();
  if (EFI_ERROR (Status)) {
    return;
  }

  // Print Firmware Version
  PrintFirmwareVersion ();
  StageBeacon (4);

  SecTrace ("SEC 3: banner\n");

  AsciiSPrint (
    TraceBuffer,
    sizeof (TraceBuffer),
    "SEC 4: fd=%lx size=%x stk=%lx\n",
    FixedPcdGet64 (PcdFdBaseAddress),
    FixedPcdGet32 (PcdFdSize),
    FixedPcdGet64 (PcdCPUCoresStackBase)
    );
  SecTrace (TraceBuffer);

  // Initialize Debug Agent
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);
#endif

  // Initialize Memory
  Status = InitializeMemory ();
  if (EFI_ERROR (Status)) {
    SecTrace ("SEC 5: memory FAIL\n");
    return;
  }

  SecTrace ("SEC 5: memory ok\n");
  StageBeacon (5);

  // Build CPU HOB
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);

  SecTrace ("SEC 6: cpu hob\n");

  // Initialize Platform HOBs
  Status = PlatformPeim ();
  if (EFI_ERROR (Status)) {
    SecTrace ("SEC 7: platform peim FAIL\n");
    return;
  }

  SecTrace ("SEC 7: platform peim ok\n");
  StageBeacon (6);

  // Pass on what the previous stage handed over in x0 (a DT address for a
  // Linux-style boot loader), so a DXE driver can use it.
  {
    UINT64  EntryArg = gSecEntryArg0;

    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));
  }

  // Run Library Cconstructors
  ProcessLibraryConstructorList ();

  SecTrace ("SEC 8: constructors\n");
  StageBeacon (7);

  // Decompress all FVs
  Status = DecompressFirstFv ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Decompress FV! Status = %r\n", Status));
    SecTrace ("SEC 9: fv FAIL\n");
    return;
  }

  SecTrace ("SEC 9: fv ok\n");
  StageBeacon (8);

  // Load DXE Core
  Status = LoadDxeCoreFromFv (NULL, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Load DXE Core! Status = %r\n", Status));
    SecTrace ("SEC10: dxe FAIL\n");
  } else {
    SecTrace ("SEC10: dxe ok\n");
  }

  // Beacon 22: LoadDxeCoreFromFv returned, DXE Core never took over
  StageBeacon (22);
}

VOID
SecEntry (VOID)
{
  // Beacon 1: C entry of SecEntry reached
  StageBeacon (1);

  // The previous stage ran with its caches on. Clean and invalidate them
  // before the HOB tables and heap are written with the MMU and caches off.
  SecCleanInvalidateDataCacheAll ();

  // Disable Data Cache
  ArmDisableDataCache ();

  // Invalidate Instruction Cache
  ArmInvalidateInstructionCache ();

  // Enable Instruction Cache
  ArmEnableInstructionCache ();

  // Write back and invalidate Stack & UEFI FD D-Cache. A plain invalidate
  // would discard what the entry code just stored there (live stack frames,
  // gSecEntryArg0) if the previous stage left the data cache on.
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdFdBaseAddress),     FixedPcdGet32 (PcdFdSize));

  // Verify Exception Vector Table
  ASSERT (((UINTN)SecVectorTable & ARM_VECTOR_TABLE_ALIGNMENT) == 0);

  // Enable new Exception Vector Table
  ArmWriteVBar ((UINTN)SecVectorTable);

  // Enter Main SEC Function
  SecMain ();
}
