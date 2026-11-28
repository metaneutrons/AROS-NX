#!/usr/bin/env python3
"""Compile and stress the actual P4 console ring header on the host."""

import os
import pathlib
import shlex
import subprocess
import tempfile


KERNEL = pathlib.Path(__file__).resolve().parents[1]
CC = shlex.split(os.environ.get("CC", "cc"))

FIXTURE = r'''
#include "console_ring.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRODUCERS 4u
#define ATTEMPTS_PER_PRODUCER 4000u
#define TOTAL_ATTEMPTS (PRODUCERS * ATTEMPTS_PER_PRODUCER)

_Static_assert(sizeof(((struct p4_console_record *)0)->sequence) == 4,
               "sequence must remain 32-bit");
_Static_assert(sizeof(((struct p4_console_ring *)0)->next_sequence) == 4,
               "atomic sequence must remain 32-bit");
_Static_assert(sizeof(((struct p4_console_ring *)0)->dropped) == 4,
               "atomic drop counter must remain 32-bit");
_Static_assert(sizeof(struct p4_console_ring) <= 10u * 1024u,
               "console ring exceeds its bounded memory budget");

static void check_zero_tail(const struct p4_console_record *record)
{
    unsigned int i;

    for (i = record->length; i < P4_CONSOLE_RECORD_BYTES; ++i)
        assert(record->data[i] == '\0');
}

static void test_empty_and_boundaries(void)
{
    struct p4_console_ring ring = { 0 };
    struct p4_console_record out, before;
    char full[P4_CONSOLE_RECORD_BYTES];
    char too_long[P4_CONSOLE_RECORD_BYTES + 1u];
    unsigned int i;

    memset(&out, 0xa5, sizeof(out));
    before = out;
    assert(!p4_console_ring_pop(&ring, &out));
    assert(memcmp(&out, &before, sizeof(out)) == 0);
    assert(p4_console_ring_next_sequence(&ring) == 0u);
    assert(p4_console_ring_dropped(&ring) == 0u);

    assert(!p4_console_ring_push(&ring, "", 0u, 0u));
    assert(p4_console_ring_next_sequence(&ring) == 1u);
    assert(p4_console_ring_dropped(&ring) == 1u);

    for (i = 0; i < sizeof(full); ++i)
        full[i] = (char)(i ^ 0x5a);
    assert(p4_console_ring_push(&ring, full, sizeof(full), 1u));
    assert(p4_console_ring_pop(&ring, &out));
    assert(out.sequence == 1u && out.length == sizeof(full) && out.hart == 1u);
    assert(memcmp(out.data, full, sizeof(full)) == 0);
    assert(p4_console_ring_pop(&ring, &out) == 0);

    memset(too_long, 'x', sizeof(too_long));
    assert(!p4_console_ring_push(&ring, too_long, sizeof(too_long), 2u));
    assert(!p4_console_ring_push(&ring, NULL, 1u, 2u));
    assert(p4_console_ring_next_sequence(&ring) == 4u);
    assert(p4_console_ring_dropped(&ring) == 3u);
    assert(!p4_console_ring_pop(&ring, &out));
}

static void test_full_and_late_drain(void)
{
    struct p4_console_ring ring = { 0 };
    struct p4_console_record out, before;
    char payload[P4_CONSOLE_RECORD_BYTES];
    unsigned int i, j;

    for (i = 0; i < P4_CONSOLE_RING_SLOTS; ++i)
    {
        memset(payload, (int)i, sizeof(payload));
        assert(p4_console_ring_push(&ring, payload, sizeof(payload),
                                    (uint8_t)(i & 1u)));
    }
    memset(payload, 0xee, sizeof(payload));
    assert(!p4_console_ring_push(&ring, payload, sizeof(payload), 3u));
    assert(p4_console_ring_next_sequence(&ring) == 65u);
    assert(p4_console_ring_dropped(&ring) == 1u);

    /* The queue remains intact after a full-queue rejection and can be
       drained once a reader becomes available. */
    for (i = 0; i < P4_CONSOLE_RING_SLOTS; ++i)
    {
        assert(p4_console_ring_pop(&ring, &out));
        assert(out.sequence == i && out.length == P4_CONSOLE_RECORD_BYTES);
        assert(out.hart == (uint8_t)(i & 1u));
        for (j = 0; j < P4_CONSOLE_RECORD_BYTES; ++j)
            assert((unsigned char)out.data[j] == (unsigned char)i);
    }

    before = out;
    assert(!p4_console_ring_pop(&ring, &out));
    assert(memcmp(&out, &before, sizeof(out)) == 0);
}

static void test_lock_contention_and_wrap(void)
{
    struct p4_console_ring ring = { 0 };
    struct p4_console_ring counter_ring = { 0 };
    struct p4_console_record out;

    assert(p4_console_ring_push(&ring, "first", 5u, 0u));
    __atomic_store_n(&ring.lock, 1u, __ATOMIC_RELAXED);
    assert(!p4_console_ring_push(&ring, "lost", 4u, 1u));
    memset(&out, 0xa5, sizeof(out));
    assert(!p4_console_ring_pop(&ring, &out));
    __atomic_store_n(&ring.lock, 0u, __ATOMIC_RELEASE);
    assert(p4_console_ring_dropped(&ring) == 1u);
    assert(p4_console_ring_next_sequence(&ring) == 2u);
    assert(p4_console_ring_pop(&ring, &out));
    assert(out.sequence == 0u && out.length == 5u);
    assert(memcmp(out.data, "first", 5u) == 0);
    check_zero_tail(&out);

    __atomic_store_n(&ring.next_sequence, UINT32_MAX - 1u, __ATOMIC_RELAXED);
    assert(p4_console_ring_push(&ring, "a", 1u, 2u));
    assert(p4_console_ring_push(&ring, "b", 1u, 2u));
    assert(p4_console_ring_push(&ring, "c", 1u, 2u));
    assert(p4_console_ring_next_sequence(&ring) == 1u);
    assert(p4_console_ring_pop(&ring, &out) && out.sequence == UINT32_MAX - 1u);
    assert(p4_console_ring_pop(&ring, &out) && out.sequence == UINT32_MAX);
    assert(p4_console_ring_pop(&ring, &out) && out.sequence == 0u);
    assert(!p4_console_ring_pop(&ring, &out));

    __atomic_store_n(&counter_ring.dropped, UINT32_MAX, __ATOMIC_RELAXED);
    assert(!p4_console_ring_push(&counter_ring, "", 0u, 0u));
    assert(p4_console_ring_dropped(&counter_ring) == 0u);
    assert(p4_console_ring_next_sequence(&counter_ring) == 1u);
}

static void test_corrupt_counters_fail_closed(void)
{
    struct p4_console_ring bad_head = { 0 };
    struct p4_console_ring bad_count = { 0 };
    struct p4_console_ring inconsistent = { 0 };
    struct p4_console_record out, before;

    bad_head.head = P4_CONSOLE_RING_SLOTS;
    assert(!p4_console_ring_push(&bad_head, "x", 1u, 0u));
    assert(p4_console_ring_next_sequence(&bad_head) == 1u);
    assert(p4_console_ring_dropped(&bad_head) == 1u);
    memset(&out, 0xa5, sizeof(out));
    before = out;
    assert(!p4_console_ring_pop(&bad_head, &out));
    assert(memcmp(&out, &before, sizeof(out)) == 0);

    bad_count.count = P4_CONSOLE_RING_SLOTS + 1u;
    assert(!p4_console_ring_push(&bad_count, "x", 1u, 0u));
    assert(p4_console_ring_dropped(&bad_count) == 1u);
    assert(!p4_console_ring_pop(&bad_count, &out));

    inconsistent.count = 1u;
    assert(!p4_console_ring_pop(&inconsistent, &out));
    assert(memcmp(&out, &before, sizeof(out)) == 0);
}

struct producer_args
{
    struct p4_console_ring *ring;
    unsigned int producer;
    uint32_t *done;
};

struct consumer_args
{
    struct p4_console_ring *ring;
    uint32_t *done;
    uint32_t records;
    uint64_t gaps;
};

static unsigned char payload_byte(unsigned int producer, uint32_t attempt,
                                  unsigned int index)
{
    return (unsigned char)((producer * 17u + attempt * 31u + index * 13u)
                           & 0xffu);
}

static void *producer_main(void *opaque)
{
    struct producer_args *args = (struct producer_args *)opaque;
    char payload[P4_CONSOLE_RECORD_BYTES];
    uint32_t attempt;
    unsigned int i;

    for (attempt = 0; attempt < ATTEMPTS_PER_PRODUCER; ++attempt)
    {
        payload[0] = (char)args->producer;
        payload[1] = (char)(attempt & 0xffu);
        payload[2] = (char)((attempt >> 8) & 0xffu);
        payload[3] = (char)((attempt >> 16) & 0xffu);
        payload[4] = (char)((attempt >> 24) & 0xffu);
        for (i = 5; i < sizeof(payload); ++i)
            payload[i] = (char)payload_byte(args->producer, attempt, i);
        (void)p4_console_ring_push(args->ring, payload, sizeof(payload),
                                   (uint8_t)args->producer);
    }
    (void)__atomic_fetch_add(args->done, 1u, __ATOMIC_RELEASE);
    return NULL;
}

static void check_concurrent_record(const struct p4_console_record *record)
{
    unsigned int i, producer;
    uint32_t attempt;

    assert(record->length == P4_CONSOLE_RECORD_BYTES);
    producer = record->hart;
    assert(producer < PRODUCERS);
    assert((unsigned char)record->data[0] == producer);
    attempt = (uint32_t)(unsigned char)record->data[1]
            | ((uint32_t)(unsigned char)record->data[2] << 8)
            | ((uint32_t)(unsigned char)record->data[3] << 16)
            | ((uint32_t)(unsigned char)record->data[4] << 24);
    assert(attempt < ATTEMPTS_PER_PRODUCER);
    for (i = 5; i < P4_CONSOLE_RECORD_BYTES; ++i)
        assert((unsigned char)record->data[i]
               == payload_byte(producer, attempt, i));
}

static void *consumer_main(void *opaque)
{
    struct consumer_args *args = (struct consumer_args *)opaque;
    uint32_t expected_sequence = 0;
    struct p4_console_record out;

    for (;;)
    {
        if (p4_console_ring_pop(args->ring, &out))
        {
            uint32_t missing = out.sequence - expected_sequence;
            assert(missing < TOTAL_ATTEMPTS);
            args->gaps += missing;
            expected_sequence = out.sequence + 1u;
            check_concurrent_record(&out);
            ++args->records;
            continue;
        }

        if (__atomic_load_n(args->done, __ATOMIC_ACQUIRE) == PRODUCERS)
            break;
        sched_yield();
    }

    args->gaps += TOTAL_ATTEMPTS - expected_sequence;
    return NULL;
}

static void test_concurrent_producers_and_consumer(void)
{
    struct p4_console_ring ring = { 0 };
    pthread_t producers[PRODUCERS], consumer;
    struct producer_args producer_args[PRODUCERS];
    uint32_t done = 0;
    struct consumer_args consumer_args = { &ring, &done, 0u, 0u };
    unsigned int i;

    assert(pthread_create(&consumer, NULL, consumer_main, &consumer_args) == 0);
    for (i = 0; i < PRODUCERS; ++i)
    {
        producer_args[i].ring = &ring;
        producer_args[i].producer = i;
        producer_args[i].done = &done;
        assert(pthread_create(&producers[i], NULL, producer_main,
                              &producer_args[i]) == 0);
    }
    for (i = 0; i < PRODUCERS; ++i)
        assert(pthread_join(producers[i], NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);

    assert(p4_console_ring_next_sequence(&ring) == TOTAL_ATTEMPTS);
    assert(consumer_args.records + p4_console_ring_dropped(&ring)
           == TOTAL_ATTEMPTS);
    assert(consumer_args.gaps == p4_console_ring_dropped(&ring));
    assert(ring.count == 0u);
}

int main(void)
{
    test_empty_and_boundaries();
    test_full_and_late_drain();
    test_lock_contention_and_wrap();
    test_corrupt_counters_fail_closed();
    test_concurrent_producers_and_consumer();
    puts("console ring passed: boundaries, full/drain, contention, wrap and SMP stress");
    return 0;
}
'''


