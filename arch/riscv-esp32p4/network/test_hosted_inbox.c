/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_inbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static struct p4_hosted_rpc event_rpc(uint32_t msg_id, uint32_t uid,
                                     const uint8_t *payload, size_t size)
{
    struct p4_hosted_rpc rpc;

    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.msg_id = msg_id;
    rpc.uid = uid;
    rpc.payload = payload;
    rpc.payload_size = size;
    return rpc;
}

static void check_queue_unchanged(const struct p4_hosted_inbox *inbox,
                                  const struct p4_hosted_inbox *snapshot)
{
    CHECK(memcmp(inbox, snapshot, sizeof(*inbox)) == 0);
}

static void check_item_zero(const struct p4_hosted_inbox_item *item)
{
    const uint8_t *bytes = (const uint8_t *)item;
    size_t i;

    for (i = 0; i < sizeof(*item); ++i) {
        if (bytes[i] != 0) {
            CHECK(0);
            return;
        }
    }
    CHECK(1);
}

static void check_items_zero(const struct p4_hosted_inbox *inbox)
{
    size_t i;

    for (i = 0; i < P4_HOSTED_INBOX_CAPACITY; ++i)
        check_item_zero(&inbox->items[i]);
}

static void test_init_and_epoch_replacement(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox before;
    uint8_t payload[] = {0x31, 0x32, 0x33};
    struct p4_hosted_rpc rpc = event_rpc(769u, 0, payload, sizeof(payload));

    CHECK(p4_hosted_inbox_init(&inbox, 11u) == 1);
    CHECK(inbox.initialized == 1u && inbox.generation == 11u);
    CHECK(inbox.head == 0 && inbox.count == 0 && inbox.poisoned == 0u);
    check_items_zero(&inbox);
    CHECK(p4_hosted_inbox_push(&inbox, 11u, &rpc) == 1);
    memcpy(&before, &inbox, sizeof(inbox));
    CHECK(p4_hosted_inbox_init(&inbox, 11u) == 0);
    check_queue_unchanged(&inbox, &before);
    CHECK(p4_hosted_inbox_init(&inbox, 0) == 0);
    check_queue_unchanged(&inbox, &before);

    CHECK(p4_hosted_inbox_init(&inbox, 12u) == 1);
    CHECK(inbox.generation == 12u && inbox.head == 0 && inbox.count == 0);
    CHECK(inbox.initialized == 1u && inbox.poisoned == 0u);
    check_items_zero(&inbox);
}

