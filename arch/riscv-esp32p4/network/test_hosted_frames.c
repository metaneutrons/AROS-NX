/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only tests for the owned Ethernet-frame queue; no hardware claims. */
#include "hosted_frames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GENERATION UINT32_C(0x7211)
#define REPLACEMENT_GENERATION UINT32_C(0x7212)

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static void fill_frame(uint8_t *frame, size_t size, uint8_t marker)
{
    size_t index;

    for (index = 0u; index < size; ++index)
        frame[index] = (uint8_t)(marker + (uint8_t)(index * 17u));
    if (size >= P4_HOSTED_ETHERNET_HEADER_SIZE) {
        frame[12] = 0x08u;
        frame[13] = 0x00u;
    }
}

static void check_item_zero(const struct p4_hosted_owned_frame *item)
{
    const uint8_t *bytes = (const uint8_t *)item;
    size_t index;

    for (index = 0u; index < sizeof(*item); ++index) {
        if (bytes[index] != 0u) {
            CHECK(0);
            return;
        }
    }
    CHECK(1);
}

static void check_items_zero(const struct p4_hosted_frames *frames)
{
    size_t index;

    for (index = 0u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index)
        check_item_zero(&frames->items[index]);
}

static void check_unchanged(const struct p4_hosted_frames *frames,
                            const struct p4_hosted_frames *before)
{
    CHECK(memcmp(frames, before, sizeof(*frames)) == 0);
}

static void test_init_epoch_rebind_and_corrupt_refusal(void)
{
    struct p4_hosted_frames frames = {0};
    struct p4_hosted_frames before;
    uint8_t frame[P4_HOSTED_ETHERNET_HEADER_SIZE];

    CHECK(p4_hosted_frames_init(&frames, TEST_GENERATION));
    CHECK(frames.initialized == 1u &&
          frames.generation == TEST_GENERATION && frames.head == 0u &&
          frames.count == 0u && frames.poisoned == 0u);
    check_items_zero(&frames);
    fill_frame(frame, sizeof frame, 0x31u);
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, frame,
                                sizeof frame));
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, TEST_GENERATION));
    check_unchanged(&frames, &before);
    CHECK(!p4_hosted_frames_init(&frames, 0u));
    check_unchanged(&frames, &before);

    CHECK(p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    CHECK(frames.generation == REPLACEMENT_GENERATION && frames.head == 0u &&
          frames.count == 0u && frames.poisoned == 0u);
    check_items_zero(&frames);

    memset(&frames, 0, sizeof frames);
    frames.generation = TEST_GENERATION;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    check_unchanged(&frames, &before);

    memset(&frames, 0, sizeof frames);
    frames.initialized = 1u;
    frames.generation = TEST_GENERATION;
    frames.head = P4_HOSTED_FRAME_QUEUE_CAPACITY;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    check_unchanged(&frames, &before);

    frames.head = 0u;
    frames.count = P4_HOSTED_FRAME_QUEUE_CAPACITY + 1u;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    check_unchanged(&frames, &before);

    frames.count = 0u;
    frames.poisoned = 2u;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    check_unchanged(&frames, &before);

    frames.poisoned = 1u;
    frames.head = 1u;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    check_unchanged(&frames, &before);
}

