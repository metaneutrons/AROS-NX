/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef uintptr_t IPTR;
#include "../sdmmc/memory.h"

int main(void)
{
    UBYTE actual[128], expected[128];
    unsigned source, destination, length, i, checks = 0;
    for (source = 0; source <= 64; ++source)
        for (destination = 0; destination <= 64; ++destination)
            for (length = 0; length <= 64; ++length)
            {
                for (i = 0; i < sizeof(actual); ++i)
                    actual[i] = expected[i] = (UBYTE)(i * 17U + 3U);
                memmove(expected + destination, expected + source, length);
                p4host_move(actual + destination, actual + source, length);
                if (memcmp(actual, expected, sizeof(actual)))
                    return 1;
                ++checks;
            }
    for (destination = 0; destination <= 64; ++destination)
        for (length = 0; length <= 64; ++length)
        {
            memset(actual, 0xA5, sizeof(actual));
            memset(expected, 0xA5, sizeof(expected));
            memset(expected + destination, 0, length);
            p4host_zero(actual + destination, length);
            if (memcmp(actual, expected, sizeof(actual)))
                return 1;
            ++checks;
        }
    printf("PASS: %u overlap/zero checks\n", checks);
    return 0;
}
