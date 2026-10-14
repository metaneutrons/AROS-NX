/* Copyright (c) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_CONSOLE_TAP_H
#define P4_CONSOLE_TAP_H

#include <stdint.h>

#include <exec/nodes.h>

#include "console_ring.h"

#define P4_CONSOLE_TAP_NAME "esp32p4console.resource"
#define P4_CONSOLE_TAP_ABI  1u

/* Private, read-only ABI for the ESP32-P4 console mirror. OpenResource() does
 * not pin a resource, so the resource and its callbacks live permanently in
 * kernel code/data. The CDC consumer owns one claim at a time. Pass the same
 * owner address and nonzero session returned by claim to every operation.
 * Stale or non-owner calls fail closed. Claim starts a fresh session and
 * discards old records; there is no implicit or recovery claim and no replay
 * of output emitted before the claim. The producer drops on ring or control
 * contention and never waits for this reader. dropped() is a lifetime-
 * cumulative 32-bit count of ring-full and control-contention losses since
 * kernel startup; it is not attributed to the current session and is not reset
 * by claim/release. This API only reads records; it does not call or forward
 * through a console backend or USB stack.
 */
struct P4ConsoleTapResource
{
    struct Node node;
    uint32_t abi;
    uint32_t size;
    uint32_t (*claim)(void *owner);
    void (*release)(void *owner, uint32_t session);
    int (*pop)(void *owner, uint32_t session,
               struct p4_console_record *record);
    uint32_t (*dropped)(void *owner, uint32_t session);
};

struct ExecBase;
void krnP4ConsoleTapRegister(struct ExecBase *SysBase);

#endif /* P4_CONSOLE_TAP_H */
