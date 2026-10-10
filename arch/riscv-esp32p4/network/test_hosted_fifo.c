/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_fifo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    struct p4_hosted_fifo s, saved;
    struct p4_hosted_fifo_plan p, sentinel;
    uint32_t n;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    memset(&s, 0, sizeof(s));
    CHECK(!p4_hosted_fifo_rx(&s, 0, 33, &p));
    CHECK(!p4_hosted_fifo_set_pool(&s, 0, 10));
    memset(&s, 0xa5, sizeof(s)); saved = s;
    CHECK(!p4_hosted_fifo_start(&s, 7, 4096));
    CHECK(memcmp(&s, &saved, sizeof(s)) == 0);
    CHECK(!p4_hosted_fifo_start(NULL, 7, 10));
    CHECK(p4_hosted_fifo_start(&s, 7, 0));
    CHECK(!p4_hosted_fifo_tx(&s, 7, 10U << 16, 33, &p));
    CHECK(!s.poisoned && !s.busy);
    CHECK(p4_hosted_fifo_rx(&s, 7, 33, &p));
    CHECK(!p4_hosted_fifo_set_pool(&s, 7, 10));
    CHECK(p4_hosted_fifo_finish(&s, 7, 1, 1, p.bus_bytes));
    CHECK(!p4_hosted_fifo_set_pool(&s, 7, 0));
    CHECK(!p4_hosted_fifo_set_pool(&s, 7, 4096));
    CHECK(p4_hosted_fifo_set_pool(&s, 7, 10));
    CHECK(!p4_hosted_fifo_set_pool(&s, 7, 20));
    CHECK(p4_hosted_fifo_tx(&s, 7, 10U << 16, 33, &p));
    CHECK(p4_hosted_fifo_finish(&s, 7, 1, 1, p.bus_bytes));
    for (n = 1; n <= P4_HOSTED_FIFO_BUS_MAX; ++n)
    {
        CHECK(p4_hosted_fifo_start(&s, 7, 64));
        /* High bits are not part of the cumulative20bit byte counter. */
        CHECK(p4_hosted_fifo_rx(&s, 7, n | UINT32_C(0xf0000000), &p));
        CHECK(p.logical_bytes == n && p.bus_bytes == ((n + 511U) & ~511U));
        CHECK(((p.argument >> 9) & 0x1ffffU) == P4_HOSTED_FIFO_END - n);
        CHECK((p.argument >> 26) == 7U); /* read, fn1, block, increment */
        CHECK((p.argument & 511U) == p.bus_bytes / 512U);
        CHECK(!p4_hosted_fifo_rx(&s, 7, n, &p));
        CHECK(p4_hosted_fifo_finish(&s, 7, 1, 1, p.bus_bytes));
        CHECK(s.rx_consumed == n && !s.busy && !s.poisoned);
        CHECK(!p4_hosted_fifo_finish(&s, 7, 1, 1, p.bus_bytes));
        p = sentinel;
        CHECK(!p4_hosted_fifo_rx(&s, 7, n, &p));
        CHECK(memcmp(&p, &sentinel, sizeof(p)) == 0);
    }
    CHECK(p4_hosted_fifo_start(&s, 9, 64));
    s.rx_consumed = P4_HOSTED_RX_MASK - 99;
    CHECK(p4_hosted_fifo_rx(&s, 9, 100, &p));
    CHECK(p.logical_bytes == 200);
    CHECK(p4_hosted_fifo_finish(&s, 9, 1, 1, p.bus_bytes));
    CHECK(s.rx_consumed == 100);
    for (n = 1; n <= P4_HOSTED_FIFO_BUS_MAX; ++n)
    {
        CHECK(p4_hosted_fifo_start(&s, 7, 64));
        s.tx_sent = 4090;
        CHECK(p4_hosted_fifo_tx(&s, 7, 58U << 16, n, &p));
        CHECK(p.credits == (n + 1535U) / 1536U);
        CHECK(p.argument >> 31 == 1 && p.logical_bytes == n);
        CHECK(p4_hosted_fifo_finish(&s, 7, 1, 1, p.bus_bytes));
        CHECK(s.tx_sent == ((4090U + p.credits) & P4_HOSTED_TX_MASK));
        CHECK(s.rx_consumed == 0);
    }
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    p = sentinel;
    CHECK(!p4_hosted_fifo_tx(&s, 7, 1U << 16, 1537, &p));
    CHECK(!s.poisoned && !s.busy && !memcmp(&p, &sentinel, sizeof(p)));
    CHECK(p4_hosted_fifo_tx(&s, 7, 2U << 16, 1537, &p));
    CHECK(!p4_hosted_fifo_finish(&s, 7, 0, 0, 0));
    CHECK(!s.busy && !s.poisoned && s.tx_sent == 0);
    CHECK(p4_hosted_fifo_tx(&s, 7, 2U << 16, 1537, &p));
    CHECK(!p4_hosted_fifo_finish(&s, 7, 1, 0, 0));
    CHECK(s.poisoned && s.tx_sent == 0);
    CHECK(!p4_hosted_fifo_rx(&s, 7, 50, &p));
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(p4_hosted_fifo_rx(&s, 7, 33, &p));
    CHECK(!p4_hosted_fifo_finish(&s, 7, 1, 1, 33));
    CHECK(s.poisoned && s.rx_consumed == 0);
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(p4_hosted_fifo_rx(&s, 7, 33, &p));
    CHECK(!p4_hosted_fifo_finish(&s, 8, 1, 1, 512));
    CHECK(s.poisoned && s.rx_consumed == 0);
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(!p4_hosted_fifo_rx(&s, 8, 33, &p) && s.poisoned);
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(!p4_hosted_fifo_rx(&s, 7, 65537, &p) && s.poisoned);
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(!p4_hosted_fifo_tx(&s, 7, 11U << 16, 33, &p) && s.poisoned);
    CHECK(p4_hosted_fifo_start(&s, 7, 10));
    CHECK(!p4_hosted_fifo_tx(&s, 7, 1U << 16, UINT32_MAX, &p));
    CHECK(!s.poisoned && !s.busy);
    CHECK(!p4_hosted_fifo_rx(NULL, 7, 33, &p));
    CHECK(!p4_hosted_fifo_finish(NULL, 7, 1, 1, 512));
    CHECK(!p4_hosted_fifo_rx(&s, 7, 33, NULL));
    CHECK(!s.busy);
    printf("hosted FIFO: %u checks passed\n", checks);
    return 0;
}
