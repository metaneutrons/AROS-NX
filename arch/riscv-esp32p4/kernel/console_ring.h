/* Copyright (c) 2026, The AROS Development Team. All rights reserved.
 * Bounded, nonblocking records for the ESP32-P4 kernel console.
 */
#ifndef P4_CONSOLE_RING_H
#define P4_CONSOLE_RING_H

#include <stddef.h>
#include <stdint.h>

#define P4_CONSOLE_RING_SLOTS 64u
#define P4_CONSOLE_RECORD_BYTES 128u

struct p4_console_record
{
    uint32_t sequence;
    uint16_t length;
    uint8_t hart;
    char data[P4_CONSOLE_RECORD_BYTES];
};

/* Define an instance with static storage duration or initialize it with
 * { 0 }. All atomic counters are 32-bit so this remains native on RV32. */
struct p4_console_ring
{
    uint32_t next_sequence;
    uint32_t dropped;
    uint32_t lock;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    struct p4_console_record slots[P4_CONSOLE_RING_SLOTS];
};

/* Indices and occupancy are private ring state protected by lock. Check them
 * before indexing so a damaged counter fails closed instead of overrunning
 * the fixed slot array. */
static inline int
p4_console_ring_state_valid(const struct p4_console_ring *ring)
{
    if (ring->head >= P4_CONSOLE_RING_SLOTS ||
        ring->tail >= P4_CONSOLE_RING_SLOTS ||
        ring->count > P4_CONSOLE_RING_SLOTS)
        return 0;

    return ring->head == (ring->tail + ring->count) % P4_CONSOLE_RING_SLOTS;
}

/* Sequence assignment for successful pushes happens while holding the ring
 * lock, so FIFO order and successful sequence order agree. A rejected
 * attempt that cannot acquire the lock reserves its sequence atomically;
 * such attempts (and bad lengths) appear as gaps between delivered records.
 */
static inline int
p4_console_ring_push(struct p4_console_ring *ring, const char *data,
                     size_t length, uint8_t hart)
{
    uint32_t expected = 0;
    uint32_t sequence;
    uint32_t i;
    struct p4_console_record *record;

    if (!ring)
        return 0;

    if (!data || length == 0 || length > P4_CONSOLE_RECORD_BYTES)
    {
        (void)__atomic_fetch_add(&ring->next_sequence, 1u, __ATOMIC_RELAXED);
        (void)__atomic_fetch_add(&ring->dropped, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    /* One CAS only: a contended producer never spins or waits for a reader. */
    if (!__atomic_compare_exchange_n(&ring->lock, &expected, 1u, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        (void)__atomic_fetch_add(&ring->next_sequence, 1u, __ATOMIC_RELAXED);
        (void)__atomic_fetch_add(&ring->dropped, 1u, __ATOMIC_RELAXED);
        return 0;
    }

    sequence = __atomic_fetch_add(&ring->next_sequence, 1u, __ATOMIC_RELAXED);
    if (!p4_console_ring_state_valid(ring))
    {
        (void)__atomic_fetch_add(&ring->dropped, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&ring->lock, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (ring->count == P4_CONSOLE_RING_SLOTS)
    {
        (void)__atomic_fetch_add(&ring->dropped, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&ring->lock, 0u, __ATOMIC_RELEASE);
        return 0;
    }

    record = &ring->slots[ring->head];
    record->sequence = sequence;
    record->length = (uint16_t)length;
    record->hart = hart;
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        record->data[i] = i < length ? data[i] : '\0';

    ring->head = (ring->head + 1u) % P4_CONSOLE_RING_SLOTS;
    ++ring->count;
    __atomic_store_n(&ring->lock, 0u, __ATOMIC_RELEASE);
    return 1;
}

/* The single consumer also makes exactly one lock attempt. If a producer
 * owns the lock, no ring state or output bytes are changed. */
static inline int
p4_console_ring_pop(struct p4_console_ring *ring, struct p4_console_record *out)
{
    uint32_t expected = 0;
    uint32_t i;
    const struct p4_console_record *record;

    if (!ring || !out)
        return 0;

    if (!__atomic_compare_exchange_n(&ring->lock, &expected, 1u, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;

    if (!p4_console_ring_state_valid(ring) || ring->count == 0)
    {
        __atomic_store_n(&ring->lock, 0u, __ATOMIC_RELEASE);
        return 0;
    }

    record = &ring->slots[ring->tail];
    out->sequence = record->sequence;
    out->length = record->length;
    out->hart = record->hart;
    for (i = 0; i < P4_CONSOLE_RECORD_BYTES; ++i)
        out->data[i] = record->data[i];

    ring->tail = (ring->tail + 1u) % P4_CONSOLE_RING_SLOTS;
    --ring->count;
    __atomic_store_n(&ring->lock, 0u, __ATOMIC_RELEASE);
    return 1;
}

static inline uint32_t
p4_console_ring_dropped(const struct p4_console_ring *ring)
{
    return ring ? __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED) : 0u;
}

static inline uint32_t
p4_console_ring_next_sequence(const struct p4_console_ring *ring)
{
    return ring ? __atomic_load_n(&ring->next_sequence, __ATOMIC_RELAXED) : 0u;
}

#endif /* P4_CONSOLE_RING_H */
