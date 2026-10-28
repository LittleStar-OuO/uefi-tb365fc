/**
  Lenovo TB365FC keys for SiliciumPkg KeypadDeviceDxe.

  Three physical keys, on two different controllers:

    Volume Up     KPD matrix bit 0   (0x1C00E000, /soc/kp@1c00e000)
    Volume Down   MT6377 PMIC home key
    Power         MT6377 PMIC power key

  The DTBO keypad overlay maps hardware keycode 0 to KEY_VOLUMEUP and
  leaves every other matrix position unused, so Volume Up is the only
  key the matrix reports.  The PMIC key nodes name the other two:
  /pmic@4/mt6377keys/home has linux,keycodes = <114> (KEY_VOLUMEDOWN)
  and /pmic@4/mt6377keys/power = <116> (KEY_POWER).  LK agrees, its log
  says "pmic_detect_homekey MTK_PMIC_RST_KEY = 17": the home key is the
  reset key it maps to hardware keycode 17.

  TOPSTATUS (PMIF/SPMI register 0x22, slave 4) drops a status bit while
  its key is held: bit 1 is PWRKEY, bit 3 is HOME, bit 2 is charger
  detect.  Measured idle value is 0x0E with USB attached.  The idle
  value is sampled once, so a press is only reported for bits that read
  high when idle.

  Every key reaches the menu as ordinary UEFI input: Volume Up and Volume
  Down move the selection, Power confirms.  Volume Up keeps the bring-up
  gesture used before the PMIC keys worked:

    Volume Up tap (< 700 ms)     SCAN_UP, reported on release
    Volume Up hold 700 ms - 3 s  ENTER, reported on release
    Any key hold 3 s             power off, bypassing the menu

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/BitmapLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/MtkPmicWrapper.h>

//
// MediaTek KPD registers (same layout as the Linux mtk-kpd driver)
//
#define KP_MEM1                0x04
#define KP_DEBOUNCE            0x18
#define KP_EN                  0x24

#define KP_NUM_REGS            5

//
// TB365FC keys (DTBO keypad and MT6377 key overlays)
//
#define KP_BIT_VOLUME_UP       0

#define KP_DEBOUNCE_VALUE      0x400
#define KP_DEBOUNCE_MASK       0x3FFF

//
// MT6377 PMIC key status
//
#define MT6377_TOPSTATUS       0x22
#define MT6377_BIT_PWRKEY      BIT1
#define MT6377_BIT_HOME        BIT3

STATIC EFI_MEMORY_REGION_DESCRIPTOR mKeypadRegion;
STATIC BOOLEAN                      mKeypadAvailable;
STATIC MTK_PMIC_WRAPPER_PROTOCOL   *mPmicWrapper;
STATIC BOOLEAN                      mPmicOffline;
STATIC UINTN                        mPmicFailures;
STATIC UINT16                       mPmicBaseline;
STATIC BOOLEAN                      mPmicBaselineValid;

UINTN gBitmapScanCodes[BITMAP_NUM_WORDS(0x18)]    = {0};
UINTN gBitmapUnicodeChars[BITMAP_NUM_WORDS(0x7f)] = {0};

EFI_KEY_DATA gKeyDataPowerDown      = {.Key = {.ScanCode = SCAN_RIGHT,}};
EFI_KEY_DATA gKeyDataPowerUp        = {.Key = {.ScanCode = SCAN_LEFT,}};
EFI_KEY_DATA gKeyDataPowerLongpress = {.Key = {.ScanCode = SCAN_ESC,}};

#define MS2NS(ms) (((UINT64)(ms)) * 1000000ULL)

STATIC
inline
VOID
KeySetState (
  UINT16  ScanCode,
  CHAR16  UnicodeChar,
  BOOLEAN Value)
{
  if (ScanCode && ScanCode < 0x18) {
    if (Value) {
      BitmapSet(gBitmapScanCodes, ScanCode);
    } else {
      BitmapClear(gBitmapScanCodes, ScanCode);
    }
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (Value) {
      BitmapSet(gBitmapUnicodeChars, UnicodeChar);
    } else {
      BitmapClear(gBitmapUnicodeChars, UnicodeChar);
    }
  }
}

STATIC
inline
BOOLEAN
KeyGetState (
  UINT16 ScanCode,
  CHAR16 UnicodeChar)
{
  if (ScanCode && ScanCode < 0x18) {
    if (!BitmapTest(gBitmapScanCodes, ScanCode)) {
      return FALSE;
    }
  }

  if (UnicodeChar && UnicodeChar < 0x7f) {
    if (!BitmapTest(gBitmapUnicodeChars, UnicodeChar)) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
inline
VOID
LibKeyInitializeKeyContext (KEY_CONTEXT *Context)
{
  SetMem(&Context->KeyData, sizeof(Context->KeyData), 0);

  Context->Time      = 0;
  Context->State     = KEYSTATE_RELEASED;
  Context->Repeat    = FALSE;
  Context->Longpress = FALSE;
}

STATIC
inline
VOID
LibKeyUpdateKeyStatus (
  KEY_CONTEXT       *Context,
  KEYPAD_RETURN_API *KeypadReturnApi,
  BOOLEAN            IsPressed,
  UINT64             Delta)
{
  // Keep Track of the Actual State
  KeySetState (Context->KeyData.Key.ScanCode, Context->KeyData.Key.UnicodeChar, IsPressed);

  // Update Key Time
  Context->Time += Delta;

  switch (Context->State) {
    case KEYSTATE_RELEASED:
      if (IsPressed) {
        // Change to Pressed
        Context->Time  = 0;
        Context->State = KEYSTATE_PRESSED;
      }

      break;

    case KEYSTATE_PRESSED:
      if (IsPressed) {
        // Key Repeat
        if (Context->Repeat && Context->Time >= MS2NS(100)) {
          KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
          Context->Time   = 0;
          Context->Repeat = TRUE;
        } else if (!Context->Longpress && Context->Time >= MS2NS(500)) {
          // Handle Key Combos
          if (Context->KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            if (KeyGetState (SCAN_DOWN, 0)) {
              // Report SCAN_RIGHT
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerDown);
            } else if (KeyGetState (SCAN_UP, 0)) {
              // Report SCAN_LEFT
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerUp);
            } else {
              // Report SCAN_ESC
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerLongpress);
            }
          } else { // Post first Keyrepeat Event
            // only Start Keyrepeat if we're not doing a Combo
            if (!KeyGetState (0, CHAR_CARRIAGE_RETURN)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
              Context->Time   = 0;
              Context->Repeat = TRUE;
            }
          }

          Context->Longpress = TRUE;
        }
      } else {
        if (!Context->Longpress) {
          // We Supressed Down, so Report it Now
          KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
          Context->State = KEYSTATE_LONGPRESS_RELEASE;
        } else if (Context->Time >= MS2NS(10)) {
          // We Reported another Key Already
          Context->Time      = 0;
          Context->Repeat    = FALSE;
          Context->Longpress = FALSE;
          Context->State     = KEYSTATE_RELEASED;
        }
      }

      break;

    case KEYSTATE_LONGPRESS_RELEASE:
      // Change to Released
      Context->Time      = 0;
      Context->Repeat    = FALSE;
      Context->Longpress = FALSE;
      Context->State     = KEYSTATE_RELEASED;
      break;

    default:
      ASSERT(FALSE);
      break;
  }
}

STATIC KEY_CONTEXT KeyContextVolumeDown;

STATIC EFI_KEY_DATA mVolumeUpKeyData  = { .Key = { .ScanCode = SCAN_UP } };
STATIC BOOLEAN      mVolumeUpDown     = FALSE;
STATIC BOOLEAN      mVolumeUpShutdown = FALSE;
STATIC UINT64       mVolumeUpTime     = 0;

STATIC BOOLEAN      mPowerDown        = FALSE;
STATIC BOOLEAN      mPowerShutdown    = FALSE;
STATIC UINT64       mPowerTime        = 0;

STATIC BOOLEAN      mVolumeDownHeld      = FALSE;
STATIC BOOLEAN      mVolumeDownShutdown  = FALSE;
STATIC UINT64       mVolumeDownTime      = 0;

STATIC
VOID
LibKeyUpdateVolumeUp (
  IN KEYPAD_RETURN_API *KeypadReturnApi,
  IN BOOLEAN            IsPressed,
  IN UINT64             Delta
  )
{
  // Keep the combo state for the power key up to date
  KeySetState (SCAN_UP, 0, IsPressed);

  if (IsPressed) {
    if (!mVolumeUpDown) {
      mVolumeUpDown     = TRUE;
      mVolumeUpShutdown = FALSE;
      mVolumeUpTime     = 0;
    }

    // Already powered off once during this hold, wait for the release
    if (mVolumeUpShutdown) {
      return;
    }

    mVolumeUpTime += Delta;

    //
    // Reserved shutdown: a very long hold powers the machine off directly.
    // It is deliberately handled before the menu sees any key, so it also
    // works when the selected entry would have booted something.
    //
    if (mVolumeUpTime >= MS2NS (3000)) {
      mVolumeUpShutdown = TRUE;
      gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
      return;
    }
  } else if (mVolumeUpDown) {
    if (!mVolumeUpShutdown) {
      if (mVolumeUpTime >= MS2NS (700)) {
        // A hold acts as ENTER. It is reported on release, so that keeping
        // the key down past 3 s can still turn into the shutdown gesture.
        EFI_KEY_DATA EnterData = { .Key = { .UnicodeChar = CHAR_CARRIAGE_RETURN } };
        KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &EnterData);
      } else {
        // A tap moves the selection
        KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &mVolumeUpKeyData);
      }
    }

    mVolumeUpDown     = FALSE;
    mVolumeUpShutdown = FALSE;
    mVolumeUpTime     = 0;
  }
}

STATIC
VOID
LibKeyUpdatePower (
  IN KEYPAD_RETURN_API *KeypadReturnApi,
  IN BOOLEAN            IsPressed,
  IN UINT64             Delta
  )
{
  //
  // Report ENTER on release.  The generic key helper would turn a hold
  // of 500 ms into SCAN_ESC, which the menu treats as a cursor move.
  //
  if (IsPressed) {
    if (!mPowerDown) {
      mPowerDown     = TRUE;
      mPowerShutdown = FALSE;
      mPowerTime     = 0;
    }

    if (mPowerShutdown) {
      return;
    }

    mPowerTime += Delta;

    if (mPowerTime >= MS2NS (3000)) {
      mPowerShutdown = TRUE;
      gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
      return;
    }
  } else if (mPowerDown) {
    mPowerDown = FALSE;
    if (!mPowerShutdown && (mPowerTime < MS2NS (3000))) {
      EFI_KEY_DATA EnterData = { .Key = { .UnicodeChar = CHAR_CARRIAGE_RETURN } };
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &EnterData);
    }

    mPowerShutdown = FALSE;
    mPowerTime     = 0;
  }
}

//
// Volume Down: the generic context keeps the tap and the auto-repeat, the
// hold timer turns a 3 s hold into the same shutdown the other keys do.
//
STATIC
VOID
LibKeyUpdateVolumeDown (
  IN KEYPAD_RETURN_API *KeypadReturnApi,
  IN BOOLEAN            IsPressed,
  IN UINT64             Delta
  )
{
  if (IsPressed) {
    if (!mVolumeDownHeld) {
      mVolumeDownHeld     = TRUE;
      mVolumeDownShutdown = FALSE;
      mVolumeDownTime     = 0;
    }

    if (!mVolumeDownShutdown) {
      mVolumeDownTime += Delta;
      if (mVolumeDownTime >= MS2NS (3000)) {
        mVolumeDownShutdown = TRUE;
        gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
      }
    }
  } else {
    mVolumeDownHeld     = FALSE;
    mVolumeDownShutdown = FALSE;
    mVolumeDownTime     = 0;
  }

  //
  // Once the shutdown fired, feed the context a release so its repeat
  // stops even if ResetSystem returns.
  //
  LibKeyUpdateKeyStatus (
    &KeyContextVolumeDown,
    KeypadReturnApi,
    IsPressed && !mVolumeDownShutdown,
    Delta
    );
}

STATIC
VOID
KeypadReadState (
  OUT UINT16 *State)
{
  for (UINTN Index = 0; Index < KP_NUM_REGS; Index++) {
    State[Index] = MmioRead16 (mKeypadRegion.Address + KP_MEM1 + Index * 4);
  }
}

STATIC
BOOLEAN
KeypadIsPressed (
  IN UINT16 *State,
  IN UINT32  Bit)
{
  // A released key reads 1, a pressed key reads 0
  return ((State[Bit / 16] & (1U << (Bit % 16))) == 0);
}

STATIC
UINT16
KeypadReadPmic (
  IN UINT16  Register
  )
{
  UINT16 Status = 0;

  if (mPmicOffline) {
    return 0xFFFF;
  }

  if (mPmicWrapper == NULL) {
    if (gBS->LocateProtocol (&gMediaTekPmicWrapperProtocolGuid, NULL, (VOID **)&mPmicWrapper) != EFI_SUCCESS) {
      mPmicWrapper = NULL;
      return 0xFFFF;
    }

    DEBUG ((EFI_D_ERROR, "KPD: PMIC wrapper ready\n"));
  }

  mPmicWrapper->Read (Register, &Status);

  //
  // The wrapper reports a timeout as zero, which TOPSTATUS itself can
  // never produce. Give up on the PMIC only if it keeps failing, so a
  // transient error cannot leave the power key dead.
  //
  if (Status == 0) {
    if (++mPmicFailures >= 50) {
      mPmicOffline = TRUE;
      DEBUG ((EFI_D_ERROR, "KPD: PMIC unresponsive, keys from KPD only\n"));
    }

    return 0xFFFF;
  }

  mPmicFailures = 0;

  return Status;
}

RETURN_STATUS
EFIAPI
KeypadDeviceConstructor ()
{
  EFI_STATUS Status;

  // Locate the Keypad Registers
  Status = LocateMemoryRegionByName ("Keypad", &mKeypadRegion);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "KPD: No Keypad Memory Region! Status = %r\n", Status));
    return RETURN_SUCCESS;
  }

  mKeypadAvailable = TRUE;

  // Same set-up as the stock keypad driver and LK (KP_EN = 1, 0x400 ms)
  MmioWrite16 (mKeypadRegion.Address + KP_DEBOUNCE, KP_DEBOUNCE_VALUE & KP_DEBOUNCE_MASK);
  MmioWrite16 (mKeypadRegion.Address + KP_EN, 1);

  DEBUG ((EFI_D_ERROR, "KPD: base 0x%lx\n", (UINT64)mKeypadRegion.Address));

  return RETURN_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceReset (KEYPAD_DEVICE_PROTOCOL *This)
{
  LibKeyInitializeKeyContext(&KeyContextVolumeDown);
  KeyContextVolumeDown.KeyData.Key.ScanCode = SCAN_DOWN;

  mVolumeUpDown       = FALSE;
  mVolumeUpShutdown   = FALSE;
  mVolumeUpTime       = 0;
  mPowerDown          = FALSE;
  mPowerShutdown      = FALSE;
  mPowerTime          = 0;
  mVolumeDownHeld     = FALSE;
  mVolumeDownShutdown = FALSE;
  mVolumeDownTime     = 0;
  mPmicBaselineValid  = FALSE;

  return EFI_SUCCESS;
}

EFI_STATUS
KeypadDeviceGetKeys (
  KEYPAD_DEVICE_PROTOCOL *This,
  KEYPAD_RETURN_API      *KeypadReturnApi,
  UINT64                  Delta)
{
  UINT16  State[KP_NUM_REGS];
  UINT16  PmicStatus;
  BOOLEAN VolumeDown;
  BOOLEAN Power;

  if (!mKeypadAvailable) {
    return EFI_SUCCESS;
  }

  KeypadReadState (State);
  PmicStatus = KeypadReadPmic (MT6377_TOPSTATUS);

  //
  // The first valid TOPSTATUS sample is the idle level. A key is only
  // reported for a bit that idles high and has dropped since then.
  //
  if (!mPmicBaselineValid && (PmicStatus != 0xFFFF)) {
    mPmicBaseline      = PmicStatus;
    mPmicBaselineValid = TRUE;
  }

  VolumeDown = mPmicBaselineValid &&
               ((mPmicBaseline & MT6377_BIT_HOME) != 0) &&
               ((PmicStatus & MT6377_BIT_HOME) == 0);
  Power      = mPmicBaselineValid &&
               ((mPmicBaseline & MT6377_BIT_PWRKEY) != 0) &&
               ((PmicStatus & MT6377_BIT_PWRKEY) == 0);

  LibKeyUpdateVolumeUp (KeypadReturnApi, KeypadIsPressed (State, KP_BIT_VOLUME_UP), Delta);
  LibKeyUpdateVolumeDown (KeypadReturnApi, VolumeDown, Delta);
  LibKeyUpdatePower (KeypadReturnApi, Power, Delta);

  return EFI_SUCCESS;
}
