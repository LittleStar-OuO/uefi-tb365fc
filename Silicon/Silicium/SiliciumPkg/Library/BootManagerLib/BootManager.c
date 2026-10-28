/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/AcpiTableUpdateLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/SecureBootHandlerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/PrintLib.h>
#include <Library/StageBeacon.h>
#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>

#include <Protocol/SimpleTextOut.h>
#include <Protocol/HiiFont.h>
#include <Protocol/GraphicsOutput.h>

#include <Configuration/BootDevices.h>

#include "BootManager.h"

//
// MediaTek keypad controller, same layout as the Linux mtk-kpd driver.
// KP_MEM1 holds one bit per matrix position, 1 = released, 0 = pressed.
//
#define KPD_KP_MEM1  0x04
#define KPD_BIT_VOLUME_UP  0x0001

//
// Global Variables
//
STATIC UINTN XPos = 0;
STATIC UINTN YPos = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

STATIC
VOID
HoldBootLogo (
  VOID
  )
{
  //
  // The boot logo is the only thing on screen: hold it long enough to be
  // read before the boot option draws over it.
  //
  gBS->Stall (2 * 1000 * 1000);
}


VOID
EFIAPI
PlatformBootManagerBeforeConsole ()
{
  StageBeacon (9);

  // Signal End Of DXE
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  // Dispatch Deferred Images
  EfiBootManagerDispatchDeferredImages ();

  // Register Boot Options
  MsBootOptionsLibRegisterDefaultBootOptions ();

  // Register Console Variables
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,  NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Execute Secondary Before Console
  DeviceBootManagerBeforeConsole (NULL, NULL);

  StageBeacon (10);
}

VOID
PostGopSetup (OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **GopProtocol)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *LocalGopProtocol;

  // Verify Parameters
  if (GopProtocol == NULL) {
    return;
  }

  // Locate GOP Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID *)&LocalGopProtocol);
  if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol from Handle! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Check Status
  if (Status == EFI_UNSUPPORTED) {
    // Locate GOP Protocol
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID *)&LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol! Status = %r\n", __FUNCTION__, Status));
      return;
    }

    // Install GOP Protocol
    Status = gBS->InstallProtocolInterface (&gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE, LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Install GOP Protocol onto \"ConsoleOutHandle\"! Status = %r\n", __FUNCTION__, Status));
    }
  }

  // Pass GOP Protocol
  *GopProtocol = LocalGopProtocol;
}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  StageBeacon (11);

  // Post Setup GOP
  PostGopSetup (&GopProtocol);

  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  StageBeacon (12);

  // Hold the Project Silicium logo on the panel
  HoldBootLogo ();
  StageBeacon (13);

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  StageBeacon (14);
  if (!EFI_ERROR (Status) && GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    // Set Combo Message Position
    XPos = (ScreenWidth - AsciiStrLen (ComboMessage) * EFI_GLYPH_WIDTH) / 2;
    YPos = ScreenHeight * 48 / 50;
  }

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();
  StageBeacon (15);
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect ()
{
  // Execute Secondary On Demand
  DeviceBootManagerOnDemandConInConnect ();
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  if (BootOption != NULL) {
    DEBUG ((
      EFI_D_ERROR,
      "BDS: option %04x status %r\n",
      (UINT32)BootOption->OptionNumber,
      BootOption->Status
      ));
  }
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  BOOLEAN                      BootManagerFlag;

  StageBeacon (16);

  // Pick up a key pressed after the BDS wait
  PollBootManagerKey ();
  StageBeacon (17);

  // Unregister Key Callback
  UnregisterKeyCallback ();
  StageBeacon (18);

  // Get Boot Manager Flag
  BootManagerFlag = EnterBootManager ();
  StageBeacon (19);
  if (BootManagerFlag == FALSE) {
    return;
  }

  // Get Boot Manager Load Option
  Status = MsBootOptionsLibGetBootManagerMenu (&LoadOption, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to get Boot Manager Load Option! Status = %r", __FUNCTION__, Status));
    return;
  }

  // Execute Boot Manager Load Option
  EfiBootManagerBoot           (&LoadOption);
  EfiBootManagerFreeLoadOption (&LoadOption);
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;

  // Verify Combo Message Position
  if (XPos == 0 && YPos == 0) {
    return;
  }

  // Get Timeout Time
  UINT16 Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color
    UINT8 DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    // Set GOP Draw Color
    Color.Red = Color.Green = Color.Blue = DrawColor;

    // Print Combo Message
    AsciiPrintXY (XPos, YPos, &Color, NULL, ComboMessage);
  }
}

STATIC
VOID
PollKeypadForShutdown (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  KeypadRegion;
  UINT32                        HeldMs;

  //
  // Nothing bootable is left. The keypad driver's timer no longer runs, so
  // poll the controller here: the same long Volume Up hold that powers off
  // in the menu has to work on this screen as well.
  //
  if (EFI_ERROR (LocateMemoryRegionByName ("Keypad", &KeypadRegion))) {
    CpuDeadLoop ();
    return;
  }

  HeldMs = 0;
  for (;;) {
    if ((MmioRead16 (KeypadRegion.Address + KPD_KP_MEM1) & KPD_BIT_VOLUME_UP) == 0) {
      HeldMs += 20;
      if (HeldMs >= 3000) {
        gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        HeldMs = 0;
      }
    } else {
      HeldMs = 0;
    }

    gBS->Stall (20000);
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  EFI_STATUS Status;

  StageBeacon (20);

  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display No Boot OS Logo! Status = %r\n", __FUNCTION__, Status));
    Print (L"No Operating System Found! Please make sure that an OS is Installed.\n");
  }

  // Keep the fallback screen; a long Volume Up hold still powers off
  PollKeypadForShutdown ();
}

VOID
EFIAPI
PlatformBootManagerBdsEntry ()
{
  EFI_STATUS Status;

  // Setup Secure Boot
  Status = SetupSecureBoot ();
  ASSERT_EFI_ERROR (Status);

  // Update ACPI Tables
  UpdateAcpiTables ();

  // Execute Secondary BDS Entry
  DeviceBootManagerBdsEntry ();
}
