/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_fifo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void test_plan_alias_refusal(void)
{
    union {
        struct p4_hosted_fifo state;
        uint8_t bytes[sizeof(struct p4_hosted_fifo) +
                      sizeof(struct p4_hosted_fifo_plan)];
    } storage;
    uint8_t before[sizeof storage];
    size_t offset;
    for (offset = 0; offset < sizeof storage.state; offset += sizeof(uint32_t))
    {
        struct p4_hosted_fifo_plan *alias =
            (struct p4_hosted_fifo_plan *)(void *)(storage.bytes + offset);
        memset(storage.bytes, 0x93, sizeof storage.bytes);
        CHECK(p4_hosted_fifo_start(&storage.state, 7, 20));
        memcpy(before, storage.bytes, sizeof before);
        CHECK(!p4_hosted_fifo_rx(&storage.state, 7, 29, alias));
        CHECK(memcmp(before, storage.bytes, sizeof before) == 0);
        CHECK(!p4_hosted_fifo_tx(&storage.state, 7, 20U << 16, 29, alias));
        CHECK(memcmp(before, storage.bytes, sizeof before) == 0);
        /* Alias refusal precedes the otherwise mutating generation check. */
        CHECK(!p4_hosted_fifo_rx(&storage.state, 8, 29, alias));
        CHECK(memcmp(before, storage.bytes, sizeof before) == 0);
        CHECK(!p4_hosted_fifo_tx(&storage.state, 8, 20U << 16, 29, alias));
        CHECK(memcmp(before, storage.bytes, sizeof before) == 0);
    }
    {
        struct {
            struct p4_hosted_fifo_plan before;
            struct p4_hosted_fifo state;
            struct p4_hosted_fifo_plan after;
        } adjacent;
        uint8_t snapshot[sizeof adjacent];
        struct p4_hosted_fifo_plan *preceding_overlap =
            (struct p4_hosted_fifo_plan *)(void *)
            ((uint8_t *)&adjacent.before + sizeof adjacent.before -
             sizeof(uint32_t));
        memset(&adjacent, 0x57, sizeof adjacent);
        CHECK(p4_hosted_fifo_start(&adjacent.state, 7, 20));
        memcpy(snapshot, &adjacent, sizeof snapshot);
        CHECK(!p4_hosted_fifo_rx(&adjacent.state, 8, 29, preceding_overlap));
        CHECK(memcmp(snapshot, &adjacent, sizeof snapshot) == 0);
        CHECK(!p4_hosted_fifo_tx(&adjacent.state, 8, 20U << 16, 29,
                                preceding_overlap));
        CHECK(memcmp(snapshot, &adjacent, sizeof snapshot) == 0);
        /* Exact adjacent ends are disjoint, not false-positive overlap. */
        CHECK(p4_hosted_fifo_rx(&adjacent.state, 7, 29, &adjacent.before));
        CHECK(p4_hosted_fifo_finish(&adjacent.state, 7, 1, 1,
                                    adjacent.before.bus_bytes));
        CHECK(p4_hosted_fifo_tx(&adjacent.state, 7, 20U << 16, 29,
                                &adjacent.after));
        CHECK(p4_hosted_fifo_finish(&adjacent.state, 7, 1, 1,
                                    adjacent.after.bus_bytes));
    }
    CHECK(!p4_hosted_fifo_tx(NULL, 7, 20U << 16, 29, NULL));
}

int main(void)
{
    struct p4_hosted_fifo s, saved;
    struct p4_hosted_fifo_plan p, sentinel;
    uint32_t n;
    test_plan_alias_refusal();
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
