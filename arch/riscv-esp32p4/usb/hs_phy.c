/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Explicit, bounded setup of the ESP32-P4 high-speed USB UTMI PHY.
*/

#include "hs_phy.h"

#ifdef P4_USB_PHY_TEST
/* Host fixture supplies a four-register MMIO model. */
extern uint32_t p4_usb_hs_phy_test_read32(uint32_t address);
extern void p4_usb_hs_phy_test_write32(uint32_t address, uint32_t value);
#endif

/*
 * Source basis: the local ESP-IDF v6.0.1 tree, revision
 * 8c19b156084a0753687347cca1f5355782893533.
 *
 * - components/esp_hal_usb/esp32p4/include/hal/usb_utmi_ll.h:42-76 enables
 *   the HP system clock and LP UTMI reference clock, asserts HS OTG reset
 *   before PHY reset, then releases PHY before HS OTG. The code below uses
 *   masked writes to preserve other clock domains.
 * - components/esp_hal_usb/usb_utmi_hal.c:10-24 enables precise detach
 *   detection and LS parallel/keep-alive bits after the reset.
 * - components/esp_hal_usb/esp32p4/usb_dwc_periph.c:38-47 maps the HS DWC
 *   to UTMI and says it is not mapped to GPIO; GPIO26/27 belong to FS OTG.
 * - components/esp_hw_support/usb_phy/usb_phy.c:147-163 adds software D+/D-
 *   pulldown control only for P4 revision 3 and later. This file is scoped to
 *   the port's P4 revision-1.x target and intentionally does not touch it.
 *
 * Addresses and bit positions below are from the hw_ver1 register headers:
 * soc/esp32p4/register/hw_ver1/soc/reg_base.h:10-11,120,129-130;
 * hp_sys_clkrst_reg.h:373-376,489-495;
 * lp_clkrst_reg.h:928-931,939-952,974-980;
 * hp_system_reg.h:1248-1251 and hp_system_struct.h:1429-1432;
 * usb_utmi_struct.h:162-187,210-221. The USB_UTMI base is also exported by
 * soc/esp32p4/ld/esp32p4.peripherals.ld:120.
 *
 * IDF's sequence has no readiness poll or delay. This implementation has no
 * unbounded wait: it performs a fixed number of MMIO operations and checks
 * each owned field by readback. It links no IDF or FreeRTOS runtime.
 * This is source-derived implementation work; it is not hardware evidence.
 */

/* hw_ver1: DR_REG_HPPERIPH1_BASE + 0x26000, SOC_CLK_CTRL1 at +0x18. */
#define P4_USB_HS_HP_SYS_CLK_CTRL1       0x500E6018UL
/* hw_ver1: DR_REG_LPAON_BASE + 0x1000, HP_USB_CLKRST_CTRL1 at +0x48. */
#define P4_USB_HS_LP_USB_CLKRST_CTRL1    0x50111048UL
/* hw_ver1: HP system USBOTG20_CTRL at DR_REG_HP_SYS_BASE + 0x15c. */
#define P4_USB_HS_HP_USBOTG20_CTRL       0x500E515CUL
/* Linker symbol USB_UTMI at 0x5009C000; fc_06 is register offset 0x18. */
#define P4_USB_HS_UTMI_FC06              0x5009C018UL

/* Exact masks from the IDF hw_ver1 register descriptions: HP clock
   0x00010000, LP PHY reference 0x40000000, LP PHY/DWC reset 0x00000006,
   precise-detection suspendm 0x00200000, UTMI FC06 fields 0x00000009. */
#define P4_USB_HS_HP_SYS_CLK_EN          (1UL << 16)
#define P4_USB_HS_LP_PHYREF_CLK_EN       (1UL << 30)
#define P4_USB_HS_LP_PHY_RESET           (1UL << 1)
#define P4_USB_HS_LP_OTG_RESET           (1UL << 2)
#define P4_USB_HS_LP_RESET_MASK          \
    (P4_USB_HS_LP_PHY_RESET | P4_USB_HS_LP_OTG_RESET)
#define P4_USB_HS_SUSPENDM               (1UL << 21)
#define P4_USB_HS_UTMI_LS_PAR_EN         (1UL << 0)
#define P4_USB_HS_UTMI_LS_KPALV_EN       (1UL << 3)
#define P4_USB_HS_UTMI_FC06_MASK         \
    (P4_USB_HS_UTMI_LS_PAR_EN | P4_USB_HS_UTMI_LS_KPALV_EN)

#define P4_USB_HS_SNAPSHOT_VALID         (1UL << 0)
#define P4_USB_HS_UTMI_SNAPSHOT_VALID    (1UL << 1)

enum
{
    P4_USB_HS_PHASE_IDLE = 0,
    P4_USB_HS_PHASE_ACTIVE = 1,
    P4_USB_HS_PHASE_FAULTED = 2
};