def compile_and_run(tmpdir, optimization, sanitizer):
    source = pathlib.Path(tmpdir) / f"console_ring_{optimization}_{sanitizer}.c"
    executable = pathlib.Path(tmpdir) / f"console_ring_{optimization}_{sanitizer}"
    source.write_text(FIXTURE)
    command = [
        *CC,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-pthread",
        f"-O{optimization}",
        "-fno-omit-frame-pointer",
        f"-fsanitize={sanitizer}",
        "-I",
        str(KERNEL),
        str(source),
        "-o",
        str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
    return executable


def try_tsan(tmpdir):
    source = pathlib.Path(tmpdir) / "console_ring_tsan.c"
    executable = pathlib.Path(tmpdir) / "console_ring_tsan"
    source.write_text(FIXTURE)
    command = [
        *CC,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
        "-pthread",
        "-O1",
        "-fno-omit-frame-pointer",
        "-fsanitize=thread",
        "-I",
        str(KERNEL),
        str(source),
        "-o",
        str(executable),
    ]
    build = subprocess.run(command, text=True, capture_output=True)
    if build.returncode:
        detail = (build.stderr or build.stdout).strip().splitlines()
        print("TSAN unavailable (compiler/runtime build failed): "
              + (detail[-1] if detail else "unknown compiler error"))
        return

    run = subprocess.run([str(executable)], text=True, capture_output=True)
    output = (run.stdout + run.stderr).strip()
    unsupported = (
        "unexpected memory mapping",
        "ThreadSanitizer is not supported",
        "failed to allocate",
        "unsupported platform",
    )
    if run.returncode and any(message in output for message in unsupported):
        print("TSAN unavailable (runtime unsupported): "
              + output.splitlines()[-1])
        return
    if run.returncode:
        raise subprocess.CalledProcessError(run.returncode, [str(executable)],
                                            output=run.stdout,
                                            stderr=run.stderr)
    print(output)


def main():
    with tempfile.TemporaryDirectory(prefix="p4-console-ring-") as tmpdir:
        compile_and_run(tmpdir, 0, "address,undefined")
        compile_and_run(tmpdir, 2, "address,undefined")
        try_tsan(tmpdir)


if __name__ == "__main__":
    main()