static void test_min_max_owned_copy_and_ring_wrap(void)
{
    struct p4_hosted_frames frames = {0};
    uint8_t minimum[P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t maximum[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t source[P4_HOSTED_FRAME_QUEUE_CAPACITY][P4_HOSTED_ETHERNET_HEADER_SIZE];
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];
    size_t output_size = 0u;
    unsigned index;

    CHECK(p4_hosted_frames_init(&frames, TEST_GENERATION));
    fill_frame(minimum, sizeof minimum, 0x11u);
    fill_frame(maximum, sizeof maximum, 0x42u);
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, minimum,
                                sizeof minimum));
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, maximum,
                                sizeof maximum));
    memset(minimum, 0xe1, sizeof minimum);
    memset(maximum, 0xe2, sizeof maximum);
    memset(output, 0xa5, sizeof output);
    CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                               sizeof output, &output_size));
    CHECK(output_size == P4_HOSTED_ETHERNET_HEADER_SIZE);
    fill_frame(minimum, sizeof minimum, 0x11u);
    CHECK(memcmp(output, minimum, sizeof minimum) == 0);
    check_item_zero(&frames.items[0]);

    memset(output, 0xa6, sizeof output);
    CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                               sizeof output, &output_size));
    CHECK(output_size == P4_HOSTED_ETHERNET_FRAME_MAX);
    fill_frame(maximum, sizeof maximum, 0x42u);
    CHECK(memcmp(output, maximum, sizeof maximum) == 0);
    check_item_zero(&frames.items[1]);
    CHECK(frames.count == 0u && frames.head == 2u);

    for (index = 0u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index) {
        fill_frame(source[index], sizeof source[index], (uint8_t)(0x30u + index));
        CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, source[index],
                                    sizeof source[index]));
    }
    CHECK(frames.count == P4_HOSTED_FRAME_QUEUE_CAPACITY && frames.head == 2u);
    for (index = 0u; index < 2u; ++index) {
        CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                                   sizeof output, &output_size));
        CHECK(output_size == sizeof source[index]);
        CHECK(memcmp(output, source[index], sizeof source[index]) == 0);
    }
    fill_frame(source[0], sizeof source[0], 0x71u);
    fill_frame(source[1], sizeof source[1], 0x82u);
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, source[0],
                                sizeof source[0]));
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, source[1],
                                sizeof source[1]));
    CHECK(frames.count == P4_HOSTED_FRAME_QUEUE_CAPACITY && frames.head == 0u);

    for (index = 2u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index) {
        CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                                   sizeof output, &output_size));
        CHECK(memcmp(output, source[index], sizeof source[index]) == 0);
    }
    CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                               sizeof output, &output_size));
    CHECK(memcmp(output, source[0], sizeof source[0]) == 0);
    CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                               sizeof output, &output_size));
    CHECK(memcmp(output, source[1], sizeof source[1]) == 0);
    CHECK(frames.count == 0u && frames.head == 0u && !frames.poisoned);
}

static void expect_push_refusal_unchanged(struct p4_hosted_frames *frames,
                                          const uint8_t *bytes, size_t size)
{
    struct p4_hosted_frames before;

    memcpy(&before, frames, sizeof before);
    CHECK(!p4_hosted_frames_push(frames, TEST_GENERATION, bytes, size));
    check_unchanged(frames, &before);
}

static void test_push_invalid_lengths_alias_ranges_and_generation(void)
{
    struct p4_hosted_frames frames = {0};
    uint8_t valid[P4_HOSTED_ETHERNET_HEADER_SIZE];

    CHECK(p4_hosted_frames_init(&frames, TEST_GENERATION));
    fill_frame(valid, sizeof valid, 0x21u);
    expect_push_refusal_unchanged(&frames, valid, sizeof valid - 1u);
    expect_push_refusal_unchanged(&frames, valid,
        P4_HOSTED_ETHERNET_FRAME_MAX + 1u);
    expect_push_refusal_unchanged(&frames, NULL, sizeof valid);
    expect_push_refusal_unchanged(&frames,
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u), sizeof valid);
    expect_push_refusal_unchanged(&frames,
        (const uint8_t *)(const void *)&frames, sizeof valid);

    {
        struct p4_hosted_frames before;
        memcpy(&before, &frames, sizeof before);
        CHECK(!p4_hosted_frames_push(&frames, REPLACEMENT_GENERATION,
                                     valid, sizeof valid));
        check_unchanged(&frames, &before);
    }
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, valid, sizeof valid));
    CHECK(frames.count == 1u && !frames.poisoned);
}

