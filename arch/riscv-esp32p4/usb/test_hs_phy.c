/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Host regression fixtures for the ESP32-P4 HS UTMI setup sequence.
*/

#include "hs_phy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HP_CLK_ADDR   0x500E6018U
#define LP_CTRL_ADDR  0x50111048U
#define HP_USB_ADDR   0x500E515CU
#define UTMI_FC06_ADDR 0x5009C018U

#define HP_CLK_EN     (1U << 16)
#define LP_PHYREF_EN  (1U << 30)
#define LP_PHY_RESET  (1U << 1)
#define LP_OTG_RESET  (1U << 2)
#define LP_RESET_MASK (LP_PHY_RESET | LP_OTG_RESET)
#define HP_SUSPENDM   (1U << 21)
#define UTMI_LS_MASK  ((1U << 0) | (1U << 3))
#define LP_OWNED_MASK (LP_PHYREF_EN | LP_RESET_MASK)

typedef struct mmio_model
{
    uint32_t hp_clock;
    uint32_t lp_control;
    uint32_t hp_usb;
    uint32_t utmi_fc06;
    unsigned int reads;
    unsigned int writes;
    unsigned int fc06_reads;
    unsigned int corrupt_fc06_read_numbers[2];
    unsigned int corrupt_fc06_read_count;
    uint32_t corrupt_fc06_mask;
} mmio_model_t;

typedef struct lock_model
{
    bool allow;
    bool held;
    unsigned int attempts;
    unsigned int unlocks;
} lock_model_t;

static mmio_model_t mmio;

static void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "test_hs_phy:%d: check failed: %s\n", line,
                expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

uint32_t p4_usb_hs_phy_test_read32(uint32_t address)
{
    uint32_t value;
    unsigned int i;

    ++mmio.reads;
    switch (address)
    {
        case HP_CLK_ADDR:
            return mmio.hp_clock;
        case LP_CTRL_ADDR:
            return mmio.lp_control;
        case HP_USB_ADDR:
            return mmio.hp_usb;
        case UTMI_FC06_ADDR:
            value = mmio.utmi_fc06;
            ++mmio.fc06_reads;
            for (i = 0; i < mmio.corrupt_fc06_read_count; ++i)
                if (mmio.corrupt_fc06_read_numbers[i] == mmio.fc06_reads)
                    value ^= mmio.corrupt_fc06_mask;
            return value;
        default:
            fprintf(stderr, "unexpected MMIO read at 0x%08x\n", address);
            exit(EXIT_FAILURE);
    }
}

void p4_usb_hs_phy_test_write32(uint32_t address, uint32_t value)
{
    ++mmio.writes;
    switch (address)
    {
        case HP_CLK_ADDR:
            mmio.hp_clock = value;
            break;
        case LP_CTRL_ADDR:
            mmio.lp_control = value;
            break;
        case HP_USB_ADDR:
            mmio.hp_usb = value;
            break;
        case UTMI_FC06_ADDR:
            mmio.utmi_fc06 = value;
            break;
        default:
            fprintf(stderr, "unexpected MMIO write at 0x%08x\n", address);
            exit(EXIT_FAILURE);
    }
}

static bool try_lock(void *context)
{
    lock_model_t *lock = (lock_model_t *)context;

    ++lock->attempts;
    if (!lock->allow || lock->held)
        return false;
    lock->held = true;
    return true;
}

static void unlock(void *context)
{
    lock_model_t *lock = (lock_model_t *)context;

    CHECK(lock != NULL);
    CHECK(lock->held);
    lock->held = false;
    ++lock->unlocks;
}

static void model_init(lock_model_t *lock)
{
    memset(&mmio, 0, sizeof(mmio));
    mmio.hp_clock = 0xA5A40005U;
    mmio.lp_control = 0x81234568U;
    mmio.hp_usb = 0xA55A0123U;
    mmio.utmi_fc06 = 0x5A5A0020U;
    memset(lock, 0, sizeof(*lock));
    lock->allow = true;
}

static void test_setup_deinit_restores_scoped_state(void)
{
    lock_model_t lock;
    p4_usb_hs_phy_t state = P4_USB_HS_PHY_INITIALIZER;
    p4_usb_hs_phy_platform_t platform = { &lock, try_lock, unlock };
    uint32_t hp_clock;
    uint32_t lp_control;
    uint32_t hp_usb;
    uint32_t utmi_fc06;

    model_init(&lock);
    hp_clock = mmio.hp_clock;
    lp_control = mmio.lp_control;
    hp_usb = mmio.hp_usb;
    utmi_fc06 = mmio.utmi_fc06;

    CHECK(p4_usb_hs_phy_setup(&state, &platform) == P4_USB_HS_PHY_OK);
    CHECK((mmio.hp_clock & HP_CLK_EN) != 0);
    CHECK((mmio.lp_control & LP_PHYREF_EN) != 0);
    CHECK((mmio.lp_control & LP_RESET_MASK) == 0);
    CHECK((mmio.hp_usb & HP_SUSPENDM) != 0);
    CHECK((mmio.utmi_fc06 & UTMI_LS_MASK) == UTMI_LS_MASK);
    CHECK((mmio.hp_clock & ~HP_CLK_EN) == (hp_clock & ~HP_CLK_EN));
    CHECK((mmio.lp_control & ~LP_OWNED_MASK) ==
          (lp_control & ~LP_OWNED_MASK));
    CHECK((mmio.hp_usb & ~HP_SUSPENDM) == (hp_usb & ~HP_SUSPENDM));
    CHECK((mmio.utmi_fc06 & ~UTMI_LS_MASK) == (utmi_fc06 & ~UTMI_LS_MASK));
    CHECK(lock.unlocks == 1);

    CHECK(p4_usb_hs_phy_deinit(&state) == P4_USB_HS_PHY_OK);
    CHECK(mmio.hp_clock == hp_clock);
    CHECK(mmio.lp_control == lp_control);
    CHECK(mmio.hp_usb == hp_usb);
    CHECK(mmio.utmi_fc06 == utmi_fc06);
    CHECK(lock.unlocks == 2);
    CHECK(!lock.held);
}

