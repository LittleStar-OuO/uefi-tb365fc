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
    0x40400000  Boot payload window, 16 MiB
    0x41400000  UEFI stack, 0x40000
    0x41440000  DXE heap up to 0x47C00000

  LK gets kernel_addr 0x40000000 from the stock vendor_boot header and
  decompresses the boot image kernel (the UEFI FD) there. Android 13+ with an
  init_boot partition ignores the ramdisk of the boot image, so the UniXNU
  payload is appended to the FD inside the same gzip stream; LK writes it to
  0x40400000, where BootRamdiskDxe expects it.

  0x100000000-0x140000000 is not listed as free by LK; keep it reserved.
  0x140000000-0x240000000 (4 GiB) is the top rank.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapLib.h>

#define TB365FC_FD_BASE            0x40000000ULL
#define TB365FC_FD_AREA_END        0x47C00000ULL
#define TB365FC_PAYLOAD_SIZE       0x01000000ULL
#define TB365FC_STACK_SIZE         0x00040000ULL

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"UEFI FD",            0x40000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"Boot Payload",       0x40400000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"UEFI Stack",         0x41400000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"DXE Heap",           0x41440000, 0x067C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},    // Fixed up from FD_BASE/FD_SIZE
  {"DDR Before DTB",     0x47C00000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"LK DTB",             0x47C80000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"AEE Debug KInfo",    0x48080000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"PStore",             0x48090000, 0x000E0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"MiniRdDump",         0x48170000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 0",          0x48180000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"BL31",               0x48200000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 1a",         0x48400000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Crypto HW",          0x48401000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 1b",         0x48402000, 0x000FE000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"PL DRAMBuf",         0x48500000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"PL BootTag",         0x48600000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 2",          0x48700000, 0x08000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"AEE LK",             0x50700000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"BL33",               0x50F00000, 0x12000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"System BL2 Ext",     0x62F00000, 0x02000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Mid 3",          0x64F00000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Boot Ramdisk",       0x66F00000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  // LK's fixed carveouts inside the conventional DDR runs, taken from the live
  // device tree /reserved-memory. The secure windows are invisible to a
  // non-secure accessor: a boot payload window placed over them takes the CPU
  // down on the first access, so they must never be Conv here.
  {"DDR High 0a",        0x6AF00000, 0x05100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"TEE Reserved",       0x70000000, 0x04600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0b",        0x74600000, 0x08A00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"CONSYS EMI",         0x7D000000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"WiFi EMI",           0x7D800000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0c",        0x7DE00000, 0x00680000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"GPUEB Shared",       0x7E480000, 0x00180000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"TEE Secmem",         0x7E600000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"MTK SMMU Pgd",       0x7EC00000, 0x01200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0d",        0x7FE00000, 0x000B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"MCUPM Reserved",     0x7FEB0000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 0e",        0x7FFB0000, 0x0000F000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Log Store",          0x7FFBF000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"EMI MBIST",          0x7FFFF000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 1a",        0x80000000, 0x0F000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"SCP Share",          0x8F000000, 0x00AA0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 1b",        0x8FAA0000, 0x0FEC0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"SSPM AP Shared",     0x9F960000, 0x00110000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Debug MET",          0x9FA70000, 0x00400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"SSPM Reserved",      0x9FE70000, 0x00180000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 1c",        0x9FFF0000, 0x1EB10000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"VCP Reserved",       0xBEB00000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"SCP Reserved",       0xBF300000, 0x00900000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"GZ Log",             0xBFC00000, 0x00041000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 1d",        0xBFC41000, 0x001BF000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"ATF Log",            0xBFE00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 2a",        0xC0000000, 0x3C000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"DfdMCU Dump",        0xFC000000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR High 2b",        0xFC100000, 0x00E2B000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Display Reserved",   0xFCF2B000, 0x030D5000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DDR Reserved 0",     0x100000000, 0x040000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Top",            0x140000000, 0x0FE600000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"TEE REE",            0x23E600000, 0x00C00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Unmap2",             0x23F200000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"TEE Log",            0x23F400000, 0x00020000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Top 2",          0x23F420000, 0x001E0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"GZ",                 0x23F600000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"ME CmdQ",            0x23FE00000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR Top 3",          0x23FF00000, 0x000FF000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"DramC RK1",          0x23FFFF000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},

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
  EFI_PHYSICAL_ADDRESS FdBase      = FixedPcdGet64 (PcdFdBaseAddress);
  UINT64               FdEnd       = FdBase + FixedPcdGet32 (PcdFdSize);
  UINT64               PayloadBase = FdEnd;
  UINT64               StackBase   = PayloadBase + TB365FC_PAYLOAD_SIZE;
  UINT64               HeapBase    = StackBase + TB365FC_STACK_SIZE;

  // The FD window comes from Resources/Configs/tb365fc.toml ([uefi_fd]); it
  // must lie inside the LK kernel mblock, at the kernel_addr of the stock
  // vendor_boot header, below the LK DTB reservation. The payload window
  // follows the FD; the stack and heap follow it.
  ASSERT (FdBase == TB365FC_FD_BASE);
  ASSERT (FdEnd > FdBase);
  ASSERT (StackBase == FixedPcdGet64 (PcdCPUCoresStackBase));
  ASSERT (HeapBase < TB365FC_FD_AREA_END);

  for (UINTN Index = 0; Index < ARRAY_SIZE (gMemoryDescriptor); Index++) {
    EFI_MEMORY_REGION_DESCRIPTOR *Region = &gMemoryDescriptor[Index];

    if (AsciiStrCmp (Region->Name, "UEFI FD") == 0) {
      Region->Address = FdBase;
      Region->Length  = FixedPcdGet32 (PcdFdSize);
    } else if (AsciiStrCmp (Region->Name, "Boot Payload") == 0) {
      Region->Address = PayloadBase;
      Region->Length  = TB365FC_PAYLOAD_SIZE;
    } else if (AsciiStrCmp (Region->Name, "UEFI Stack") == 0) {
      Region->Address = StackBase;
    } else if (AsciiStrCmp (Region->Name, "DXE Heap") == 0) {
      Region->Address = HeapBase;
      Region->Length  = TB365FC_FD_AREA_END - HeapBase;
    }
  }

  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
