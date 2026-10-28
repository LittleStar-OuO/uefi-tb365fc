#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/IoLib.h>

#include <Library/PmicWrapperImplLib.h>

#include <Protocol/MtkPmicWrapper.h>

typedef enum {
  WacsFsmIdle = 0,
  WacsFsmWfVldClr = 6,
} WACS_FSM_STATE;

#define WACS_TIMEOUT_US        1000

//
// SPMI command types for single-byte register accesses. LK sends
// opcode 0x38 for a register read (EXT_REG_LONG, type 0xC) and 0x30
// for a write (type 0xE); the type occupies bits 31:29.
//
#define WACS_SPMI_READ         0xC0000000
#define WACS_SPMI_WRITE        0xE0000000

STATIC EFI_MEMORY_REGION_DESCRIPTOR mPmicWrapperRegion;

STATIC
UINT32
PmicWrapperRead(
  IN  UINT16 Reg)
{
  return MmioRead32 (mPmicWrapperRegion.Address + gPlatformInfo.RegMap[Reg]);
}

STATIC
VOID
PmicWrapperWrite(
  IN  UINT16 Reg,
  IN  UINT32 Value)
{
  MmioWrite32 (mPmicWrapperRegion.Address + gPlatformInfo.RegMap[Reg], Value);
}

STATIC
WACS_FSM_STATE
WacsGetFsm ()
{
  UINT32 Value;

  // Read Data
  Value = PmicWrapperRead (PmicWrapperWacs2RData);

  // Get FSM State
  if (gPlatformInfo.ArbCapabilities) {
    Value = (Value >> 1) & 0x7;
  } else {
    Value = (Value >> 16) & 0x7;
  }

  return Value;
}

STATIC
BOOLEAN
WacsWaitFor (
  IN WACS_FSM_STATE Fsm)
{
  UINTN Elapsed;

  // Wait until FSM reaches requested state
  for (Elapsed = 0; Elapsed <= WACS_TIMEOUT_US; Elapsed += 10) {
    if (WacsGetFsm () == Fsm) {
      return TRUE;
    }

    MicroSecondDelay (10);
  }

  return FALSE;
}

STATIC
BOOLEAN
WacsCommand (
  IN UINT32  Address,
  IN UINT32  Data,
  IN BOOLEAN IsWrite)
{
  UINT32 WacsCommand;

  //
  // A transaction left waiting for its data valid clear, for instance by
  // the bootloader, would stall every later access. Drop it once.
  //
  if (WacsGetFsm () == WacsFsmWfVldClr) {
    PmicWrapperWrite (PmicWrapperWacs2VldClr, 1);
  }

  // Wait until FSM reaches idle state
  if (!WacsWaitFor (WacsFsmIdle)) {
    return FALSE;
  }

  // Encode Address, data and write mode
  if (gPlatformInfo.ArbCapabilities) {
    //
    // SPMI: bits 31:29 select the transaction, 27:24 carry the slave
    // (USID), 21:16 the byte count minus one and 15:0 the register.
    //
    if (IsWrite) {
      PmicWrapperWrite (PmicWrapperSwinf2WData31, Data);
      WacsCommand = WACS_SPMI_WRITE;
    } else {
      WacsCommand = WACS_SPMI_READ;
    }

    WacsCommand |= (UINT32)gPlatformInfo.SlaveId << 24;
    WacsCommand |= Address & 0xFFFF;
  } else {
    WacsCommand = (IsWrite << 31) | ((Address >> 1) << 16) | Data;
  }

  // Write Command
  PmicWrapperWrite (PmicWrapperWacs2Cmd, WacsCommand);

  return TRUE;
}

STATIC
VOID
PmicWrapperImplRead (
  IN  UINT16  Address,
  OUT UINT16 *Value)
{
  UINT32 Result;

  *Value = 0;

  // Send read transaction and wait until result become valid
  if (!WacsCommand (Address, 0, FALSE)) {
    return;
  }

  if (!WacsWaitFor (WacsFsmWfVldClr)) {
    PmicWrapperWrite (PmicWrapperWacs2VldClr, 1);
    return;
  }

  // Read data
  Result = PmicWrapperRead (gPlatformInfo.ArbCapabilities ? PmicWrapperSwinf2RData31 : PmicWrapperWacs2RData);
  PmicWrapperWrite (PmicWrapperWacs2VldClr, 1);

  *Value = (Result & 0xFFFF);
}

STATIC
VOID
PmicWrapperImplWrite (
  IN  UINT16 Address,
  OUT UINT16 Value)
{
  // Send write transaction
  WacsCommand (Address, Value, TRUE);
}

STATIC MTK_PMIC_WRAPPER_PROTOCOL mPmicWrapper = {
  PmicWrapperImplRead,
  PmicWrapperImplWrite
};

EFI_STATUS
EFIAPI
InitPmicWrapper (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  UINT32     InitState;

  // Locate PMIC Wrapper Memory Region
  Status = LocateMemoryRegionByName ("PMIC Wrapper", &mPmicWrapperRegion);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate PMIC Wrapper Memory Region! Status = %r\n", Status));
    return Status;
  }

  // Read init state of PMIC Wrapper
  if (!gPlatformInfo.ArbCapabilities) {
    InitState = PmicWrapperRead (PmicWrapperInitDone2);
    if (InitState != 1) {
      DEBUG ((EFI_D_ERROR, "PMIC Wrapper Not Initialized!\n"));
    }
  }

  // Register PMIC Wrapper Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gMediaTekPmicWrapperProtocolGuid, &mPmicWrapper, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register PMIC Wrapper Protocol! Status = %r\n", Status));
    return Status;
  }

  return Status;
}