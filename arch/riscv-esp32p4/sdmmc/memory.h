/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32P4_SDMMC_MEMORY_H
#define ESP32P4_SDMMC_MEMORY_H

/* This permanent boot resource cannot acquire a C-runtime startup dependency.
 * Volatile byte accesses prevent the compiler replacing these bounded loops
 * with libc calls. DMA clients may overlap the permanent bounce buffer. */
static inline void p4host_zero(void *destination, ULONG length)
{
    volatile UBYTE *out = destination;
    while (length--)
        *out++ = 0;
}

static inline void p4host_move(void *destination, const void *source, ULONG length)
{
    volatile UBYTE *out = destination;
    const volatile UBYTE *in = source;
    if ((IPTR)destination > (IPTR)source &&
        (IPTR)destination - (IPTR)source < length)
    {
        while (length--)
            out[length] = in[length];
    }
    else
    {
        while (length--)
            *out++ = *in++;
    }
}
#endif
