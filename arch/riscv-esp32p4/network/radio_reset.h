/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_RADIO_RESET_H
#define P4_WIFI_RADIO_RESET_H
#include <stdint.h>

/* Startup-only GPIO reset, outside shared SDMMC ownership. No RPC, retry,
 * firmware write or association recovery is implied by a successful pulse. */
struct p4_radio_reset_ops {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*wait_us)(void *, uint32_t);
};

/* Addresses relative to the GPIO block; IOMUX is the following 0x1000 block. */
static inline int p4_radio_reset_level(const struct p4_radio_reset_ops *ops,
                                       unsigned pin, unsigned level)
{
    uint32_t bit = UINT32_C(1) << (pin & 31u);
    uint32_t out = pin < 32u ? 0x04u : 0x10u;
    uint32_t input = pin < 32u ? 0x3cu : 0x40u;
    ops->write(ops->context, out + (level ? 4u : 8u), bit);
    return !!(ops->read(ops->context, out) & bit) == !!level &&
           !!(ops->read(ops->context, input) & bit) == !!level;
}

static inline int p4_radio_reset(const struct p4_radio_reset_ops *ops,
                                 int pin, unsigned active_level)
{
    uint32_t bit, mux, route, value, enable, out;
    int asserted, released;
    if (pin == -1)
        return 1; /* Capability absent: touch nothing. */
    if (!ops || !ops->read || !ops->write || !ops->wait_us ||
        pin < 0 || pin > 54 || active_level > 1u)
        return 0;
    bit = UINT32_C(1) << ((unsigned)pin & 31u);
    out = pin < 32 ? 0x04u : 0x10u;
    enable = pin < 32 ? 0x20u : 0x2cu;
    mux = 0x1004u + (uint32_t)pin * 4u;
    route = 0x558u + (uint32_t)pin * 4u;
    /* Preload deasserted output before selecting/enabling the pad. */
    ops->write(ops->context, out + (active_level ? 8u : 4u), bit);
    value = ops->read(ops->context, mux);
    value &= ~UINT32_C(0x7180); /* MCU_SEL, pull-up/down. */
    value |= UINT32_C(0x1200);  /* GPIO function1, input enabled. */
    ops->write(ops->context, mux, value);
    value = ops->read(ops->context, 0x74u + (uint32_t)pin * 4u);
    ops->write(ops->context, 0x74u + (uint32_t)pin * 4u,
               value & ~UINT32_C(4)); /* push-pull */
    value = ops->read(ops->context, route);
    ops->write(ops->context, route,
               (value & ~UINT32_C(0xfff)) | UINT32_C(0x500));
    ops->write(ops->context, enable + 4u, bit);
    if (!(ops->read(ops->context, enable) & bit) ||
        (ops->read(ops->context, mux) & UINT32_C(0x7000)) != 0x1000u ||
        (ops->read(ops->context, route) & UINT32_C(0xfff)) != 0x500u) {
        /* Do not leave an unverified output route driving CHIP_PU. On the
         * JC1060 the external R81 pull-up releases the disabled output. */
        ops->write(ops->context, enable + 8u, bit);
        return 0;
    }
    ops->write(ops->context, out + (active_level ? 4u : 8u), bit);
    ops->wait_us(ops->context, 200000u);
    asserted = !!(ops->read(ops->context, pin < 32 ? 0x3cu : 0x40u) & bit)
               == !!active_level;
    /* Always release, including failed assertion readback. Allow C64/R81
     * settling and C6 startup; no waits while holding the storage host. */
    ops->write(ops->context, out + (active_level ? 8u : 4u), bit);
    ops->wait_us(ops->context, 1000000u);
    released = p4_radio_reset_level(ops, (unsigned)pin, !active_level);
    return asserted && released;
}
#endif