static void test_pop_transactional_alias_capacity_and_wrap_ranges(void)
{
    struct p4_hosted_frames frames = {0};
    struct p4_hosted_frames before;
    uint8_t input[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];
    uint8_t output_before[P4_HOSTED_ETHERNET_FRAME_MAX];
    size_t output_size = 0x1234u, size_before;
    union output_alias {
        size_t aligned_size;
        uint8_t bytes[P4_HOSTED_ETHERNET_FRAME_MAX];
    } alias;

    CHECK(p4_hosted_frames_init(&frames, TEST_GENERATION));
    fill_frame(input, sizeof input, 0x53u);
    CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, input,
                                P4_HOSTED_ETHERNET_HEADER_SIZE));
    memset(output, 0xb4, sizeof output);
    memcpy(output_before, output, sizeof output);
    size_before = output_size;
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
        P4_HOSTED_ETHERNET_HEADER_SIZE - 1u, &output_size));
    check_unchanged(&frames, &before);
    CHECK(memcmp(output, output_before, sizeof output) == 0 &&
          output_size == size_before);

    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
        sizeof output, NULL));
    check_unchanged(&frames, &before);
    CHECK(memcmp(output, output_before, sizeof output) == 0 &&
          output_size == size_before);

    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION,
        (uint8_t *)(void *)&frames, P4_HOSTED_ETHERNET_HEADER_SIZE,
        &output_size));
    check_unchanged(&frames, &before);
    CHECK(output_size == size_before);

    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
        sizeof output, (size_t *)(void *)&frames));
    check_unchanged(&frames, &before);
    CHECK(memcmp(output, output_before, sizeof output) == 0 &&
          output_size == size_before);

    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION,
        (uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u),
        P4_HOSTED_ETHERNET_HEADER_SIZE, &output_size));
    check_unchanged(&frames, &before);
    CHECK(output_size == size_before);

    memset(&alias, 0xa7, sizeof alias);
    memcpy(output_before, alias.bytes, sizeof alias.bytes);
    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, alias.bytes,
        sizeof alias.bytes, &alias.aligned_size));
    check_unchanged(&frames, &before);
    CHECK(memcmp(output_before, alias.bytes, sizeof alias.bytes) == 0);

    memcpy(output_before, output, sizeof output);
    CHECK(!p4_hosted_frames_pop(&frames, REPLACEMENT_GENERATION, output,
        sizeof output, &output_size));
    check_unchanged(&frames, &before);
    CHECK(memcmp(output, output_before, sizeof output) == 0 &&
          output_size == size_before);

    CHECK(p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                               sizeof output, &output_size));
    CHECK(output_size == P4_HOSTED_ETHERNET_HEADER_SIZE);
    CHECK(memcmp(output, input, output_size) == 0);
    check_item_zero(&frames.items[0]);
    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
                                sizeof output, &output_size));
}

static void test_full_queue_is_wiped_poisoned_and_epoch_scoped(void)
{
    struct p4_hosted_frames frames = {0};
    struct p4_hosted_frames before;
    uint8_t frame[P4_HOSTED_ETHERNET_HEADER_SIZE];
    size_t output_size = 0x9876u;
    size_t output_before;
    uint8_t output[P4_HOSTED_ETHERNET_FRAME_MAX];
    unsigned index;

    CHECK(p4_hosted_frames_init(&frames, TEST_GENERATION));
    for (index = 0u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index) {
        fill_frame(frame, sizeof frame, (uint8_t)(0x20u + index));
        CHECK(p4_hosted_frames_push(&frames, TEST_GENERATION, frame,
                                    sizeof frame));
    }
    memcpy(&before, &frames, sizeof before);
    fill_frame(frame, sizeof frame, 0x99u);
    CHECK(!p4_hosted_frames_push(&frames, TEST_GENERATION, frame,
                                 sizeof frame));
    CHECK(frames.initialized == 1u && frames.generation == TEST_GENERATION &&
          frames.poisoned == 1u && frames.head == 0u && frames.count == 0u);
    check_items_zero(&frames);
    output_before = output_size;
    memset(output, 0xacu, sizeof output);
    CHECK(!p4_hosted_frames_pop(&frames, TEST_GENERATION, output,
        sizeof output, &output_size));
    CHECK(frames.poisoned == 1u && frames.count == 0u);
    CHECK(output_size == output_before && output[0] == 0xacu);
    CHECK(!p4_hosted_frames_invalidate(&frames, REPLACEMENT_GENERATION));
    CHECK(!p4_hosted_frames_init(&frames, TEST_GENERATION));
    CHECK(p4_hosted_frames_init(&frames, REPLACEMENT_GENERATION));
    CHECK(frames.generation == REPLACEMENT_GENERATION && !frames.poisoned &&
          frames.count == 0u && frames.head == 0u);
    check_items_zero(&frames);

    fill_frame(frame, sizeof frame, 0x75u);
    CHECK(p4_hosted_frames_push(&frames, REPLACEMENT_GENERATION, frame,
                                sizeof frame));
    memcpy(&before, &frames, sizeof before);
    CHECK(!p4_hosted_frames_invalidate(&frames, TEST_GENERATION));
    check_unchanged(&frames, &before);
    CHECK(p4_hosted_frames_invalidate(&frames, REPLACEMENT_GENERATION));
    CHECK(frames.poisoned == 1u && frames.head == 0u && frames.count == 0u);
    check_items_zero(&frames);
}

int main(void)
{
    test_init_epoch_rebind_and_corrupt_refusal();
    test_min_max_owned_copy_and_ring_wrap();
    test_push_invalid_lengths_alias_ranges_and_generation();
    test_pop_transactional_alias_capacity_and_wrap_ranges();
    test_full_queue_is_wiped_poisoned_and_epoch_scoped();
    printf("hosted owned Ethernet frames: %lu checks passed\n", checks);
    return 0;
}
