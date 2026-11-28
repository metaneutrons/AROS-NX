/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: kernel.resource debug output via UART0.
*/

#include <aros/kernel.h>
#include <inttypes.h>

#include <kernel_base.h>
#include <kernel_debug.h>

#include "kernel_intern.h"

int krnPutC(int chr, struct KernelBase *KernelBase)
{
    /* 0x03 is the "disable debug output" marker used by other ports */
    if (chr != 0x03)
        krnP4PutC((char)chr);

    return 1;
}
