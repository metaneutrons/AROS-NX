/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "hosted_frames.h"

struct frames_range {
    uintptr_t first;
    uintptr_t end;
};

static int make_range(const void *pointer, size_t size,
                      struct frames_range *range)
{
    uintptr_t first;

    if (!pointer || size == 0u || !range)
        return 0;
    first = (uintptr_t)pointer;
    if ((uintmax_t)size > (uintmax_t)(UINTPTR_MAX - first))
        return 0;
    range->first = first;
    range->end = first + (uintptr_t)size;
    return 1;
}

static int ranges_disjoint(const struct frames_range *left,
                           const struct frames_range *right)
{
    return left->end <= right->first || right->end <= left->first;
}

static void wipe_bytes(void *memory, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)memory;
    size_t index;

    for (index = 0u; index < size; ++index)
        bytes[index] = 0u;
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    volatile uint8_t *out = (volatile uint8_t *)destination;
    const volatile uint8_t *in = (const volatile uint8_t *)source;
    size_t index;

    for (index = 0u; index < size; ++index)
        out[index] = in[index];
}

static void wipe_items(struct p4_hosted_frames *frames)
{
    size_t index;

    for (index = 0u; index < P4_HOSTED_FRAME_QUEUE_CAPACITY; ++index)
        wipe_bytes(&frames->items[index], sizeof(frames->items[index]));
}

static int initialization_allowed(const struct p4_hosted_frames *frames,
                                 uint32_t generation)
{
    if (frames->initialized == 0u)
        return frames->generation == 0u && frames->head == 0u &&
               frames->count == 0u && frames->poisoned == 0u;
    if (frames->initialized != 1u || frames->generation == 0u ||
        frames->generation == generation ||
        frames->head >= P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        frames->count > P4_HOSTED_FRAME_QUEUE_CAPACITY ||
        frames->poisoned > 1u)
        return 0;
    if (frames->poisoned != 0u &&
        (frames->head != 0u || frames->count != 0u))
        return 0;
    return 1;
}

static int queue_matches(const struct p4_hosted_frames *frames,
                         uint32_t generation)
{
    return generation != 0u && frames->initialized == 1u &&
           frames->generation == generation && frames->poisoned == 0u &&
           frames->head < P4_HOSTED_FRAME_QUEUE_CAPACITY &&
           frames->count <= P4_HOSTED_FRAME_QUEUE_CAPACITY;
}

int p4_hosted_frames_init(struct p4_hosted_frames *frames,
                          uint32_t generation)
{
    if (!frames || generation == 0u ||
        !make_range(frames, sizeof(*frames), &(struct frames_range){0}) ||
        !initialization_allowed(frames, generation))
        return 0;

    wipe_items(frames);
    frames->generation = generation;
    frames->head = 0u;
    frames->count = 0u;
    frames->initialized = 1u;
    frames->poisoned = 0u;
    return 1;
}

int p4_hosted_frames_push(struct p4_hosted_frames *frames,
                          uint32_t generation,
                          const uint8_t *bytes, size_t size)
{
    struct frames_range queue_range, input_range;
    struct p4_hosted_owned_frame *item;
    size_t tail;

    if (!frames || !bytes ||
        size < P4_HOSTED_ETHERNET_HEADER_SIZE ||
        size > P4_HOSTED_ETHERNET_FRAME_MAX ||
        !make_range(frames, sizeof(*frames), &queue_range) ||
        !make_range(bytes, size, &input_range) ||
        !ranges_disjoint(&queue_range, &input_range) ||
        !queue_matches(frames, generation))
        return 0;

    if (frames->count == P4_HOSTED_FRAME_QUEUE_CAPACITY) {
        wipe_items(frames);
        frames->head = 0u;
        frames->count = 0u;
        frames->poisoned = 1u;
        return 0;
    }

    tail = frames->head + frames->count;
    if (tail >= P4_HOSTED_FRAME_QUEUE_CAPACITY)
        tail -= P4_HOSTED_FRAME_QUEUE_CAPACITY;
    item = &frames->items[tail];
    wipe_bytes(item, sizeof(*item));
    item->size = size;
    copy_bytes(item->bytes, bytes, size);
    ++frames->count;
    return 1;
}

int p4_hosted_frames_pop(struct p4_hosted_frames *frames,
                         uint32_t generation,
                         uint8_t *out, size_t capacity, size_t *out_size)
{
    struct frames_range queue_range, output_range, size_range;
    struct p4_hosted_owned_frame *item;
    size_t frame_size;

    if (!frames || !out || !out_size ||
        !make_range(frames, sizeof(*frames), &queue_range) ||
        !make_range(out, capacity, &output_range) ||
        !make_range(out_size, sizeof(*out_size), &size_range) ||
        !ranges_disjoint(&queue_range, &output_range) ||
        !ranges_disjoint(&queue_range, &size_range) ||
        !ranges_disjoint(&output_range, &size_range) ||
        !queue_matches(frames, generation) || frames->count == 0u)
        return 0;

    item = &frames->items[frames->head];
    frame_size = item->size;
    if (frame_size < P4_HOSTED_ETHERNET_HEADER_SIZE ||
        frame_size > P4_HOSTED_ETHERNET_FRAME_MAX || capacity < frame_size)
        return 0;

    copy_bytes(out, item->bytes, frame_size);
    wipe_bytes(item, sizeof(*item));
    frames->head++;
    if (frames->head == P4_HOSTED_FRAME_QUEUE_CAPACITY)
        frames->head = 0u;
    --frames->count;
    *out_size = frame_size;
    return 1;
}

int p4_hosted_frames_invalidate(struct p4_hosted_frames *frames,
                                uint32_t generation)
{
    if (!frames || generation == 0u ||
        !make_range(frames, sizeof(*frames), &(struct frames_range){0}) ||
        frames->initialized != 1u || frames->generation != generation)
        return 0;

    wipe_items(frames);
    frames->head = 0u;
    frames->count = 0u;
    frames->poisoned = 1u;
    return 1;
}