static void test_denied_lock_has_no_mmio_effect(void)
{
    lock_model_t lock;
    p4_usb_hs_phy_t state = P4_USB_HS_PHY_INITIALIZER;
    p4_usb_hs_phy_platform_t platform = { &lock, try_lock, unlock };
    uint32_t hp_clock;
    uint32_t lp_control;
    uint32_t hp_usb;
    uint32_t utmi_fc06;

    model_init(&lock);
    lock.allow = false;
    hp_clock = mmio.hp_clock;
    lp_control = mmio.lp_control;
    hp_usb = mmio.hp_usb;
    utmi_fc06 = mmio.utmi_fc06;

    CHECK(p4_usb_hs_phy_setup(&state, &platform) == P4_USB_HS_PHY_BUSY);
    CHECK(mmio.reads == 0 && mmio.writes == 0);
    CHECK(lock.attempts == 1 && lock.unlocks == 0);
    CHECK(mmio.hp_clock == hp_clock);
    CHECK(mmio.lp_control == lp_control);
    CHECK(mmio.hp_usb == hp_usb);
    CHECK(mmio.utmi_fc06 == utmi_fc06);
}

static void test_readback_failure_restores_and_unlocks(void)
{
    lock_model_t lock;
    p4_usb_hs_phy_t state = P4_USB_HS_PHY_INITIALIZER;
    p4_usb_hs_phy_platform_t platform = { &lock, try_lock, unlock };
    uint32_t hp_clock;
    uint32_t lp_control;
    uint32_t hp_usb;
    uint32_t utmi_fc06;

    model_init(&lock);
    hp_clock = mmio.hp_clock;
    lp_control = mmio.lp_control;
    hp_usb = mmio.hp_usb;
    utmi_fc06 = mmio.utmi_fc06;
    mmio.corrupt_fc06_read_numbers[0] = 3;
    mmio.corrupt_fc06_read_count = 1;
    mmio.corrupt_fc06_mask = 1U;

    CHECK(p4_usb_hs_phy_setup(&state, &platform) ==
          P4_USB_HS_PHY_MMIO_ERROR);
    CHECK(state.phase == 0);
    CHECK(mmio.hp_clock == hp_clock);
    CHECK(mmio.lp_control == lp_control);
    CHECK(mmio.hp_usb == hp_usb);
    CHECK(mmio.utmi_fc06 == utmi_fc06);
    CHECK(lock.unlocks == 1 && !lock.held);
}

static void test_restore_failure_leaves_hs_reset_held(void)
{
    lock_model_t lock;
    p4_usb_hs_phy_t state = P4_USB_HS_PHY_INITIALIZER;
    p4_usb_hs_phy_platform_t platform = { &lock, try_lock, unlock };
    uint32_t lp_control;

    model_init(&lock);
    lp_control = mmio.lp_control;
    mmio.corrupt_fc06_read_numbers[0] = 3;
    mmio.corrupt_fc06_read_numbers[1] = 5;
    mmio.corrupt_fc06_read_count = 2;
    mmio.corrupt_fc06_mask = 1U;

    CHECK(p4_usb_hs_phy_setup(&state, &platform) ==
          P4_USB_HS_PHY_RESTORE_ERROR);
    CHECK(state.phase == 2);
    CHECK((mmio.hp_clock & HP_CLK_EN) != 0);
    CHECK((mmio.lp_control & LP_PHYREF_EN) != 0);
    CHECK((mmio.lp_control & LP_RESET_MASK) == LP_RESET_MASK);
    CHECK((mmio.lp_control & ~LP_OWNED_MASK) ==
          (lp_control & ~LP_OWNED_MASK));
    CHECK(lock.unlocks == 1 && !lock.held);

    /* A later retry can restore the pre-setup state after the readback fault. */
    mmio.corrupt_fc06_read_count = 0;
    CHECK(p4_usb_hs_phy_deinit(&state) == P4_USB_HS_PHY_OK);
    CHECK(mmio.hp_clock == 0xA5A40005U);
    CHECK(mmio.lp_control == 0x81234568U);
    CHECK(mmio.hp_usb == 0xA55A0123U);
    CHECK(mmio.utmi_fc06 == 0x5A5A0020U);
    CHECK(lock.unlocks == 2 && !lock.held);
}

int main(void)
{
    test_setup_deinit_restores_scoped_state();
    test_denied_lock_has_no_mmio_effect();
    test_readback_failure_restores_and_unlocks();
    test_restore_failure_leaves_hs_reset_held();
    puts("HS PHY host regressions passed");
    return EXIT_SUCCESS;
}