static uint32_t p4_usb_hs_read(uint32_t address)
{
#ifdef P4_USB_PHY_TEST
    return p4_usb_hs_phy_test_read32(address);
#else
    uint32_t value = *(volatile uint32_t *)(uintptr_t)address;

    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return value;
#endif
}

static void p4_usb_hs_write(uint32_t address, uint32_t value)
{
#ifdef P4_USB_PHY_TEST
    p4_usb_hs_phy_test_write32(address, value);
#else
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)address = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#endif
}

/* Caller holds the supplied RCC lock. Only mask bits are changed. */
static bool p4_usb_hs_update(uint32_t address, uint32_t mask,
                             uint32_t value)
{
    uint32_t old_value = p4_usb_hs_read(address);
    uint32_t new_value = (old_value & ~mask) | (value & mask);

    if (new_value != old_value)
        p4_usb_hs_write(address, new_value);

    return (p4_usb_hs_read(address) & mask) == (value & mask);
}

static void p4_usb_hs_clear_state(p4_usb_hs_phy_t *state)
{
    state->lock_context = 0;
    state->try_rcc_lock = 0;
    state->rcc_unlock = 0;
    state->saved_hp_clock = 0;
    state->saved_lp_control = 0;
    state->saved_suspendm = 0;
    state->saved_utmi_fc06 = 0;
    state->snapshot_flags = 0;
    state->phase = P4_USB_HS_PHASE_IDLE;
}

/* Best-effort terminal state if the normal restoration path cannot verify. */
static void p4_usb_hs_force_quiesce(void)
{
    (void)p4_usb_hs_update(P4_USB_HS_HP_SYS_CLK_CTRL1,
                           P4_USB_HS_HP_SYS_CLK_EN,
                           P4_USB_HS_HP_SYS_CLK_EN);
    (void)p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                           P4_USB_HS_LP_PHYREF_CLK_EN,
                           P4_USB_HS_LP_PHYREF_CLK_EN);
    (void)p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                           P4_USB_HS_LP_OTG_RESET,
                           P4_USB_HS_LP_OTG_RESET);
    (void)p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                           P4_USB_HS_LP_PHY_RESET,
                           P4_USB_HS_LP_PHY_RESET);
}

/* Restore state snapshots. Each register update preserves unowned fields. */
static bool p4_usb_hs_restore(p4_usb_hs_phy_t *state)
{
    bool ok = true;

    if ((state->snapshot_flags & P4_USB_HS_SNAPSHOT_VALID) == 0)
        return true;

    if (state->snapshot_flags & P4_USB_HS_UTMI_SNAPSHOT_VALID)
    {
        /* Keep UTMI accessible while its saved configuration is restored. */
        if (!p4_usb_hs_update(P4_USB_HS_HP_SYS_CLK_CTRL1,
                              P4_USB_HS_HP_SYS_CLK_EN,
                              P4_USB_HS_HP_SYS_CLK_EN))
            ok = false;
        if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                              P4_USB_HS_LP_PHYREF_CLK_EN,
                              P4_USB_HS_LP_PHYREF_CLK_EN))
            ok = false;
        /* IDF asserts the DWC reset first, followed by the PHY reset. */
        if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                              P4_USB_HS_LP_OTG_RESET,
                              P4_USB_HS_LP_OTG_RESET))
            ok = false;
        if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                              P4_USB_HS_LP_PHY_RESET,
                              P4_USB_HS_LP_PHY_RESET))
            ok = false;
        if (!p4_usb_hs_update(P4_USB_HS_UTMI_FC06,
                              P4_USB_HS_UTMI_FC06_MASK,
                              state->saved_utmi_fc06))
            ok = false;
        if (!p4_usb_hs_update(P4_USB_HS_HP_USBOTG20_CTRL,
                              P4_USB_HS_SUSPENDM,
                              state->saved_suspendm))
            ok = false;
    }

    /* Setup releases PHY before DWC, so restore the saved fields in that
       order as well if the preexisting state had either reset asserted. */
    if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_PHY_RESET,
                          state->saved_lp_control))
        ok = false;
    if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_OTG_RESET,
                          state->saved_lp_control))
        ok = false;
    if (!p4_usb_hs_update(P4_USB_HS_HP_SYS_CLK_CTRL1,
                          P4_USB_HS_HP_SYS_CLK_EN,
                          state->saved_hp_clock))
        ok = false;
    if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_PHYREF_CLK_EN,
                          state->saved_lp_control))
        ok = false;

    return ok;
}

