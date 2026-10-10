/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_COMMAND_H
#define ESP32P4_SDMMC_COMMAND_H
#include "host.h"
#include <hardware/mmc.h>

#define P4HOST_COMMAND_TIMEOUT_US 250000UL
#define P4HOST_DATA_TIMEOUT_US 5000000UL
#define P4HOST_BUSY_TIMEOUT_US 1000000UL
#define P4HOST_COMMAND_POLLS 1000000UL

struct P4HostCommand
{
    unsigned slot;
    ULONG generation, index, argument, response_flags;
    ULONG response_error_mask; /* Client's R1/R5 policy; no 136-bit mask. */
    UBYTE *data;
    ULONG length, block_size;
    BOOL writing;
    BOOL early_cmd18; /* Storage workaround only; requires outer CMD12/13. */
};

enum P4HostCommandStatus
{
    P4HOST_COMMAND_OK,
    P4HOST_COMMAND_REFUSED,
    P4HOST_COMMAND_TIMEOUT,
    P4HOST_COMMAND_ERROR,
    P4HOST_COMMAND_CARD_ERROR,
    P4HOST_COMMAND_DMA_ERROR
};

struct P4HostCommandResult
{
    enum P4HostCommandStatus status;
    BOOL started;
    ULONG raw, idstatus;
    ULONG response[4]; /* Published only on success; MSW first for R2. */
};

/* Caller owns the resource in task context, with interrupts enabled, for
 * this call AND dependent card commands. Invalid requests cause no MMIO.
 * Global initialization and slot/card enumeration are prerequisites.
 * Success means controller/card-mask completion, not Hosted RPC acceptance.
 * RX/TX are staged in permanent resource memory. Controller and descriptor
 * byte counts are identical: length<4 or a word multiple, as in ESP-IDF's
 * P4 host. Transport must split unaligned RX tails or frame-pad TX itself;
 * this engine must not invent extra bus bytes or rounded descriptors.
 * Failure resets the prepared DMA/FIFO, not the card protocol. The client
 * must recover/confirm its card or invalidate readiness before releasing
 * its transaction. No controller-wide reset is performed here.
 * Exposed through the private resource ABI. Production SD uses this motor;
 * legacy diagnostic SD builds and the radio client are not integrated yet. */
BOOL p4host_command(struct P4SDMMCHost *host,
                    const struct P4HostCommand *request,
                    struct P4HostCommandResult *result);
#endif