static void test_fifo_wrap_and_owned_copy(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox_item out;
    uint8_t bytes[4][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9},
                           {10, 11, 12}};
    struct p4_hosted_rpc rpc;
    unsigned i;
    const uint32_t ids[] = {769u, 770u, 771u, 772u, 773u, 774u};

    CHECK(p4_hosted_inbox_init(&inbox, 20u) == 1);
    for (i = 0; i < 4; ++i) {
        rpc = event_rpc(ids[i], i, bytes[i], sizeof(bytes[i]));
        CHECK(p4_hosted_inbox_push(&inbox, 20u, &rpc) == 1);
    }
    CHECK(inbox.count == 4u && inbox.head == 0u);

    memset(&out, 0xa5, sizeof(out));
    CHECK(p4_hosted_inbox_pop(&inbox, 20u, &out) == 1);
    CHECK(out.msg_id == ids[0] && out.uid == 0u);
    CHECK(out.payload_size == sizeof(bytes[0]) &&
          memcmp(out.payload, bytes[0], sizeof(bytes[0])) == 0);
    CHECK(inbox.count == 3u && inbox.head == 1u);
    check_item_zero(&inbox.items[0]);
    CHECK(p4_hosted_inbox_pop(&inbox, 20u, &out) == 1);
    CHECK(out.msg_id == ids[1] && out.uid == 1u);

    rpc = event_rpc(ids[4], 4u, bytes[0], sizeof(bytes[0]));
    CHECK(p4_hosted_inbox_push(&inbox, 20u, &rpc) == 1);
    rpc = event_rpc(ids[5], 5u, bytes[1], sizeof(bytes[1]));
    CHECK(p4_hosted_inbox_push(&inbox, 20u, &rpc) == 1);
    CHECK(inbox.head == 2u && inbox.count == 4u);

    for (i = 2; i < 6; ++i) {
        CHECK(p4_hosted_inbox_pop(&inbox, 20u, &out) == 1);
        CHECK(out.msg_id == ids[i] && out.uid == i);
    }
    CHECK(inbox.count == 0u && inbox.head == 2u);

    memset(&out, 0x5a, sizeof(out));
    {
        struct p4_hosted_inbox_item before;
        memcpy(&before, &out, sizeof(out));
        CHECK(p4_hosted_inbox_pop(&inbox, 20u, &out) == 0);
        CHECK(memcmp(&out, &before, sizeof(out)) == 0);
    }

    /* Source bytes and descriptor may change after a successful push. */
    bytes[2][0] = 0x41;
    bytes[2][1] = 0x42;
    bytes[2][2] = 0x43;
    rpc = event_rpc(776u, 99u, bytes[2], sizeof(bytes[2]));
    CHECK(p4_hosted_inbox_push(&inbox, 20u, &rpc) == 1);
    memset(bytes[2], 0xee, sizeof(bytes[2]));
    rpc.msg_id = 769u;
    rpc.uid = 0;
    rpc.payload_size = 0;
    CHECK(p4_hosted_inbox_pop(&inbox, 20u, &out) == 1);
    CHECK(out.msg_id == 776u && out.uid == 99u);
    CHECK(out.payload_size == 3u && out.payload[0] == 0x41 &&
          out.payload[1] == 0x42 && out.payload[2] == 0x43);
}

static void test_response_and_payload_boundaries(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox_item out;
    static uint8_t maximum[P4_HOSTED_RPC_MAX_SIZE];
    struct p4_hosted_rpc rpc;
    size_t i;

    for (i = 0; i < sizeof(maximum); ++i)
        maximum[i] = (uint8_t)(i ^ 0x6du);
    CHECK(p4_hosted_inbox_init(&inbox, 30u) == 1);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = 513u;
    rpc.uid = 1u;
    rpc.payload = maximum;
    rpc.payload_size = sizeof(maximum);
    CHECK(p4_hosted_inbox_push(&inbox, 30u, &rpc) == 1);
    CHECK(p4_hosted_inbox_pop(&inbox, 30u, &out) == 1);
    CHECK(out.msg_type == P4_HOSTED_RPC_TYPE_RESPONSE && out.msg_id == 513u);
    CHECK(out.uid == 1u && out.payload_size == P4_HOSTED_RPC_MAX_SIZE);
    CHECK(memcmp(out.payload, maximum, sizeof(maximum)) == 0);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.msg_id = 769u;
    rpc.uid = 0u;
    rpc.payload = NULL;
    rpc.payload_size = 0;
    CHECK(p4_hosted_inbox_push(&inbox, 30u, &rpc) == 1);
    CHECK(p4_hosted_inbox_pop(&inbox, 30u, &out) == 1);
    CHECK(out.msg_type == P4_HOSTED_RPC_TYPE_EVENT && out.uid == 0u);
    CHECK(out.payload_size == 0u);
    CHECK(out.payload[0] == 0u && out.payload[P4_HOSTED_RPC_MAX_SIZE - 1u] == 0u);
}

static void expect_bad_rpc_unchanged(struct p4_hosted_inbox *inbox,
                                     const struct p4_hosted_rpc *rpc)
{
    struct p4_hosted_inbox before;

    memcpy(&before, inbox, sizeof(before));
    CHECK(p4_hosted_inbox_push(inbox, inbox->generation, rpc) == 0);
    check_queue_unchanged(inbox, &before);
}