p4_usb_hs_phy_result_t p4_usb_hs_phy_setup(
    p4_usb_hs_phy_t *state,
    const p4_usb_hs_phy_platform_t *platform)
{
    uint32_t hp_clock;
    uint32_t lp_control;
    uint32_t suspendm;
    bool restored;
    void (*rcc_unlock)(void *context);
    void *lock_context;

    if (!state || !platform || !platform->try_rcc_lock ||
        !platform->rcc_unlock)
        return P4_USB_HS_PHY_INVALID;
    if (state->phase != P4_USB_HS_PHASE_IDLE)
        return P4_USB_HS_PHY_BUSY;
    if (!platform->try_rcc_lock(platform->context))
        return P4_USB_HS_PHY_BUSY;

    rcc_unlock = platform->rcc_unlock;
    lock_context = platform->context;
    state->lock_context = platform->context;
    state->try_rcc_lock = platform->try_rcc_lock;
    state->rcc_unlock = platform->rcc_unlock;

    hp_clock = p4_usb_hs_read(P4_USB_HS_HP_SYS_CLK_CTRL1);
    lp_control = p4_usb_hs_read(P4_USB_HS_LP_USB_CLKRST_CTRL1);
    suspendm = p4_usb_hs_read(P4_USB_HS_HP_USBOTG20_CTRL);
    state->saved_hp_clock = hp_clock & P4_USB_HS_HP_SYS_CLK_EN;
    state->saved_lp_control = lp_control &
        (P4_USB_HS_LP_PHYREF_CLK_EN | P4_USB_HS_LP_RESET_MASK);
    state->saved_suspendm = suspendm & P4_USB_HS_SUSPENDM;
    state->snapshot_flags = P4_USB_HS_SNAPSHOT_VALID;
    state->phase = P4_USB_HS_PHASE_FAULTED;

    /* IDF order: HP system gate, then the LP UTMI 48-MHz reference gate. */
    if (!p4_usb_hs_update(P4_USB_HS_HP_SYS_CLK_CTRL1,
                          P4_USB_HS_HP_SYS_CLK_EN,
                          P4_USB_HS_HP_SYS_CLK_EN) ||
        !p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_PHYREF_CLK_EN,
                          P4_USB_HS_LP_PHYREF_CLK_EN))
        goto failed;

    /* Save only after enabling the bus/ref clocks; no other UTMI fields are
       written, and this saved pair is restored on deinit or any later error. */
    state->saved_utmi_fc06 = p4_usb_hs_read(P4_USB_HS_UTMI_FC06) &
                              P4_USB_HS_UTMI_FC06_MASK;
    state->snapshot_flags |= P4_USB_HS_UTMI_SNAPSHOT_VALID;

    /* IDF asserts DWC reset before PHY reset, then releases PHY first. */
    if (!p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_OTG_RESET,
                          P4_USB_HS_LP_OTG_RESET) ||
        !p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_PHY_RESET,
                          P4_USB_HS_LP_PHY_RESET) ||
        !p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_PHY_RESET, 0) ||
        !p4_usb_hs_update(P4_USB_HS_LP_USB_CLKRST_CTRL1,
                          P4_USB_HS_LP_OTG_RESET, 0) ||
        !p4_usb_hs_update(P4_USB_HS_HP_USBOTG20_CTRL,
                          P4_USB_HS_SUSPENDM,
                          P4_USB_HS_SUSPENDM) ||
        !p4_usb_hs_update(P4_USB_HS_UTMI_FC06,
                          P4_USB_HS_UTMI_FC06_MASK,
                          P4_USB_HS_UTMI_FC06_MASK))
        goto failed;

    state->phase = P4_USB_HS_PHASE_ACTIVE;
    state->rcc_unlock(state->lock_context);
    return P4_USB_HS_PHY_OK;

failed:
    restored = p4_usb_hs_restore(state);
    if (!restored)
    {
        p4_usb_hs_force_quiesce();
        state->phase = P4_USB_HS_PHASE_FAULTED;
    }
    rcc_unlock(lock_context);
    if (restored)
        p4_usb_hs_clear_state(state);
    return restored ? P4_USB_HS_PHY_MMIO_ERROR : P4_USB_HS_PHY_RESTORE_ERROR;
}

p4_usb_hs_phy_result_t p4_usb_hs_phy_deinit(p4_usb_hs_phy_t *state)
{
    bool restored;
    void (*rcc_unlock)(void *context);
    void *lock_context;

    if (!state || (state->phase != P4_USB_HS_PHASE_ACTIVE &&
                   state->phase != P4_USB_HS_PHASE_FAULTED) ||
        !state->try_rcc_lock || !state->rcc_unlock)
        return P4_USB_HS_PHY_INVALID;
    if (!state->try_rcc_lock(state->lock_context))
        return P4_USB_HS_PHY_BUSY;

    rcc_unlock = state->rcc_unlock;
    lock_context = state->lock_context;
    restored = p4_usb_hs_restore(state);
    if (!restored)
    {
        p4_usb_hs_force_quiesce();
        state->phase = P4_USB_HS_PHASE_FAULTED;
    }

    rcc_unlock(lock_context);
    if (restored)
        p4_usb_hs_clear_state(state);
    return restored ? P4_USB_HS_PHY_OK : P4_USB_HS_PHY_RESTORE_ERROR;
}
