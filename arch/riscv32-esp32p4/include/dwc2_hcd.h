/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Synopsys DesignWare Core 2 (DWC2) USB Host Controller Driver Header
          for ESP32-P4 High-Speed USB-OTG on AROS Poseidon Stack.
*/

#ifndef RISCV32_DWC2_HCD_H
#define RISCV32_DWC2_HCD_H

#include "types.h"

/* ESP32-P4 USB-OTG DWC2 Register Base */
#define ESP32P4_USB_OTG_BASE           0x50000000UL

/* Core Global Registers */
#define DWC2_GOTGCTL                    0x0000
#define DWC2_GOTGINT                    0x0004
#define DWC2_GAHBCFG                    0x0008
#define DWC2_GUSBCFG                    0x000C
#define DWC2_GRSTCTL                    0x0010
#define DWC2_GINTSTS                    0x0014
#define DWC2_GINTMSK                    0x0018
#define DWC2_GRXSTSR                    0x001C
#define DWC2_GRXSTSP                    0x0020
#define DWC2_GRXFSIZ                    0x0024
#define DWC2_GNPTXFSIZ                  0x0028
#define DWC2_GNPTXSTS                   0x002C

/* Host Global Registers */
#define DWC2_HCFG                       0x0400
#define DWC2_HFIR                       0x0404
#define DWC2_HFNUM                      0x0408
#define DWC2_HPTXFSIZ                   0x0410
#define DWC2_HAINT                      0x0414
#define DWC2_HAINTMSK                   0x0418
#define DWC2_HPRT                       0x0440

/* Host Channel Registers (16 Channels available on ESP32-P4 DWC2) */
#define DWC2_HCCHAR(n)                  (0x0500 + (n) * 0x20)
#define DWC2_HCSPLT(n)                  (0x0504 + (n) * 0x20)
#define DWC2_HCTSIZ(n)                  (0x0508 + (n) * 0x20)
#define DWC2_HCDMA(n)                   (0x050C + (n) * 0x20)
#define DWC2_HCINT(n)                   (0x0510 + (n) * 0x20)
#define DWC2_HCINTMSK(n)                (0x0514 + (n) * 0x20)

/* HPRT Register Bits */
#define DWC2_HPRT_PRTSPD_MASK           (3 << 17)
#define DWC2_HPRT_PRTSPD_HIGH           (0 << 17)
#define DWC2_HPRT_PRTSPD_FULL           (1 << 17)
#define DWC2_HPRT_PRTSPD_LOW            (2 << 17)
#define DWC2_HPRT_PRTPWR                (1 << 12)
#define DWC2_HPRT_PRTRST                (1 << 8)
#define DWC2_HPRT_PRTENCHNG             (1 << 3)
#define DWC2_HPRT_PRTENA                (1 << 2)
#define DWC2_HPRT_PRTCONNDET            (1 << 1)
#define DWC2_HPRT_PRTCONNSTS            (1 << 0)

/* Maximum number of host channels */
#define DWC2_MAX_CHANNELS               16

/* USB Transfer Types */
#define USB_XFER_CONTROL                0
#define USB_XFER_ISOC                   1
#define USB_XFER_BULK                   2
#define USB_XFER_INTERRUPT              3

/* USB Packet IDs */
#define USB_PID_DATA0                   0
#define USB_PID_DATA2                   1
#define USB_PID_DATA1                   2
#define USB_PID_SETUP                   3

/* Transfer Request Structure */
struct dwc2_xfer_req
{
    UBYTE   dev_addr;
    UBYTE   ep_num;
    UBYTE   ep_type;
    UBYTE   ep_is_in;
    UWORD   max_packet_size;
    UBYTE   pid;
    UBYTE  *buf;
    ULONG   len;
    ULONG   actual_len;
    LONG    status;
};

/* Driver Interface */
int  dwc2_hcd_init(void);
int  dwc2_hcd_probe_root_hub(void);
void dwc2_hcd_root_hub_power(BOOL on);
void dwc2_hcd_root_hub_reset(void);
int  dwc2_hcd_submit_urb(struct dwc2_xfer_req *req);

#endif /* RISCV32_DWC2_HCD_H */
