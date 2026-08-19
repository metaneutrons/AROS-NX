/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Synopsys DesignWare Core 2 (DWC2) USB Host Controller Driver Engine
          for ESP32-P4 High-Speed USB-OTG on AROS Poseidon Stack.
*/

#include "types.h"
#include "../include/dwc2_hcd.h"
#include "../boot/uart.h"

static inline void wr32(ULONG offset, ULONG val) {
    *(volatile ULONG *)(ESP32P4_USB_OTG_BASE + offset) = val;
}

static inline ULONG rd32(ULONG offset) {
    return *(volatile ULONG *)(ESP32P4_USB_OTG_BASE + offset);
}

static void dwc2_delay_ms(ULONG ms) {
    for (volatile ULONG i = 0; i < ms * 40000; i++) {
        __asm__ volatile ("nop");
    }
}

/* Core Soft Reset */
static int dwc2_core_reset(void) {
    /* Wait for AHB master idle */
    ULONG count = 0;
    while (!(rd32(DWC2_GRSTCTL) & (1 << 31))) {
        if (++count > 100000) return -1;
    }

    /* Trigger Core Soft Reset */
    wr32(DWC2_GRSTCTL, (1 << 0));
    count = 0;
    while (rd32(DWC2_GRSTCTL) & (1 << 0)) {
        if (++count > 100000) return -1;
    }

    dwc2_delay_ms(100);
    return 0;
}

int dwc2_hcd_init(void) {
    uart_puts("[usb] Initializing Synopsys DWC2 High-Speed USB Host Controller...\n");

    /* 1. Core Soft Reset */
    if (dwc2_core_reset() < 0) {
        uart_puts("[usb] ERR: DWC2 core reset timed out!\n");
        return -1;
    }

    /* 2. Configure USB Mode to Force Host (GUSBCFG: bit 29=ForceHost, bit 6=ULPI PHY) */
    ULONG usbcfg = rd32(DWC2_GUSBCFG);
    usbcfg |= (1 << 29); /* Force Host Mode */
    usbcfg &= ~(1 << 30); /* Clear Force Device */
    wr32(DWC2_GUSBCFG, usbcfg);
    dwc2_delay_ms(25);

    /* 3. Configure AHB Master and Enable DMA (GAHBCFG: bit 5=DMAEnable, bit 0=GlblIntrMask) */
    wr32(DWC2_GAHBCFG, (1 << 5) | (1 << 0) | (7 << 1)); /* 64-word burst, DMA on */

    /* 4. Configure FIFOs: RX FIFO = 512 words (2KB), Non-periodic TX = 256 words (1KB), Periodic TX = 256 words */
    wr32(DWC2_GRXFSIZ, 512);
    wr32(DWC2_GNPTXFSIZ, (256 << 16) | 512);
    wr32(DWC2_HPTXFSIZ, (256 << 16) | 768);

    /* 5. Configure Host Clock & Frame Interval (HCFG: 30/60 MHz PHY Clock) */
    wr32(DWC2_HCFG, 0x00000000); /* 30-60 MHz High-Speed clock */
    wr32(DWC2_HFIR, 60000);      /* 60,000 clock cycles per 1ms frame */

    /* 6. Power on Root Hub Port */
    dwc2_hcd_root_hub_power(TRUE);

    uart_puts("[usb] DWC2 Host Controller ready (16 Channels, DMA Active, 480 Mbps High-Speed PHY).\n");
    return 0;
}

void dwc2_hcd_root_hub_power(BOOL on) {
    ULONG hprt = rd32(DWC2_HPRT) & ~(DWC2_HPRT_PRTENA | DWC2_HPRT_PRTCONNDET | DWC2_HPRT_PRTENCHNG);
    if (on) {
        hprt |= DWC2_HPRT_PRTPWR;
    } else {
        hprt &= ~DWC2_HPRT_PRTPWR;
    }
    wr32(DWC2_HPRT, hprt);
}

void dwc2_hcd_root_hub_reset(void) {
    ULONG hprt = rd32(DWC2_HPRT) & ~(DWC2_HPRT_PRTENA | DWC2_HPRT_PRTCONNDET | DWC2_HPRT_PRTENCHNG);
    hprt |= DWC2_HPRT_PRTRST;
    wr32(DWC2_HPRT, hprt);
    dwc2_delay_ms(50);
    hprt &= ~DWC2_HPRT_PRTRST;
    wr32(DWC2_HPRT, hprt);
    dwc2_delay_ms(20);
}

int dwc2_hcd_probe_root_hub(void) {
    ULONG hprt = rd32(DWC2_HPRT);
    if (hprt & DWC2_HPRT_PRTCONNSTS) {
        ULONG speed = (hprt & DWC2_HPRT_PRTSPD_MASK);
        if (speed == DWC2_HPRT_PRTSPD_HIGH) {
            uart_puts("[usb] Root Hub: High-Speed (480 Mbps) Device Connected.\n");
        } else if (speed == DWC2_HPRT_PRTSPD_FULL) {
            uart_puts("[usb] Root Hub: Full-Speed (12 Mbps) Device Connected.\n");
        } else {
            uart_puts("[usb] Root Hub: Low-Speed (1.5 Mbps) Device Connected.\n");
        }
        return 1;
    }
    return 0;
}

/* Submit USB Request Block via Channel 0 DMA */
int dwc2_hcd_submit_urb(struct dwc2_xfer_req *req) {
    if (!req) return -1;

    int ch = 0; /* Use Channel 0 for control/bulk transfers */

    /* 1. Setup Host Channel Characteristics (HCCHAR) */
    ULONG hcchar = ((ULONG)req->dev_addr << 22) |
                   ((ULONG)(req->ep_num & 0x0F) << 11) |
                   ((ULONG)req->ep_is_in << 15) |
                   ((ULONG)req->ep_type << 18) |
                   ((ULONG)req->max_packet_size << 0);
    wr32(DWC2_HCCHAR(ch), hcchar);

    /* 2. Setup Host Channel Transfer Size (HCTSIZ) */
    ULONG pkt_cnt = (req->len + req->max_packet_size - 1) / req->max_packet_size;
    if (pkt_cnt == 0) pkt_cnt = 1;

    ULONG hctsiz = (req->len << 0) | (pkt_cnt << 19) | ((ULONG)req->pid << 29);
    wr32(DWC2_HCTSIZ(ch), hctsiz);

    /* 3. Setup DMA Address (HCDMA) */
    if (req->buf) {
        wr32(DWC2_HCDMA(ch), (ULONG)(IPTR)req->buf);
    }

    /* 4. Enable Channel to Start Transfer */
    wr32(DWC2_HCCHAR(ch), hcchar | (1 << 31)); /* Channel Enable */

    /* 5. Wait for Channel Transfer Completion */
    ULONG count = 0;
    while (rd32(DWC2_HCCHAR(ch)) & (1 << 31)) {
        if (++count > 200000) {
            req->status = -1; /* Timeout */
            return -1;
        }
    }

    req->actual_len = req->len;
    req->status = 0;
    return (int)req->actual_len;
}
