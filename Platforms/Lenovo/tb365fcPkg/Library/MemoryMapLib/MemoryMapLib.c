/**
  Lenovo TB365FC (MediaTek MT6835) memory map.

  LK mblock/reservation layout taken from the device expdb (Android 15 /
  ZUXOS 1.1.10.122, A/B, 8 GiB):

    0x40000000  mb_kernel, 0x07C80000 (LK decompresses BootShim + FD here)
    0x47C80000  mb_dtb, 0x00400000 (LK copies the appended DTB here, x0)
    0x48080000  aee_debug_kinfo, then pstore, minirdump
    0x48200000  BL31
    0x48500000  pl-drambuf, pl-boottag
    0x50700000  aee_lk
    0x50F00000  BL33 (LK itself)
    0x62F00000  system_bl2-ext
    0x66F00000  mb_ramdisk, 0x04000000 (boot image ramdisk_addr)
    0x7FFBF000  log_store
    0xBFE00000  atf-log
    0xFCF2B000  framebuffer, 0x030D5000 (LK disp_init, 1600x2560x32)

  UEFI occupies the start of the kernel mblock:
    0x40000000  FD_BASE (Resources/Configs/tb365fc.toml), 4 MiB
    0x40400000  UEFI stack, 0x40000
    0x40440000  DXE heap up to 0x47C00000

  LK gets kernel_addr 0x40000000 from the stock vendor_boot header and
  decompresses the boot image kernel (the UEFI FD) there.

  0x100000000-0x140000000 is not listed as free by LK; keep it reserved.
  0x140000000-0x240000000 (4 GiB) is the top rank.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapLib.h>

#define TB365FC_FD_BASE            0x40000000ULL
#define TB365FC_FD_AREA_END        0x47C00000ULL
#define TB365FC_STACK_SIZE         0x00040000ULL

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"UEFI FD",            0x40000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"UEFI Stack",         0x40400000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"DXE Heap",           0x40440000, 0x077C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"DDR Before DTB",     0x47C00000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"LK DTB",             0x47C80000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"AEE Debug KInfo",    0x48080000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"PStore",             0x48090000, 0x000E0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"MiniRdDump",         0x48170000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 0",          0x48180000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"BL31",               0x48200000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 1",          0x48400000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"PL DRAMBuf",         0x48500000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"PL BootTag",         0x48600000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 2",          0x48700000, 0x08000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"AEE LK",             0x50700000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"BL33",               0x50F00000, 0x12000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"System BL2 Ext",     0x62F00000, 0x02000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 3",          0x64F00000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Boot Ramdisk",       0x66F00000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0",         0x6AF00000, 0x150BF000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Log Store",          0x7FFBF000, 0x00004000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0 Tail",    0x7FFFB000, 0x00005000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"DDR High 1",         0x80000000, 0x3FE00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"ATF Log",            0xBFE00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 2",         0xC0000000, 0x3CF2B000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Display Reserved",   0xFCF2B000, 0x030D5000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DDR Reserved 0",     0x100000000, 0x040000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Top",            0x140000000, 0x100000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Register Regions (TB365FC device tree)
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GIC Redistributor",  0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Top Ck Gen",         0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Infra Cfg AO",       0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Pericfg",            0x10003000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SPM",                0x10006000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AP Mixed",           0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UART0",              0x11001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UART1",              0x11002000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS Host",           0x112B0000, 0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS AO Cfg",         0x112B8000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS PDN Cfg",        0x112BB000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Display MMSYS",      0x14000000, 0x00020000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WatchDog Timer",     0x1C007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Keypad",             0x1C00E000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMIC Wrapper",       0x1C804000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  EFI_PHYSICAL_ADDRESS FdBase = FixedPcdGet64 (PcdFdBaseAddress);
  UINT64               FdEnd  = FdBase + FixedPcdGet32 (PcdFdSize);
  UINT64               HeapBase = FdEnd + TB365FC_STACK_SIZE;

  // The FD window comes from Resources/Configs/tb365fc.toml ([uefi_fd]); it
  // must lie inside the LK kernel mblock, at the kernel_addr of the stock
  // vendor_boot header, below the LK DTB reservation.
  ASSERT (FdBase == TB365FC_FD_BASE);
  ASSERT (FdEnd > FdBase);
  ASSERT (FdEnd == FixedPcdGet64 (PcdCPUCoresStackBase));
  ASSERT (HeapBase < TB365FC_FD_AREA_END);

  for (UINTN Index = 0; Index < ARRAY_SIZE (gMemoryDescriptor); Index++) {
    EFI_MEMORY_REGION_DESCRIPTOR *Region = &gMemoryDescriptor[Index];

    if (AsciiStrCmp (Region->Name, "UEFI FD") == 0) {
      Region->Address = FdBase;
      Region->Length  = FixedPcdGet32 (PcdFdSize);
    } else if (AsciiStrCmp (Region->Name, "UEFI Stack") == 0) {
      Region->Address = FdEnd;
    } else if (AsciiStrCmp (Region->Name, "DXE Heap") == 0) {
      Region->Address = HeapBase;
      Region->Length  = TB365FC_FD_AREA_END - HeapBase;
    }
  }

  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