static void test_reject_invalid_envelopes(void)
{
    struct p4_hosted_inbox inbox = {0};
    uint8_t one = 0x7c;
    struct p4_hosted_rpc rpc;

    CHECK(p4_hosted_inbox_init(&inbox, 40u) == 1);
    rpc = event_rpc(769u, 0u, &one, 1u);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_REQUEST;
    expect_bad_rpc_unchanged(&inbox, &rpc);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_INVALID;
    expect_bad_rpc_unchanged(&inbox, &rpc);
    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.msg_id = 768u;
    expect_bad_rpc_unchanged(&inbox, &rpc);
    rpc.msg_id = 777u;
    expect_bad_rpc_unchanged(&inbox, &rpc);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_RESPONSE;
    rpc.msg_id = 512u;
    rpc.uid = 1u;
    expect_bad_rpc_unchanged(&inbox, &rpc);
    rpc.msg_id = 606u;
    expect_bad_rpc_unchanged(&inbox, &rpc);
    rpc.msg_id = 513u;
    rpc.uid = 0u;
    expect_bad_rpc_unchanged(&inbox, &rpc);

    rpc.msg_type = P4_HOSTED_RPC_TYPE_EVENT;
    rpc.msg_id = 769u;
    rpc.uid = 0u;
    rpc.payload_size = P4_HOSTED_RPC_MAX_SIZE + 1u;
    expect_bad_rpc_unchanged(&inbox, &rpc);
}

static void test_full_poison_and_invalidation(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox_item out, before_out;
    uint8_t payload[] = {0xa1, 0xb2};
    struct p4_hosted_rpc rpc;
    unsigned i;

    CHECK(p4_hosted_inbox_init(&inbox, 50u) == 1);
    for (i = 0; i < P4_HOSTED_INBOX_CAPACITY; ++i) {
        rpc = event_rpc(769u + i, i, payload, sizeof(payload));
        CHECK(p4_hosted_inbox_push(&inbox, 50u, &rpc) == 1);
    }
    CHECK(inbox.count == P4_HOSTED_INBOX_CAPACITY);
    rpc = event_rpc(776u, 100u, payload, sizeof(payload));
    CHECK(p4_hosted_inbox_push(&inbox, 50u, &rpc) == 0);
    CHECK(inbox.initialized == 1u && inbox.generation == 50u);
    CHECK(inbox.poisoned == 1u && inbox.count == 0u && inbox.head == 0u);
    check_items_zero(&inbox);

    memset(&out, 0x4c, sizeof(out));
    memcpy(&before_out, &out, sizeof(out));
    CHECK(p4_hosted_inbox_pop(&inbox, 50u, &out) == 0);
    CHECK(memcmp(&out, &before_out, sizeof(out)) == 0);
    CHECK(p4_hosted_inbox_push(&inbox, 50u, &rpc) == 0);

    CHECK(p4_hosted_inbox_init(&inbox, 50u) == 0);
    CHECK(p4_hosted_inbox_invalidate(&inbox, 50u) == 1);
    CHECK(inbox.poisoned == 1u && inbox.count == 0u && inbox.head == 0u);
    check_items_zero(&inbox);
    CHECK(p4_hosted_inbox_invalidate(&inbox, 50u) == 1);
    CHECK(p4_hosted_inbox_init(&inbox, 51u) == 1);
    CHECK(inbox.generation == 51u && inbox.poisoned == 0u);
}

