/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include "sdio_commands.h"

static unsigned int checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expr); \
    exit(1); } } while (0)

int main(void)
{
    uint32_t arg;
    unsigned int fn, write, inc, count;
    CHECK(p4_sdio_cmd52(0, 0, 0, 0, 0, &arg) && arg == 0);
    CHECK(p4_sdio_cmd52(1, 7, 1, 0x1ffff, 255, &arg) &&
        arg == UINT32_C(0xfbfffeff));
    CHECK(p4_sdio_cmd53(0, 1, 0, 1, 0x1234, 512, 0, &arg) &&
        arg == UINT32_C(0x14246800));
    CHECK(p4_sdio_cmd53(1, 1, 1, 0, 0x1f800, 3, 512, &arg) &&
        arg == UINT32_C(0x9bf00003));
    for (fn = 0; fn < 8; ++fn)
        for (write = 0; write < 2; ++write)
            for (inc = 0; inc < 2; ++inc)
                for (count = 1; count <= 512; ++count)
                {
                    CHECK(p4_sdio_cmd53(write, fn, 0, inc, 0, count, 0, &arg));
                    CHECK(arg == ((uint32_t)write << 31 |
                        (uint32_t)fn << 28 | (uint32_t)inc << 26 |
                        (count == 512 ? 0 : count)));
                    arg = UINT32_C(0xdeadbeef);
                    CHECK(p4_sdio_cmd53(write, fn, 1, inc, 0, count, 512,
                        &arg) == (count <= 511 && (!inc || count <= 256)));
                    if (count > 511 || (inc && count > 256))
                        CHECK(arg == UINT32_C(0xdeadbeef));
                }
    for (fn = 0; fn < 8; ++fn)
        for (count = 0; count <= 255; ++count)
        {
            CHECK(p4_sdio_cmd52(1, fn, 1, 0x1234, count, &arg));
            CHECK(arg == (UINT32_C(0x88246800) | (uint32_t)fn << 28 | count));
        }
#define BAD52(w,f,x,a,d) do { arg = 0xdeadbeef; \
    CHECK(!p4_sdio_cmd52(w,f,x,a,d,&arg)); CHECK(arg == 0xdeadbeef); } while (0)
    BAD52(2,0,0,0,0); BAD52(0,8,0,0,0); BAD52(1,0,2,0,0);
    BAD52(0,0,1,0,0); BAD52(1,0,0,0x20000,0);
    BAD52(1,0,0,0,256); BAD52(0,0,0,0,1);
    CHECK(!p4_sdio_cmd52(0,0,0,0,0,NULL));
#define BAD53(w,f,b,i,a,n,s) do { arg = 0xdeadbeef; \
    CHECK(!p4_sdio_cmd53(w,f,b,i,a,n,s,&arg)); CHECK(arg == 0xdeadbeef); } while (0)
    BAD53(2,0,0,0,0,1,0); BAD53(0,8,0,0,0,1,0);
    BAD53(0,0,2,0,0,1,0); BAD53(0,0,0,2,0,1,0);
    BAD53(0,0,0,0,0x20000,1,0); BAD53(0,0,0,0,0,0,0);
    BAD53(0,0,0,0,0,513,0); BAD53(0,0,0,0,0,1,512);
    BAD53(0,0,1,0,0,1,0); BAD53(0,0,1,0,0,1,2049);
    BAD53(0,0,1,0,0,512,512); BAD53(0,0,0,1,0x1ffff,2,0);
    CHECK(p4_sdio_cmd53(0,0,0,1,0x1ffff,1,0,&arg));
    CHECK(p4_sdio_cmd53(0,0,0,0,0x1ffff,512,0,&arg));
    CHECK(!p4_sdio_cmd53(0,0,0,0,0,1,0,NULL));
    printf("PASS: %u SDIO command checks\n", checks);
    return 0;
}
