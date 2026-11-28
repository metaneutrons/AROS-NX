/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Explicit, bounded setup of the ESP32-P4 high-speed USB UTMI PHY.
*/

#ifndef ESP32P4_USB_HS_PHY_H
#define ESP32P4_USB_HS_PHY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The clock/reset registers contain fields for several unrelated blocks.
 * The caller supplies a bounded try-lock which must serialize updates to
 * those shared registers with every other clock/reset-register writer.
 * These callbacks must not sleep or depend on FreeRTOS/IDF.
 */
typedef struct p4_usb_hs_phy_platform
{
    void *context;
    bool (*try_rcc_lock)(void *context);
    void (*rcc_unlock)(void *context);
} p4_usb_hs_phy_platform_t;

typedef enum p4_usb_hs_phy_result
{
    P4_USB_HS_PHY_OK = 0,
    P4_USB_HS_PHY_INVALID = -1,
    P4_USB_HS_PHY_BUSY = -2,
    P4_USB_HS_PHY_MMIO_ERROR = -3,
    P4_USB_HS_PHY_RESTORE_ERROR = -4
} p4_usb_hs_phy_result_t;

/*
 * Caller-owned state; initialize it with P4_USB_HS_PHY_INITIALIZER.
 * It must not be copied or concurrently used while active. The implementation
 * fields are public only so the prototype requires no allocator or runtime.
 */
typedef struct p4_usb_hs_phy
{
    void *lock_context;
    bool (*try_rcc_lock)(void *context);
    void (*rcc_unlock)(void *context);
    uint32_t saved_hp_clock;
    uint32_t saved_lp_control;
    uint32_t saved_suspendm;
    uint32_t saved_utmi_fc06;
    uint32_t snapshot_flags;
    uint32_t phase;
} p4_usb_hs_phy_t;

#define P4_USB_HS_PHY_INITIALIZER { 0 }

/*
 * Enable the HS OTG system/UTMI clocks, pulse only the HS OTG and UTMI reset
 * bits, enable the P4 UTMI detach-detection signal, and set UTMI LS support.
 * This function has no effect unless called explicitly. It does not configure
 * the DWC controller, pins through GPIO, interrupts, DMA, or USB descriptors.
 *
 * The caller must have exclusive ownership of the HS OTG controller and stop
 * it before deinit. For the P4 revision-1.x mapping used by this port, HS USB
 * uses dedicated UTMI pins; the FS/USJ GPIO pins are outside this API.
 */
p4_usb_hs_phy_result_t p4_usb_hs_phy_setup(
    p4_usb_hs_phy_t *state,
    const p4_usb_hs_phy_platform_t *platform);

/* Restore the exact prior values of the fields this module owns. */
p4_usb_hs_phy_result_t p4_usb_hs_phy_deinit(p4_usb_hs_phy_t *state);

#endif /* ESP32P4_USB_HS_PHY_H */