static void test_stale_generation_preserves_state_and_output(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox before;
    struct p4_hosted_inbox_item out, before_out;
    uint8_t payload[] = {0x77};
    struct p4_hosted_rpc rpc = event_rpc(770u, 7u, payload, sizeof(payload));

    CHECK(p4_hosted_inbox_init(&inbox, 60u) == 1);
    CHECK(p4_hosted_inbox_push(&inbox, 60u, &rpc) == 1);
    memcpy(&before, &inbox, sizeof(before));
    memset(&out, 0x91, sizeof(out));
    memcpy(&before_out, &out, sizeof(out));

    CHECK(p4_hosted_inbox_push(&inbox, 59u, &rpc) == 0);
    check_queue_unchanged(&inbox, &before);
    CHECK(p4_hosted_inbox_pop(&inbox, 59u, &out) == 0);
    CHECK(memcmp(&out, &before_out, sizeof(out)) == 0);
    check_queue_unchanged(&inbox, &before);
    CHECK(p4_hosted_inbox_invalidate(&inbox, 59u) == 0);
    check_queue_unchanged(&inbox, &before);
    CHECK(p4_hosted_inbox_init(&inbox, 60u) == 0);
    check_queue_unchanged(&inbox, &before);
}

static void test_alias_and_range_overflow_refusals(void)
{
    struct p4_hosted_inbox inbox = {0};
    struct p4_hosted_inbox before;
    uint8_t payload[] = {0x42, 0x43};
    struct p4_hosted_rpc rpc = event_rpc(771u, 3u, payload, sizeof(payload));
    const struct p4_hosted_rpc *bad_rpc;
    uintptr_t near_end;

    CHECK(p4_hosted_inbox_init(&inbox, 70u) == 1);
    CHECK(p4_hosted_inbox_push(&inbox, 70u, &rpc) == 1);
    memcpy(&before, &inbox, sizeof(before));

    bad_rpc = (const struct p4_hosted_rpc *)(const void *)&inbox;
    CHECK(p4_hosted_inbox_push(&inbox, 70u, bad_rpc) == 0);
    check_queue_unchanged(&inbox, &before);

    rpc.payload = (const uint8_t *)(const void *)&inbox;
    rpc.payload_size = 1u;
    CHECK(p4_hosted_inbox_push(&inbox, 70u, &rpc) == 0);
    check_queue_unchanged(&inbox, &before);

    rpc.payload = (const uint8_t *)(const void *)&rpc;
    CHECK(p4_hosted_inbox_push(&inbox, 70u, &rpc) == 0);
    check_queue_unchanged(&inbox, &before);

    rpc.payload = payload;
    near_end = UINTPTR_MAX - (uintptr_t)sizeof(*bad_rpc) + 1u;
    bad_rpc = (const struct p4_hosted_rpc *)near_end;
    CHECK(p4_hosted_inbox_push(&inbox, 70u, bad_rpc) == 0);
    check_queue_unchanged(&inbox, &before);

    rpc.payload = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    rpc.payload_size = 4u;
    CHECK(p4_hosted_inbox_push(&inbox, 70u, &rpc) == 0);
    check_queue_unchanged(&inbox, &before);

    near_end = UINTPTR_MAX - (uintptr_t)sizeof(inbox) + 1u;
    CHECK(p4_hosted_inbox_init((struct p4_hosted_inbox *)near_end, 71u) == 0);
    CHECK(p4_hosted_inbox_invalidate((struct p4_hosted_inbox *)near_end,
                                     70u) == 0);
    check_queue_unchanged(&inbox, &before);

    near_end = UINTPTR_MAX - (uintptr_t)sizeof(struct p4_hosted_inbox_item) +
               1u;
    CHECK(p4_hosted_inbox_pop(&inbox, 70u,
                              (struct p4_hosted_inbox_item *)near_end) == 0);
    check_queue_unchanged(&inbox, &before);

    CHECK(p4_hosted_inbox_pop(
              &inbox, 70u,
              (struct p4_hosted_inbox_item *)(void *)&inbox) == 0);
    check_queue_unchanged(&inbox, &before);
}

int main(void)
{
    test_init_and_epoch_replacement();
    test_fifo_wrap_and_owned_copy();
    test_response_and_payload_boundaries();
    test_reject_invalid_envelopes();
    test_full_poison_and_invalidation();
    test_stale_generation_preserves_state_and_output();
    test_alias_and_range_overflow_refusals();
    printf("hosted inbox: %u checks passed\n", checks);
    return 0;
}
