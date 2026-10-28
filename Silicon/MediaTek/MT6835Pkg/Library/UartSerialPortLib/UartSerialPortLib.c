/** @file
  MediaTek MT8167 UART Serial Port library.

  Adapted from mouseos/edk2-mtk (Silicon/MediaTek/MT8167Pkg/Library/
  UartSerialPortLib, commit 1982d888c765), where it drives the A04BR3 UART0
  console (0x11005000, 16550 compatible with 32-bit register spacing,
  26 MHz baud clock).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>

#define MTK_UART_BASE          0x11001000U
#define MTK_UART_CLOCK_HZ      26000000U
#define MTK_UART_RBR           (MTK_UART_BASE + 0x00U)
#define MTK_UART_THR           (MTK_UART_BASE + 0x00U)
#define MTK_UART_IER           (MTK_UART_BASE + 0x04U)
#define MTK_UART_FCR           (MTK_UART_BASE + 0x08U)
#define MTK_UART_LCR           (MTK_UART_BASE + 0x0CU)
#define MTK_UART_MCR           (MTK_UART_BASE + 0x10U)
#define MTK_UART_LSR           (MTK_UART_BASE + 0x14U)
#define MTK_UART_HIGHSPEED     (MTK_UART_BASE + 0x24U)
#define MTK_UART_ESCAPE_EN     (MTK_UART_BASE + 0x44U)
#define MTK_UART_SLEEP_EN      (MTK_UART_BASE + 0x48U)
#define MTK_UART_DMA_EN        (MTK_UART_BASE + 0x4CU)

#define MTK_UART_LCR_DLAB      BIT7
#define MTK_UART_LCR_8N1       0x03U
#define MTK_UART_LCR_CONF_B    0xBFU
#define MTK_UART_LSR_DR        BIT0
#define MTK_UART_LSR_THRE      BIT5
#define MTK_UART_FCR_INIT      (BIT0 | BIT1 | BIT2)
#define MTK_UART_MCR_DTR_RTS   (BIT0 | BIT1)
#define MTK_UART_EFR_ECB       BIT4

STATIC
RETURN_STATUS
MtkUartConfigureBaud (
  IN UINT64  BaudRate
  )
{
  UINT32  Divisor;
  UINT32  HighSpeed;
  UINT32  Quotient;
  UINT32  Remainder;

  if ((BaudRate == 0) || (BaudRate > MAX_UINT32)) {
    return RETURN_INVALID_PARAMETER;
  }

  if (BaudRate <= 115200) {
    HighSpeed = 0;
    Quotient  = 16;
  } else {
    HighSpeed = 2;
    Quotient  = 4;
  }

  Divisor   = MTK_UART_CLOCK_HZ / (Quotient * (UINT32)BaudRate);
  Remainder = MTK_UART_CLOCK_HZ % (Quotient * (UINT32)BaudRate);
  if (Remainder >= ((Quotient / 2) * (UINT32)BaudRate)) {
    Divisor++;
  }

  if ((Divisor == 0) || (Divisor > MAX_UINT16)) {
    return RETURN_UNSUPPORTED;
  }

  MmioWrite32 (MTK_UART_HIGHSPEED, HighSpeed);
  MmioWrite32 (MTK_UART_LCR, MTK_UART_LCR_8N1 | MTK_UART_LCR_DLAB);
  MmioWrite32 (MTK_UART_THR, Divisor & 0xFFU);
  MmioWrite32 (MTK_UART_IER, (Divisor >> 8) & 0xFFU);
  MmioWrite32 (MTK_UART_LCR, MTK_UART_LCR_8N1);
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  RETURN_STATUS  Status;
  UINT32         LineControl;

  MmioWrite32 (MTK_UART_IER, 0);
  Status = MtkUartConfigureBaud (FixedPcdGet64 (PcdUartDefaultBaudRate));
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  // Match the working A05BD handoff: leave UART0 in plain polled 16550 mode.
  MmioWrite32 (MTK_UART_DMA_EN, 0);
  MmioWrite32 (MTK_UART_SLEEP_EN, 0);
  MmioWrite32 (MTK_UART_ESCAPE_EN, 0);

  LineControl = MmioRead32 (MTK_UART_LCR);
  MmioWrite32 (MTK_UART_LCR, MTK_UART_LCR_CONF_B);
  MmioWrite32 (MTK_UART_FCR, MTK_UART_EFR_ECB);
  MmioWrite32 (MTK_UART_LCR, LineControl);

  MmioWrite32 (MTK_UART_FCR, MTK_UART_FCR_INIT);
  MmioWrite32 (MTK_UART_MCR, MTK_UART_MCR_DTR_RTS);
  return RETURN_SUCCESS;
}

//
// Bounded status wait. The UART clock is a top-level gate this firmware does
// not drive; a gated UART reads back LSR without THRE ever setting, and an
// unbounded wait would stop the boot on the first debug line. Dropping output
// is always better than that.
//
#define MTK_UART_WAIT_LOOPS  100000U

STATIC
BOOLEAN
MtkUartWait (
  IN UINT32  Mask
  )
{
  UINT32  Loop;

  for (Loop = 0; Loop < MTK_UART_WAIT_LOOPS; Loop++) {
    if ((MmioRead32 (MTK_UART_LSR) & Mask) != 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
MtkUartWriteByte (
  IN UINT8  Data
  )
{
  if (MtkUartWait (MTK_UART_LSR_THRE)) {
    MmioWrite32 (MTK_UART_THR, Data);
  }
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  for (Index = 0; Index < NumberOfBytes; Index++) {
    MtkUartWriteByte (Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if (!MtkUartWait (MTK_UART_LSR_DR)) {
      break;
    }

    Buffer[Index] = (UINT8)MmioRead32 (MTK_UART_RBR);
  }

  return Index;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return (BOOLEAN)((MmioRead32 (MTK_UART_LSR) & MTK_UART_LSR_DR) != 0);
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  RETURN_STATUS  Status;
  UINT64         RequestedBaud;

  if ((BaudRate == NULL) || (ReceiveFifoDepth == NULL) || (Timeout == NULL) ||
      (Parity == NULL) || (DataBits == NULL) || (StopBits == NULL))
  {
    return RETURN_INVALID_PARAMETER;
  }

  if (((*Parity != DefaultParity) && (*Parity != NoParity)) ||
      ((*DataBits != 0) && (*DataBits != 8)) ||
      ((*StopBits != DefaultStopBits) && (*StopBits != OneStopBit)))
  {
    return RETURN_UNSUPPORTED;
  }

  RequestedBaud = (*BaudRate == 0) ? FixedPcdGet64 (PcdUartDefaultBaudRate) : *BaudRate;
  Status = MtkUartConfigureBaud (RequestedBaud);
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  *BaudRate         = RequestedBaud;
  *ReceiveFifoDepth = 16;
  *Timeout          = 0;
  *Parity           = NoParity;
  *DataBits         = 8;
  *StopBits         = OneStopBit;
  return RETURN_SUCCESS;
}
