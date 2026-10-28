##
#  Lenovo TB365FC (MediaTek MT6835) UEFI platform.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = tb365fc
  PLATFORM_GUID                  = 3A7C1E54-9D2B-4F68-8C15-6B0E4A7D2F91
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/tb365fcPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = tb365fcPkg/tb365fc.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!ifndef ENABLE_SECUREBOOT
  DEFINE ENABLE_SECUREBOOT       = 0
!endif

!include MT6835Pkg/MT6835Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory: 8 GiB at 0x40000000 (see MemoryMapLib). This value only
  # divides the memory map from the register regions in MemoryInitPeiLib.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack (see MemoryMapLib): right above the 4 MiB FD at 0x40000000,
  # which is the kernel_addr LK reads from the stock vendor_boot header.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x40400000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Lenovo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"TB365FC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"tb365fc"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"TB365FC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"TB365FC"

  #
  # Simple Frame Buffer: LK already scans out 1600x2560x32 from 0xFCF2B000
  # (disp_init), so the GOP reuses that surface without a panel driver.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2560
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Storage: UFS 2.2 (the scatter file label "EMMC" is misleading; LK logs
  # "[UFS] info: ufs init OK" and the DT has ufshci@112b0000).
  #
  gMediaTekPkgTokenSpaceGuid.PcdStorageIsEMMC|FALSE

  #
  # UART0 at 0x11001000, 8N1. Not the active debug console for the first
  # bring-up (FrameBufferSerialPortLib is), kept for the serial fallback.
  #
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits|8
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity|0
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits|1

  #
  # Device Tree: publish the built-in TB365FC tree (no ACPI tables).
  #
  gEmbeddedTokenSpaceGuid.PcdDefaultDtPref|TRUE

  #
  # TEMPORARY (bring-up): the stock mask leaves DEBUG_INFO off, which hides
  # every BDS decision ("Booting ...", "Image Return Status = ..."). Turn on
  # ERROR|WARN|LOAD|INFO|VARIABLE|BM so the framebuffer console carries the
  # boot story. Restore 0x8007EE0F when bring-up is done.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000546

  #
  # Advanced Logger: the in-RAM log that UniXNU's Tools menu exports as QR
  # codes (see UniXNUPkg/Driver/AdvancedLogDxe). The model name is part of the
  # exported file name.
  #
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"TB365FC"
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerPages|256

[PcdsFeatureFlag]
  #
  # Keep the newest messages once the 128 KiB ring is full; the boot story that
  # matters is at the end.
  #
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerAutoWrapEnable|TRUE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|tb365fcPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Keypad Device
  #
  KeypadDeviceLib|tb365fcPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # UniXNU Libraries
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

  #
  # Advanced Logger: the log ring is produced in the DXE core and mirrored to a
  # hardware port that resolves through SerialPortLib, so Tools -> Advanced
  # Logger and the serial console both read the same stream
  # (Mu QemuQ35Pkg-style wiring).
  #
  AdvancedLoggerHdwPortLib|AdvLoggerPkg/Library/AdvancedLoggerHdwPortLib/AdvancedLoggerHdwPortLib.inf
  AdvancedLoggerAccessLib|AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf

#
# DXE-phase debug goes through the Advanced Logger: BaseDebugLibAdvancedLogger
# consumes AdvancedLoggerLib, which is what actually links the DxeCore producer
# into DxeMain. The logger's hardware port is UART0, so the panel is left to
# the UI instead of being overpainted by log text; SEC and PEI still use the
# framebuffer console for the stages before the menu exists.
#
[LibraryClasses.common.DXE_CORE, LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.DXE_RUNTIME_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf
  AssertLib|AdvLoggerPkg/Library/AssertLib/AssertLib.inf
  SerialPortLib|MT6835Pkg/Library/UartSerialPortLib/UartSerialPortLib.inf

[LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Runtime/AdvancedLoggerLib.inf

[LibraryClasses.common.DXE_CORE]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/DxeCore/AdvancedLoggerLib.inf

[Components]
  #
  # Serial I/O
  #
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  #
  # Keypad (KPD -> Simple Text Input)
  #
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf

  #
  # Boot image ramdisk (UniXNU payload volume)
  #
  tb365fcPkg/Drivers/BootRamdiskDxe/BootRamdiskDxe.inf

  #
  # UniXNU
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

[PcdsDynamicExDefault]
  #
  # Diagnostics: skip the boot logo background fill so the SEC stage
  # beacons stay visible under the logo (see StageBeacon.h).
  #
  gMsGraphicsPkgTokenSpaceGuid.PcdPostBackgroundColoringSkipCount|1
