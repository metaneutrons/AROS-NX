/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: What has to be settled about the machine before anything else
          runs. At this stage that is the watchdogs the ROM loader leaves
          armed; the interrupt controller and the timer follow.
*/

#include <inttypes.h>

#include "hardware.h"
#include "kernel_intern.h"

static inline void reg_wr(uint32_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint32_t reg_rd(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

/*
 * A timer group's main watchdog. Clearing flashboot mode matters as much
 * as clearing the enable bit: flashboot is a separate path to a timeout
 * and survives the enable bit being cleared on its own.
 */
static void mwdt_off(uint32_t timg)
{
    uint32_t cfg;

    reg_wr(timg, P4_TIMG_WDTWPROTECT, P4_WDT_WKEY);
    cfg = reg_rd(timg, P4_TIMG_WDTCONFIG0);
    cfg &= ~(P4_TIMG_WDT_EN | P4_TIMG_WDT_FLASHBOOT);
    reg_wr(timg, P4_TIMG_WDTCONFIG0, cfg);
    reg_wr(timg, P4_TIMG_WDTWPROTECT, 0);
}

void platform_init(void)
{
    uint32_t cfg;

    mwdt_off(P4_TIMG0_BASE);
    mwdt_off(P4_TIMG1_BASE);

    /* The low power domain watchdog, the same way */
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_WPROTECT, P4_WDT_WKEY);
    cfg = reg_rd(P4_LPWDT_BASE, P4_LPWDT_CONFIG0);
    cfg &= ~(P4_LPWDT_EN | P4_LPWDT_FLASHBOOT);
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_CONFIG0, cfg);
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_WPROTECT, 0);

    /*
     * The super watchdog has no off switch. Telling it to feed itself is
     * what the ESP-IDF bootloader does with it too, and it stays that way
     * until something wants to use it deliberately.
     */
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_SWD_WPROTECT, P4_WDT_WKEY);
    cfg = reg_rd(P4_LPWDT_BASE, P4_LPWDT_SWD_CONFIG);
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_SWD_CONFIG, cfg | P4_LPWDT_SWD_AUTO_FEED);
    reg_wr(P4_LPWDT_BASE, P4_LPWDT_SWD_WPROTECT, 0);
}

/* Whether the three are actually quiet, for the bring-up report to say */
int platform_wdt_quiet(void)
{
    uint32_t t0 = reg_rd(P4_TIMG0_BASE, P4_TIMG_WDTCONFIG0);
    uint32_t t1 = reg_rd(P4_TIMG1_BASE, P4_TIMG_WDTCONFIG0);
    uint32_t lp = reg_rd(P4_LPWDT_BASE, P4_LPWDT_CONFIG0);
    uint32_t mask_t = P4_TIMG_WDT_EN | P4_TIMG_WDT_FLASHBOOT;

    return !(t0 & mask_t) && !(t1 & mask_t) &&
           !(lp & (P4_LPWDT_EN | P4_LPWDT_FLASHBOOT));
}
