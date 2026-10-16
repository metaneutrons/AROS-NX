/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
 * SDIO field layout checked against ESP-IDF v6.0.1 sd_protocol_defs.h,
 * SD_ARG_CMD52_* and SD_ARG_CMD53_*. Independently implemented, not copied.
 */
#include "sdio_commands.h"

int p4_sdio_cmd52(unsigned int write, unsigned int function,
    unsigned int exchange, uint32_t address, unsigned int data,
    uint32_t *argument)
{
    if (!argument || write > 1 || function > 7 || exchange > 1 ||
        (exchange && !write) || address > UINT32_C(0x1ffff) || data > 255 ||
        (!write && data != 0))
        return 0;
    *argument = ((uint32_t)write << 31) | ((uint32_t)function << 28) |
        ((uint32_t)exchange << 27) | (address << 9) | data;
    return 1;
}

int p4_sdio_cmd53(unsigned int write, unsigned int function,
    unsigned int block_mode, unsigned int increment, uint32_t address,
    unsigned int count, unsigned int block_size, uint32_t *argument)
{
    uint32_t bytes;
    if (!argument || write > 1 || function > 7 || block_mode > 1 ||
        increment > 1 || address > UINT32_C(0x1ffff) || count == 0)
        return 0;
    if (block_mode)
    {
        if (count > 511 || block_size == 0 || block_size > 2048)
            return 0;
        bytes = (uint32_t)count * block_size;
    }
    else
    {
        /* No hidden block-size interpretation in byte mode. */
        if (count > 512 || block_size != 0)
            return 0;
        bytes = count;
    }
    if (increment && bytes > UINT32_C(0x20000) - address)
        return 0;
    *argument = ((uint32_t)write << 31) | ((uint32_t)function << 28) |
        ((uint32_t)block_mode << 27) | ((uint32_t)increment << 26) |
        (address << 9) | (count & 511U);
    return 1;
}
