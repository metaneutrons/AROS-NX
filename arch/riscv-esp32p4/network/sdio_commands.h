/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SDIO_COMMANDS_H
#define P4_SDIO_COMMANDS_H

#include <stdint.h>

/* Pure command construction: no MMIO, allocation or controller ownership.
 * Invalid inputs leave the output unchanged. Booleans must be 0 or 1.
 * CMD53 block counts are finite (1..511); zero means an unbounded block
 * transfer and is deliberately excluded. Byte count 512 is encoded as zero.
 * The caller must independently check function availability, negotiated block
 * size, DMA limits/alignment and exclusive ownership of the shared host.
 */
int p4_sdio_cmd52(unsigned int write, unsigned int function,
    unsigned int exchange, uint32_t address, unsigned int data,
    uint32_t *argument);
int p4_sdio_cmd53(unsigned int write, unsigned int function,
    unsigned int block_mode, unsigned int increment, uint32_t address,
    unsigned int count, unsigned int block_size, uint32_t *argument);

#endif
