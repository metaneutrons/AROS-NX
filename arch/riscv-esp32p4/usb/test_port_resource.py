#!/usr/bin/env python3
"""Compile the production port-resource wrappers against fake Exec services."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


USB = Path(__file__).resolve().parent
RESOURCE = USB / "port_resource.c"
BROKER = USB / "port_broker.c"
START_MARKER = "static bool try_lock(void)"
END_MARKER = "static bool rcc_lock(void *ctx)"
BULK_START_MARKER = "static void bulk_step(void)"
BULK_END_MARKER = "static void port_task(void)"


PRELUDE = r'''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_broker.h"

#define P4_USB_PORT_CHUNK 512u
enum {
    P4_USB_PORT_STARTING = 0,
    P4_USB_PORT_READY = 1,
    P4_USB_PORT_FAULTED = 2
};

struct SignalSemaphore { bool held; };
struct Task { unsigned id; };

static struct SignalSemaphore lock;
static struct p4_port_broker broker;
static struct Task task_a = { 1u };
static struct Task task_b = { 2u };
static struct Task *current_task;
static uint32_t state = P4_USB_PORT_STARTING;
static bool invalidate_pending;
static bool lock_held;
static uint32_t last_client_ms;
static uint32_t fake_ms;
static unsigned checks;

#define FAKE_ENDPOINT_CAPACITY 4096u
static uint8_t vendor_rx[FAKE_ENDPOINT_CAPACITY];
static size_t vendor_rx_length;
static uint8_t vendor_tx[FAKE_ENDPOINT_CAPACITY];
static size_t vendor_tx_length;
static size_t vendor_write_room;
static size_t vendor_write_accept;
static bool vendor_mounted;
static unsigned vendor_read_calls;
static unsigned vendor_write_calls;
static unsigned vendor_read_flushes;
static unsigned vendor_write_clears;
static unsigned vendor_write_flushes;

static struct Task *FindTask(const char *name)
{
    (void)name;
    return current_task;
}

static int AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    if (semaphore->held)
        return 0;
    semaphore->held = true;
    return 1;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    assert(semaphore->held);
    semaphore->held = false;
}

static uint32_t p4_usb_millis(void)
{
    return fake_ms;
}

static void reset_endpoints(void)
{
    vendor_rx_length = 0;
    vendor_tx_length = 0;
    vendor_write_room = 0;
    vendor_write_accept = 0;
    vendor_mounted = true;
    vendor_read_calls = 0;
    vendor_write_calls = 0;
    vendor_read_flushes = 0;
    vendor_write_clears = 0;
    vendor_write_flushes = 0;
    memset(vendor_rx, 0, sizeof(vendor_rx));
    memset(vendor_tx, 0, sizeof(vendor_tx));
}

static void enqueue_vendor_rx(const uint8_t *bytes, size_t length)
{
    assert(length <= sizeof(vendor_rx) - vendor_rx_length);
    memcpy(vendor_rx + vendor_rx_length, bytes, length);
    vendor_rx_length += length;
}

static bool tud_mounted(void)
{
    return vendor_mounted;
}

static uint32_t tud_vendor_available(void)
{
    return (uint32_t)vendor_rx_length;
}

static uint32_t tud_vendor_read(void *bytes, uint32_t requested)
{
    size_t count = requested < vendor_rx_length ? requested : vendor_rx_length;
    memcpy(bytes, vendor_rx, count);
    memmove(vendor_rx, vendor_rx + count, vendor_rx_length - count);
    vendor_rx_length -= count;
    ++vendor_read_calls;
    return (uint32_t)count;
}

static void tud_vendor_read_flush(void)
{
    vendor_rx_length = 0;
    ++vendor_read_flushes;
}

static uint32_t tud_vendor_write_available(void)
{
    return (uint32_t)vendor_write_room;
}

static uint32_t tud_vendor_write(const void *bytes, uint32_t requested)
{
    size_t count = requested < vendor_write_accept ? requested : vendor_write_accept;
    size_t room = sizeof(vendor_tx) - vendor_tx_length;
    if (count > room)
        count = room;
    memcpy(vendor_tx + vendor_tx_length, bytes, count);
    vendor_tx_length += count;
    ++vendor_write_calls;
    return (uint32_t)count;
}

static bool tud_vendor_write_flush(void)
{
    ++vendor_write_flushes;
    return true;
}

static bool tud_vendor_write_clear(void)
{
    vendor_tx_length = 0;
    ++vendor_write_clears;
    return true;
}

static void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "test_port_resource.c:%d: check failed: %s\n",
                line, expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)
'''


TESTS = r'''
static void fill(uint8_t *bytes, size_t length, uint8_t seed)
{
    size_t i;
    for (i = 0; i < length; ++i)
        bytes[i] = (uint8_t)(seed + i * 37u);
}

static void reset_fixture(void)
{
    p4_port_broker_init(&broker);
    lock.held = false;
    lock_held = false;
    current_task = &task_a;
    state = P4_USB_PORT_READY;
    __atomic_store_n(&invalidate_pending, false, __ATOMIC_RELEASE);
    last_client_ms = 0;
    fake_ms = 100;
    reset_endpoints();
}

static void test_claim_ownership_and_contention(void)
{
    uint32_t token;
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    uint32_t previous_lease;

    reset_fixture();
    CHECK(port_state() == P4_USB_PORT_READY);
    token = port_claim();
    CHECK(token != 0);
    CHECK(token == 1);
    CHECK(broker.claimed && broker.open);
    CHECK(broker.owner_key == (uintptr_t)&task_a);
    CHECK(last_client_ms == fake_ms);

    current_task = &task_b;
    CHECK(port_claim() == 0);
    CHECK(port_valid(token) == 0);
    CHECK(port_read(token, bytes, sizeof(bytes)) == 0);
    CHECK(port_write(token, bytes, sizeof(bytes)) == 0);
    CHECK(port_release(token) == 0);
    CHECK(broker.claimed && broker.owner_key == (uintptr_t)&task_a);

    current_task = &task_a;
    CHECK(port_valid(token + 1u) == 0);
    CHECK(port_read(token + 1u, bytes, sizeof(bytes)) == 0);
    CHECK(port_write(token + 1u, bytes, sizeof(bytes)) == 0);
    CHECK(port_release(token + 1u) == 0);
    CHECK(port_valid(token) == 1);

    previous_lease = last_client_ms;
    lock.held = true;
    CHECK(port_valid(token) == -1);
    CHECK(port_release(token) == -1);
    CHECK(last_client_ms == previous_lease);
    CHECK(broker.claimed && broker.active_generation == token);
    CHECK(!__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));
    lock.held = false;
    CHECK(port_valid(token) == 1);
}

static void test_bounded_copies_and_lease_renewal(void)
{
    uint8_t input[P4_USB_PORT_CHUNK + 1u];
    uint8_t output[P4_USB_PORT_CHUNK + 1u];
    uint8_t peek[P4_USB_PORT_CHUNK];
    uint32_t token;
    size_t count = 0;
    uint32_t lease_before;

    reset_fixture();
    fill(input, sizeof(input), 0x31u);
    memset(output, 0, sizeof(output));
    token = port_claim();
    CHECK(token != 0);
    CHECK(last_client_ms == 100u);

    fake_ms = 111u;
    CHECK(port_valid(token) == 1);
    CHECK(last_client_ms == 111u);

    fake_ms = 122u;
    CHECK(port_write(token, input, P4_USB_PORT_CHUNK) == P4_USB_PORT_CHUNK);
    CHECK(last_client_ms == 122u);
    CHECK(broker.tx_used == P4_USB_PORT_CHUNK);
    CHECK(p4_port_broker_usb_peek(&broker, &task_a, token, peek,
                                  sizeof(peek), &count) == P4_PORT_BROKER_OK);
    CHECK(count == P4_USB_PORT_CHUNK);
    CHECK(memcmp(peek, input, sizeof(peek)) == 0);
    CHECK(p4_port_broker_usb_consume(&broker, &task_a, token, count) ==
          P4_PORT_BROKER_OK);

    lease_before = last_client_ms;
    CHECK(port_write(token, input, P4_USB_PORT_CHUNK + 1u) == 0);
    CHECK(broker.tx_used == 0);
    CHECK(last_client_ms == lease_before);

    CHECK(p4_port_broker_usb_receive(&broker, &task_a, token, input,
                                     P4_USB_PORT_CHUNK, &count) ==
          P4_PORT_BROKER_OK);
    CHECK(count == P4_USB_PORT_CHUNK);
    lease_before = last_client_ms;
    CHECK(port_read(token, output, P4_USB_PORT_CHUNK + 1u) == 0);
    CHECK(broker.rx_used == P4_USB_PORT_CHUNK);
    CHECK(last_client_ms == lease_before);

    fake_ms = 133u;
    CHECK(port_read(token, output, P4_USB_PORT_CHUNK) == P4_USB_PORT_CHUNK);
    CHECK(last_client_ms == 133u);
    CHECK(memcmp(output, input, P4_USB_PORT_CHUNK) == 0);

    /* A valid empty read still renews the live client's lease. */
    fake_ms = 144u;
    CHECK(port_read(token, output, 1u) == 0);
    CHECK(last_client_ms == 144u);
}

static void test_pending_reset_and_release_cleanup(void)
{
    uint8_t bytes[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    uint32_t token, renewed;
    size_t count = 99;

    reset_fixture();
    token = port_claim();
    CHECK(token != 0);
    fake_ms = 200u;
    __atomic_store_n(&invalidate_pending, true, __ATOMIC_RELEASE);
    CHECK(port_valid(token) == 0);
    CHECK(port_read(token, bytes, sizeof(bytes)) == 0);
    CHECK(port_write(token, bytes, sizeof(bytes)) == 0);
    current_task = &task_b;
    CHECK(port_claim() == 0);
    CHECK(broker.claimed && broker.active_generation == token);

    /* Model owner-side broker retirement while the reset admission barrier
       remains raised. A new client must not claim in that interval. */
    current_task = &task_a;
    CHECK(try_lock());
    CHECK(p4_port_broker_usb_reset(&broker, &task_a, token) ==
          P4_PORT_BROKER_OK);
    unlock();
    CHECK(!broker.claimed && !broker.open);
    current_task = &task_b;
    CHECK(port_claim() == 0);

    __atomic_store_n(&invalidate_pending, false, __ATOMIC_RELEASE);
    renewed = port_claim();
    CHECK(renewed != 0 && renewed != token);
    CHECK(renewed == 2u);

    /* A completed explicit release is immediately no-service and remains
       admission-closed until the owner drains endpoint state. */
    CHECK(port_release(renewed) == 1);
    CHECK(!broker.claimed && !broker.open);
    CHECK(__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));
    current_task = &task_a;
    CHECK(port_valid(renewed) == 0);
    CHECK(port_read(renewed, bytes, sizeof(bytes)) == 0);
    CHECK(port_write(renewed, bytes, sizeof(bytes)) == 0);
    CHECK(p4_port_broker_service_read(&broker, &task_b, renewed, bytes,
                                      sizeof(bytes), &count) ==
          P4_PORT_BROKER_NO_SERVICE);
    CHECK(count == 0);
    CHECK(port_claim() == 0);
}

static void test_faulted_state_denies_all_client_access(void)
{
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    uint32_t token;

    reset_fixture();
    token = port_claim();
    CHECK(token != 0);
    set_state(P4_USB_PORT_FAULTED);
    CHECK(port_state() == P4_USB_PORT_FAULTED);
    CHECK(port_claim() == 0);
    CHECK(port_valid(token) == 0);
    CHECK(port_read(token, bytes, sizeof(bytes)) == 0);
    CHECK(port_write(token, bytes, sizeof(bytes)) == 0);
    CHECK(port_release(token) == 0);
    CHECK(broker.claimed && broker.active_generation == token);
}

static void test_owner_drains_without_service_and_moves_active_rx(void)
{
    uint8_t air[200];
    uint8_t initial[P4_PORT_BROKER_RING_CAPACITY - 124u];
    uint8_t output[512];
    uint32_t token;
    size_t accepted = 0;
    unsigned reads_before;

    reset_fixture();
    fill(air, sizeof(air), 0x51u);
    vendor_write_room = 64u;
    enqueue_vendor_rx(air, 9u);
    bulk_step();
    CHECK(vendor_rx_length == 0);
    CHECK(vendor_read_calls == 1u);
    CHECK(vendor_tx_length == 0);
    CHECK(vendor_write_calls == 0u);
    CHECK(broker.rx_used == 0 && broker.tx_used == 0);

    reset_fixture();
    token = port_claim();
    CHECK(token != 0);
    fill(initial, sizeof(initial), 0x13u);
    fill(air, sizeof(air), 0x71u);
    CHECK(p4_port_broker_usb_receive(&broker, &task_a, token, initial,
                                     sizeof(initial), &accepted) ==
          P4_PORT_BROKER_OK);
    CHECK(accepted == sizeof(initial));
    enqueue_vendor_rx(air, sizeof(air));
    bulk_step();
    CHECK(broker.rx_used == P4_PORT_BROKER_RING_CAPACITY);
    CHECK(vendor_rx_length == 76u);
    reads_before = vendor_read_calls;

    /* A full broker RX ring leaves endpoint bytes pending for the next poll. */
    bulk_step();
    CHECK(vendor_read_calls == reads_before);
    CHECK(vendor_rx_length == 76u);

    CHECK(port_read(token, output, sizeof(output)) == sizeof(output));
    CHECK(memcmp(output, initial, sizeof(output)) == 0);
    bulk_step();
    CHECK(vendor_rx_length == 0);
    CHECK(broker.rx_used == 588u);
    CHECK(port_read(token, output, sizeof(output)) == sizeof(output));
    CHECK(memcmp(output, initial + sizeof(output), sizeof(initial) - sizeof(output)) == 0);
    CHECK(memcmp(output + (sizeof(initial) - sizeof(output)), air, 124u) == 0);
    CHECK(port_read(token, output, 76u) == 76u);
    CHECK(memcmp(output, air + 124u, 76u) == 0);
}

static void test_owner_tx_partial_acceptance_and_backpressure(void)
{
    uint8_t payload[6];
    uint8_t peek[P4_USB_PORT_CHUNK];
    uint32_t token;
    size_t count = 0;

    reset_fixture();
    token = port_claim();
    CHECK(token != 0);
    fill(payload, sizeof(payload), 0x29u);
    CHECK(port_write(token, payload, sizeof(payload)) == sizeof(payload));

    vendor_write_room = 3u;
    vendor_write_accept = 2u;
    bulk_step();
    CHECK(vendor_tx_length == 2u);
    CHECK(memcmp(vendor_tx, payload, 2u) == 0);
    CHECK(broker.tx_used == 4u);
    CHECK(p4_port_broker_usb_peek(&broker, &task_a, token, peek,
                                  sizeof(peek), &count) ==
          P4_PORT_BROKER_PARTIAL);
    CHECK(count == 4u && memcmp(peek, payload + 2u, 4u) == 0);

    vendor_write_room = 4u;
    vendor_write_accept = 0u;
    bulk_step();
    CHECK(broker.tx_used == 4u);
    CHECK(vendor_tx_length == 2u);

    vendor_write_room = 4u;
    vendor_write_accept = 4u;
    bulk_step();
    CHECK(broker.tx_used == 0u);
    CHECK(vendor_tx_length == sizeof(payload));
    CHECK(memcmp(vendor_tx, payload, sizeof(payload)) == 0);
    CHECK(vendor_write_flushes == 3u);
}

static void test_owner_reset_clears_broker_and_endpoint_queues(void)
{
    uint8_t rx[4], tx_first[3], tx_queued[5], stale[2];
    uint32_t token, renewed;
    size_t accepted = 0;

    reset_fixture();
    token = port_claim();
    CHECK(token != 0);
    fill(rx, sizeof(rx), 0x41u);
    fill(tx_first, sizeof(tx_first), 0x61u);
    fill(tx_queued, sizeof(tx_queued), 0x81u);
    fill(stale, sizeof(stale), 0xa1u);

    CHECK(port_write(token, tx_first, sizeof(tx_first)) == sizeof(tx_first));
    vendor_write_room = sizeof(tx_first);
    vendor_write_accept = sizeof(tx_first);
    bulk_step();
    CHECK(vendor_tx_length == sizeof(tx_first));
    CHECK(broker.tx_used == 0u);

    CHECK(port_write(token, tx_queued, sizeof(tx_queued)) == sizeof(tx_queued));
    CHECK(p4_port_broker_usb_receive(&broker, &task_a, token, rx, sizeof(rx),
                                     &accepted) == P4_PORT_BROKER_OK);
    CHECK(accepted == sizeof(rx));
    enqueue_vendor_rx(stale, sizeof(stale));
    CHECK(broker.rx_used == sizeof(rx));
    CHECK(broker.tx_used == sizeof(tx_queued));

    __atomic_store_n(&invalidate_pending, true, __ATOMIC_RELEASE);
    CHECK(port_valid(token) == 0);
    bulk_step();

    CHECK(!broker.claimed && !broker.open);
    CHECK(broker.rx_used == 0u && broker.tx_used == 0u);
    CHECK(vendor_rx_length == 0u && vendor_tx_length == 0u);
    CHECK(vendor_read_flushes == 1u && vendor_write_clears == 1u);
    CHECK(port_valid(token) == 0);
    CHECK(!__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));

    current_task = &task_b;
    renewed = port_claim();
    CHECK(renewed != 0u && renewed != token);
    CHECK(renewed == 2u);
}

static void test_owner_lease_boundary_and_renewal(void)
{
    uint32_t token;

    reset_fixture();
    vendor_mounted = false;
    token = port_claim();
    CHECK(token != 0u);
    fake_ms = 2099u;
    bulk_step();
    CHECK(broker.claimed && broker.open);
    CHECK(!__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));

    fake_ms = 2100u;
    bulk_step();
    CHECK(!broker.claimed && !broker.open);
    CHECK(port_valid(token) == 0);

    reset_fixture();
    vendor_mounted = false;
    token = port_claim();
    CHECK(token != 0u);
    fake_ms = 1600u;
    CHECK(port_valid(token) == 1);
    CHECK(last_client_ms == 1600u);
    fake_ms = 3599u;
    bulk_step();
    CHECK(broker.claimed && broker.open);
    CHECK(!__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));

    fake_ms = 3600u;
    bulk_step();
    CHECK(!broker.claimed && !broker.open);
    CHECK(port_valid(token) == 0);
}

static void test_owner_lock_contention_preserves_all_queues(void)
{
    struct p4_port_broker before;
    uint8_t rx[16], tx[16], air[7];
    uint32_t token;
    size_t accepted = 0;
    unsigned read_calls, write_calls, read_flushes, write_clears;
    size_t rx_length, tx_length;

    reset_fixture();
    token = port_claim();
    CHECK(token != 0u);
    fill(rx, sizeof(rx), 0x11u);
    fill(tx, sizeof(tx), 0x31u);
    fill(air, sizeof(air), 0x51u);
    CHECK(p4_port_broker_usb_receive(&broker, &task_a, token, rx, sizeof(rx),
                                     &accepted) == P4_PORT_BROKER_OK);
    CHECK(port_write(token, tx, sizeof(tx)) == sizeof(tx));
    enqueue_vendor_rx(air, sizeof(air));
    vendor_tx[0] = 0x7bu;
    vendor_tx_length = 1u;
    vendor_write_room = sizeof(tx);
    vendor_write_accept = sizeof(tx);
    memcpy(&before, &broker, sizeof(before));
    read_calls = vendor_read_calls;
    write_calls = vendor_write_calls;
    read_flushes = vendor_read_flushes;
    write_clears = vendor_write_clears;
    rx_length = vendor_rx_length;
    tx_length = vendor_tx_length;

    fake_ms = 2100u; /* Would expire the client if the owner got the lock. */
    lock.held = true;
    bulk_step();
    CHECK(memcmp(&broker, &before, sizeof(broker)) == 0);
    CHECK(vendor_rx_length == rx_length && vendor_tx_length == tx_length);
    CHECK(memcmp(vendor_rx, air, sizeof(air)) == 0);
    CHECK(vendor_tx[0] == 0x7bu);
    CHECK(vendor_read_calls == read_calls && vendor_write_calls == write_calls);
    CHECK(vendor_read_flushes == read_flushes && vendor_write_clears == write_clears);
    CHECK(!__atomic_load_n(&invalidate_pending, __ATOMIC_ACQUIRE));
    CHECK(!lock_held);
    lock.held = false;
}

int main(void)
{
    test_claim_ownership_and_contention();
    test_bounded_copies_and_lease_renewal();
    test_pending_reset_and_release_cleanup();
    test_faulted_state_denies_all_client_access();
    test_owner_drains_without_service_and_moves_active_rx();
    test_owner_tx_partial_acceptance_and_backpressure();
    test_owner_reset_clears_broker_and_endpoint_queues();
    test_owner_lease_boundary_and_renewal();
    test_owner_lock_contention_preserves_all_queues();
    printf("PASS: %u source-bound port resource checks\n", checks);
    return EXIT_SUCCESS;
}
'''


def extract_wrappers() -> str:
    source = RESOURCE.read_text(encoding="utf-8")
    if source.count(START_MARKER) != 1 or source.count(END_MARKER) != 1:
        raise RuntimeError("resource wrapper extraction markers are missing or ambiguous")
    start = source.index(START_MARKER)
    end = source.index(END_MARKER)
    if end <= start:
        raise RuntimeError("resource wrapper extraction markers are out of order")
    wrappers = source[start:end]
    required = ("port_claim", "port_valid", "port_release", "port_read", "port_write")
    missing = [name for name in required if name not in wrappers]
    if missing:
        raise RuntimeError(f"extracted section is missing API wrappers: {missing}")
    return wrappers


def extract_bulk_step() -> str:
    source = RESOURCE.read_text(encoding="utf-8")
    if source.count(BULK_START_MARKER) != 1 or source.count(BULK_END_MARKER) != 1:
        raise RuntimeError("bulk_step extraction markers are missing or ambiguous")
    start = source.index(BULK_START_MARKER)
    end = source.index(BULK_END_MARKER)
    if end <= start:
        raise RuntimeError("bulk_step extraction markers are out of order")
    body = source[start:end]
    if "p4_port_broker_usb_reset" not in body or "tud_vendor_read_flush" not in body:
        raise RuntimeError("extracted bulk_step does not include reset queue handling")
    return body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-dir", type=Path,
                        help="keep generated fixture, binaries and logs in this directory")
    args = parser.parse_args()

    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        print("FAIL: CC is empty", file=sys.stderr)
        return 1

    wrappers = extract_wrappers()
    bulk_step = extract_bulk_step()
    assembled = PRELUDE + "\n" + wrappers + "\n" + bulk_step + "\n" + TESTS
    digest = hashlib.sha256(assembled.encode("utf-8")).hexdigest()
    broker_digest = hashlib.sha256(BROKER.read_bytes()).hexdigest()
    print(f"Extracted wrapper source: {RESOURCE}")
    print(f"Wrapper markers: {START_MARKER!r} .. {END_MARKER!r}")
    print(f"Owner markers: {BULK_START_MARKER!r} .. {BULK_END_MARKER!r}")
    print(f"SHA256 assembled fixture: {digest}")
    print(f"SHA256 production broker: {broker_digest}")

    if args.evidence_dir:
        directory = args.evidence_dir.resolve()
        directory.mkdir(parents=True, exist_ok=True)
        keep_artifacts = True
    else:
        temporary = tempfile.TemporaryDirectory(prefix="p4-usb-port-resource-")
        directory = Path(temporary.name)
        keep_artifacts = False

    fixture = directory / "test_port_resource_fixture.c"
    fixture.write_text(assembled, encoding="utf-8")
    sanitizer_environment = os.environ.copy()
    sanitizer_environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    sanitizer_environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    failures = 0

    for optimization in (0, 2):
        binary = directory / f"test_port_resource_o{optimization}"
        build_log = directory / f"test_port_resource_o{optimization}.build.log"
        run_log = directory / f"test_port_resource_o{optimization}.run.log"
        command = compiler + [
            "-std=c11", "-g", f"-O{optimization}",
            "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(USB), str(fixture), str(BROKER), "-o", str(binary),
        ]
        print(f"RUN native-O{optimization}-compile: {shlex.join(command)}", flush=True)
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, check=False,
                                  timeout=60)
        build_log.write_text(compiled.stdout, encoding="utf-8")
        if compiled.stdout:
            print(compiled.stdout, end="" if compiled.stdout.endswith("\n") else "\n")
        print(f"EXIT native-O{optimization}-compile: {compiled.returncode}", flush=True)
        if compiled.returncode != 0:
            failures += 1
            continue

        print(f"RUN native-O{optimization}-test: {binary}", flush=True)
        tested = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False,
                                env=sanitizer_environment, timeout=60)
        run_log.write_text(tested.stdout, encoding="utf-8")
        if tested.stdout:
            print(tested.stdout, end="" if tested.stdout.endswith("\n") else "\n")
        print(f"EXIT native-O{optimization}-test: {tested.returncode}", flush=True)
        failures += tested.returncode != 0

    if keep_artifacts:
        print(f"Evidence directory: {directory}")
    else:
        temporary.cleanup()
    print(f"RESULT {'FAIL' if failures else 'PASS'} ({failures} failed step(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
