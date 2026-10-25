/* Copyright (c) 2026, The AROS Development Team.
 * Standalone, single-task polled PIO adapter; no hardware IRQ registration.
 * TinyUSB P4 rhport1 = HS, IDF controller[0] = HS. Never access FS/USJ.
 */
#ifndef DWC2_P4_AROS_H
#define DWC2_P4_AROS_H
#include "p4_usb_platform.h"
#define DWC2_EP_MAX 16
static const dwc2_controller_t _dwc2_controller[] = {
    { .reg_base = 0, .irqnum = 0, .ep_count = 0, .ep_in_count = 0, .otg_dfifo_depth = 0 },
    { .reg_base = 0x50000000UL, .irqnum = 94, .ep_count = 16,
      .ep_in_count = 8, .otg_dfifo_depth = 1024 }
};
static inline void dwc2_clock_init(uint8_t rhport, tusb_role_t role)
{
    p4_usb_owner_check();
    if (rhport != 1 || role != TUSB_ROLE_DEVICE) p4_usb_fault("wrong controller/role");
}
static inline void dwc2_int_set(uint8_t rhport, tusb_role_t role, bool enabled)
{
    dwc2_clock_init(rhport, role);
    p4_usb_poll_enable(enabled);
}
static inline void dwc2_remote_wakeup_delay(void) { p4_usb_delay(1); }
/* HS PHY lifecycle is owned by main, outside the TinyUSB core reset. */
static inline void dwc2_phy_init(dwc2_regs_t *r, uint8_t p) { (void)r; (void)p; }
static inline void dwc2_phy_update(dwc2_regs_t *r, uint8_t p) { (void)r; (void)p; }
static inline void dwc2_phy_deinit(dwc2_regs_t *r, uint8_t p) { (void)r; (void)p; }
#endif
