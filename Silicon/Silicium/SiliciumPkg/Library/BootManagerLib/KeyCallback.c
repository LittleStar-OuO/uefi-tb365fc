/**
  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>

#include <Protocol/SimpleTextInEx.h>

#include "BootManager.h"

//
// Global Variables
//
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mSimpleInExProtocol     = NULL;
STATIC VOID                              *mNotifyHandleUp        = NULL;
STATIC VOID                              *mNotifyHandleEnter     = NULL;
STATIC BOOLEAN                            mEnterBootManager      = FALSE;

EFI_STATUS
EFIAPI
KeyNotify (IN EFI_KEY_DATA *KeyData)
{
  // Toggle Boot Manager Flag
  mEnterBootManager = TRUE;

  return EFI_SUCCESS;
}

EFI_STATUS
SetupKeypad (IN EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  EFI_STATUS                Status;
  EFI_STATUS                ConnectStatus;
  EFI_HANDLE                StiHandle;
  EFI_HANDLE                *KeypadBuffer;
  UINTN                     KeypadCount;
  VOID                      *Sti;
  EFI_DEVICE_PATH_PROTOCOL  *Path;

  // Locate Keypad Controller Protocols
  Status = gBS->LocateHandleBuffer (ByProtocol, &gKeypadDeviceProtocolGuid, NULL, &KeypadCount, &KeypadBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: no keypad handles (%r)\n", __FUNCTION__, Status));
    KeypadCount = 0;
  } else {
    DEBUG ((EFI_D_ERROR, "%a: %u keypad handle(s)\n", __FUNCTION__, (UINT32)KeypadCount));

    // Start all Keypad Controllers and take the STIEx they install on the
    // controller handle itself (the driver model installs it there, so a
    // device path lookup on the console variable is not needed).
    for (UINTN i = 0; i < KeypadCount; i++) {
      ConnectStatus = gBS->ConnectController (KeypadBuffer[i], NULL, NULL, TRUE);

      Sti    = NULL;
      Status = gBS->HandleProtocol (KeypadBuffer[i], &gEfiSimpleTextInputExProtocolGuid, &Sti);
      DEBUG ((EFI_D_ERROR, "%a: kpd %u connect=%r stiex=%r\n", __FUNCTION__, (UINT32)i, ConnectStatus, Status));
      if (!EFI_ERROR (Status) && (Sti != NULL) && (mSimpleInExProtocol == NULL)) {
        mSimpleInExProtocol = Sti;
      }
    }

    // Free Buffer
    FreePool (KeypadBuffer);
  }

  // Last resort: walk the console device path the same way the stock BDS did
  if (mSimpleInExProtocol == NULL) {
    Path   = DevicePath;
    Status = gBS->LocateDevicePath (&gEfiSimpleTextInputExProtocolGuid, &Path, &StiHandle);
    if (!EFI_ERROR (Status)) {
      Status = gBS->HandleProtocol (StiHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID **)&mSimpleInExProtocol);
    }

    DEBUG ((EFI_D_ERROR, "%a: path stiex=%r\n", __FUNCTION__, Status));
  }

  if (mSimpleInExProtocol == NULL) {
    return EFI_NOT_FOUND;
  }

  // Reset STI Device
  Status = mSimpleInExProtocol->Reset (mSimpleInExProtocol, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Reset STI Device! Status = %r\n", __FUNCTION__, Status));
  }

  return EFI_SUCCESS;
}

EFI_STATUS
RegisterKeyNotify ()
{
  EFI_STATUS   Status;
  EFI_KEY_DATA KeyData;

  // Register Volume Up Key Notify
  KeyData.KeyState.KeyToggleState = 0;
  KeyData.KeyState.KeyShiftState  = 0;
  KeyData.Key.UnicodeChar         = CHAR_NULL;
  KeyData.Key.ScanCode            = SCAN_UP;
  Status                          = mSimpleInExProtocol->RegisterKeyNotify (mSimpleInExProtocol, &KeyData, &KeyNotify, &mNotifyHandleUp);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Register Volume Up Key Notify! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  // The keypad reports a held Volume Up as Enter, accept that as well
  KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
  KeyData.Key.ScanCode    = SCAN_NULL;
  Status                  = mSimpleInExProtocol->RegisterKeyNotify (mSimpleInExProtocol, &KeyData, &KeyNotify, &mNotifyHandleEnter);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Register Enter Key Notify! Status = %r\n", __FUNCTION__, Status));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
RegisterKeyCallback (IN EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  EFI_STATUS Status;

  // Setup Keypad
  Status = SetupKeypad (DevicePath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Register Key Notify
  Status = RegisterKeyNotify ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

VOID
UnregisterKeyCallback ()
{
  EFI_STATUS Status;

  // Verify STI Protocol Presense & Key Notify
  if (mSimpleInExProtocol == NULL) {
    return;
  }

  if (mNotifyHandleUp != NULL) {
    Status = mSimpleInExProtocol->UnregisterKeyNotify (mSimpleInExProtocol, mNotifyHandleUp);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Unregister Volume Up Key Notify! Status = %r\n", __FUNCTION__, Status));
    }

    mNotifyHandleUp = NULL;
  }

  if (mNotifyHandleEnter != NULL) {
    Status = mSimpleInExProtocol->UnregisterKeyNotify (mSimpleInExProtocol, mNotifyHandleEnter);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Unregister Enter Key Notify! Status = %r\n", __FUNCTION__, Status));
    }

    mNotifyHandleEnter = NULL;
  }
}

VOID
PollBootManagerKey ()
{
  EFI_KEY_DATA KeyData;

  if (mSimpleInExProtocol == NULL) {
    return;
  }

  // Catch keys the notify callbacks could not see (for example one that was
  // still queued when the BDS wait ended).
  for (UINTN i = 0; i < 32; i++) {
    if (EFI_ERROR (mSimpleInExProtocol->ReadKeyStrokeEx (mSimpleInExProtocol, &KeyData))) {
      break;
    }

    if ((KeyData.Key.ScanCode == SCAN_UP) || (KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN)) {
      mEnterBootManager = TRUE;
    }
  }
}

BOOLEAN
EnterBootManager ()
{
  // Pass Boot Manager Flag
  return mEnterBootManager;
}
