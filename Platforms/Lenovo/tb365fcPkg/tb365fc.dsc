##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
#  Lenovo TB365FC (Xiaoxin Pad 12.1 2025), MediaTek MT6835.
#
#  Every PCD marked REQUIRED is a placeholder taken from the MT6833 family and the
#  announced hardware; replace it from the stock DTB / partition dump before building
#  a firmware that is flashed to hardware.
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = tb365fc
  PLATFORM_GUID                  = 6E1D4A20-8B3C-4C57-9E64-2B7A5F0D91C3
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/tb365fcPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = tb365fcPkg/tb365fc.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include MT6835Pkg/MT6835Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  # REQUIRED: 8 GiB at 0x40000000 is the announced configuration, not a measurement.
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x200000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x40000000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"LENOVO"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Lenovo TB365FC"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"tb365fc"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Lenovo_TB365FC_tb365fc"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Lenovo TB365FC"

  #
  # Simple Frame Buffer
  #
  # REQUIRED: panel mode. 2560x1600 is the announced 12.1" panel; verify the LK
  # framebuffer geometry before relying on the console.
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|2560
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferXOffset|0

  #
  # Storage
  #
  # The stock DTB enables the UFS host (ufshci@112b0000) and disables both MSDC
  # hosts, while the package scatter carries both EMMC and UFS layouts. Keep this
  # path off until the device inventory settles the storage type; MsdcImplLib's
  # eMMC GPIO/clock tables are MT6833 values in any case.
  gMediaTekPkgTokenSpaceGuid.PcdStorageIsEMMC|FALSE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|tb365fcPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|tb365fcPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # UniXNU Libraries
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

[Components]
  #
  # Memory
  #
  tb365fcPkg/Drivers/Tb365fcBootMemoryDxe/Tb365fcBootMemoryDxe.inf

  #
  # Input
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # UniXNU
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
