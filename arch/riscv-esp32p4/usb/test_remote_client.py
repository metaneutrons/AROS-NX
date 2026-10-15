#!/usr/bin/env python3
"""Source-bound lifecycle tests for the DOS USB remote client entry point.

The fixture includes the production remote_client.c with only its main symbol
renamed. Exec, DOS, the permanent USB resource, and the RemoteDiag bridge are
bounded fakes; no USB stack, device, or controller is opened.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import textwrap


USB = Path(__file__).resolve().parent
REPO = USB.parents[2]
CLIENT = USB / "remote_client.c"
BRIDGE_INCLUDE = REPO / "workbench" / "tools" / "RemoteDiag"


FAKE_HEADERS = {
    "exec/libraries.h": r'''#ifndef TEST_EXEC_LIBRARIES_H
#define TEST_EXEC_LIBRARIES_H
struct Library { int unused; };
struct ExecBase { int unused; };
#endif
''',
    "dos/dos.h": r'''#ifndef TEST_DOS_DOS_H
#define TEST_DOS_DOS_H
#include <stdint.h>
typedef intptr_t LONG;
typedef intptr_t SIPTR;
typedef const char *CONST_STRPTR;
struct RDArgs { int unused; };
#define RETURN_OK ((LONG)0)
#define RETURN_FAIL ((LONG)20)
#define SIGBREAKF_CTRL_C (1ul << 12)
#endif
''',
    "proto/dos.h": r'''#ifndef TEST_PROTO_DOS_H
#define TEST_PROTO_DOS_H
#include <dos/dos.h>
struct RDArgs *ReadArgs(CONST_STRPTR, SIPTR *, struct RDArgs *);
LONG IoErr(void);
void PrintFault(LONG, CONST_STRPTR);
void FreeArgs(struct RDArgs *);
#endif
''',
    "proto/exec.h": r'''#ifndef TEST_PROTO_EXEC_H
#define TEST_PROTO_EXEC_H
#include <dos/dos.h>
void *OpenResource(CONST_STRPTR);
unsigned long SetSignal(unsigned long, unsigned long);
void Delay(unsigned long);
#endif
''',
}


FIXTURE = r'''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dos/dos.h>
#include "port_resource.h"

static int fake_puts(const char *text);
#define puts fake_puts
#define main extracted_remote_client_main
#include "remote_client.c"
#undef main
#undef puts

#define EVENT_CAPACITY 512u
#define VALUE_CAPACITY 64u

enum event_type {
    EVENT_BRIDGE_OPEN = 1,
    EVENT_CLAIM,
    EVENT_VALID,
    EVENT_RESET,
    EVENT_POLL,
    EVENT_READ,
    EVENT_WRITE,
    EVENT_RELEASE,
    EVENT_BRIDGE_CLOSE,
    EVENT_DELAY,
    EVENT_STATE
};

struct event { enum event_type type; uint32_t value; };
struct rd_bridge { int marker; };

static struct RDArgs fake_rdargs;
static struct P4USBPort fake_port;
static struct rd_bridge fake_bridge;
static struct event events[EVENT_CAPACITY];
static size_t event_count;
static SIPTR configured_args[7];
static LONG configured_seconds;
static const char fake_screen[] = "Workbench";
static const char fake_root[] = "SYS:Prefs";
static const uint8_t expected_read[] = { 0xa0u, 0xa1u, 0xa2u, 0xa3u };
static const uint8_t expected_write[] = { 0x91u, 0x92u, 0x93u };
static uint32_t claim_script[VALUE_CAPACITY];
static size_t claim_script_count, claim_script_index;
static int valid_script[VALUE_CAPACITY];
static size_t valid_script_count, valid_script_index;
static int release_script[VALUE_CAPACITY];
static size_t release_script_count, release_script_index;
static uint32_t state_script[VALUE_CAPACITY];
static size_t state_script_count, state_script_index;
static uint32_t observed_read_tokens[VALUE_CAPACITY];
static size_t observed_read_count;
static uint32_t observed_write_tokens[VALUE_CAPACITY];
static size_t observed_write_count;
static uint32_t observed_release_tokens[VALUE_CAPACITY];
static size_t observed_release_count;
static uint32_t expected_io_token;
static uint32_t owner_state;
static int readargs_fails;
static int resource_available;
static int bridge_open_fails;
static int print_fault_count;
static int free_args_count;
static int open_resource_count;
static int bridge_open_count;
static int bridge_close_count;
static int bridge_reset_count;
static int bridge_poll_count;
static int bridge_feed_count;
static int bridge_peek_count;
static int bridge_consume_count;
static int claim_count;
static int valid_count;
static int read_count;
static int write_count;
static int release_count;
static int delay_count;
static int signal_count;
static int signal_break_on_call;
static int puts_count;
static size_t last_peek_size;
static unsigned checks;

static void fail_check(const char *expression, int line)
{
    fprintf(stderr, "client fixture:%d: check failed: %s\n", line,
            expression);
    exit(EXIT_FAILURE);
}

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) fail_check(#expression, __LINE__); \
} while (0)

static void record_event(enum event_type type, uint32_t value)
{
    CHECK(event_count < EVENT_CAPACITY);
    events[event_count].type = type;
    events[event_count].value = value;
    ++event_count;
}

static int find_event(enum event_type type, size_t start)
{
    size_t i;
    for (i = start; i < event_count; ++i)
        if (events[i].type == type) return (int)i;
    return -1;
}

static int fake_puts(const char *text)
{
    CHECK(text != NULL);
    ++puts_count;
    return 0;
}

struct RDArgs *ReadArgs(CONST_STRPTR template, SIPTR *args,
                        struct RDArgs *allocated)
{
    CHECK(strcmp(template,
        "ENABLE/S,NONEAUTH/S,SCREEN/K/A,ROOT/K,ALLOWMOUSE/S,"
        "UNATTENDED/S,SECONDS/N") == 0);
    CHECK(allocated == NULL);
    if (readargs_fails) return NULL;
    memcpy(args, configured_args, sizeof(configured_args));
    return &fake_rdargs;
}

LONG IoErr(void)
{
    return 77;
}

void PrintFault(LONG error, CONST_STRPTR name)
{
    CHECK(error == 77);
    CHECK(strcmp(name, "USBRemote") == 0);
    ++print_fault_count;
}

void FreeArgs(struct RDArgs *args)
{
    CHECK(args == &fake_rdargs);
    ++free_args_count;
}

static uint32_t fake_state(void)
{
    uint32_t result = owner_state;
    if (state_script_index < state_script_count)
        result = state_script[state_script_index++];
    ++valid_count;
    record_event(EVENT_STATE, result);
    return result;
}

static uint32_t fake_claim(void)
{
    uint32_t result = 0x55u;
    if (claim_script_index < claim_script_count)
        result = claim_script[claim_script_index++];
    ++claim_count;
    record_event(EVENT_CLAIM, result);
    return result;
}

static int fake_valid(uint32_t token)
{
    int result = 1;
    CHECK(token != 0);
    if (valid_script_index < valid_script_count)
        result = valid_script[valid_script_index++];
    ++valid_count;
    record_event(EVENT_VALID, token);
    return result;
}

static int fake_release(uint32_t token)
{
    int result = 0;
    CHECK(token != 0);
    CHECK(observed_release_count < VALUE_CAPACITY);
    observed_release_tokens[observed_release_count++] = token;
    if (release_script_index < release_script_count)
        result = release_script[release_script_index++];
    ++release_count;
    record_event(EVENT_RELEASE, (uint32_t)result);
    return result;
}

static uint32_t fake_read(uint32_t token, void *buffer, uint32_t capacity)
{
    size_t count = sizeof(expected_read);
    CHECK(token != 0);
    CHECK(expected_io_token == 0 || token == expected_io_token);
    CHECK(buffer != NULL);
    CHECK(capacity >= count);
    CHECK(observed_read_count < VALUE_CAPACITY);
    observed_read_tokens[observed_read_count++] = token;
    memcpy(buffer, expected_read, count);
    ++read_count;
    record_event(EVENT_READ, token);
    return (uint32_t)count;
}

static uint32_t fake_write(uint32_t token, const void *buffer,
                           uint32_t length)
{
    CHECK(token != 0);
    CHECK(expected_io_token == 0 || token == expected_io_token);
    CHECK(buffer == expected_write);
    CHECK(length == sizeof(expected_write));
    CHECK(observed_write_count < VALUE_CAPACITY);
    observed_write_tokens[observed_write_count++] = token;
    ++write_count;
    record_event(EVENT_WRITE, token);
    return length > 1u ? 2u : length;
}

void *OpenResource(CONST_STRPTR name)
{
    CHECK(strcmp(name, P4_USB_PORT_NAME) == 0);
    ++open_resource_count;
    return resource_available ? &fake_port : NULL;
}

unsigned long SetSignal(unsigned long new_signals, unsigned long signal_mask)
{
    CHECK(new_signals == 0);
    CHECK(signal_mask == SIGBREAKF_CTRL_C);
    ++signal_count;
    return signal_break_on_call == signal_count ? SIGBREAKF_CTRL_C : 0;
}

void Delay(unsigned long ticks)
{
    CHECK(ticks == 1);
    ++delay_count;
    record_event(EVENT_DELAY, (uint32_t)ticks);
}

struct rd_bridge *rd_bridge_open(const char *screen, const char *root,
                                 bool mouse)
{
    CHECK(screen != NULL);
    CHECK(strcmp(screen, fake_screen) == 0);
    CHECK(root == NULL || strcmp(root, fake_root) == 0);
    CHECK(mouse == (configured_args[4] != 0));
    ++bridge_open_count;
    record_event(EVENT_BRIDGE_OPEN, (uint32_t)mouse);
    return bridge_open_fails ? NULL : &fake_bridge;
}

void rd_bridge_close(struct rd_bridge *bridge)
{
    CHECK(bridge == NULL || bridge == &fake_bridge);
    ++bridge_close_count;
    record_event(EVENT_BRIDGE_CLOSE, bridge != NULL);
}

void rd_bridge_reset(struct rd_bridge *bridge)
{
    CHECK(bridge == &fake_bridge);
    ++bridge_reset_count;
    record_event(EVENT_RESET, 0);
}

void rd_bridge_poll(struct rd_bridge *bridge)
{
    CHECK(bridge == &fake_bridge);
    ++bridge_poll_count;
    record_event(EVENT_POLL, 0);
}

size_t rd_bridge_room(struct rd_bridge *bridge)
{
    CHECK(bridge == &fake_bridge);
    return 8u;
}

size_t rd_bridge_feed(struct rd_bridge *bridge, const uint8_t *bytes,
                      size_t length)
{
    CHECK(bridge == &fake_bridge);
    CHECK(length == sizeof(expected_read));
    CHECK(memcmp(bytes, expected_read, sizeof(expected_read)) == 0);
    ++bridge_feed_count;
    return length;
}

size_t rd_bridge_peek(struct rd_bridge *bridge, const uint8_t **bytes)
{
    CHECK(bridge == &fake_bridge);
    *bytes = expected_write;
    last_peek_size = sizeof(expected_write);
    ++bridge_peek_count;
    return last_peek_size;
}

bool rd_bridge_consume(struct rd_bridge *bridge, size_t length)
{
    CHECK(bridge == &fake_bridge);
    CHECK(length <= last_peek_size);
    ++bridge_consume_count;
    return true;
}

static void fixture_reset(void)
{
    size_t i;
    memset(&fake_port, 0, sizeof(fake_port));
    memset(events, 0, sizeof(events));
    memset(configured_args, 0, sizeof(configured_args));
    memset(claim_script, 0, sizeof(claim_script));
    memset(valid_script, 0, sizeof(valid_script));
    memset(release_script, 0, sizeof(release_script));
    memset(state_script, 0, sizeof(state_script));
    memset(observed_read_tokens, 0, sizeof(observed_read_tokens));
    memset(observed_write_tokens, 0, sizeof(observed_write_tokens));
    memset(observed_release_tokens, 0, sizeof(observed_release_tokens));
    event_count = 0;
    configured_seconds = 1;
    configured_args[0] = 1;
    configured_args[1] = 1;
    configured_args[2] = (SIPTR)(uintptr_t)fake_screen;
    configured_args[3] = (SIPTR)(uintptr_t)fake_root;
    configured_args[4] = 0;
    configured_args[5] = 0;
    configured_args[6] = (SIPTR)(uintptr_t)&configured_seconds;
    claim_script_count = claim_script_index = 0;
    valid_script_count = valid_script_index = 0;
    release_script_count = release_script_index = 0;
    state_script_count = state_script_index = 0;
    expected_io_token = 0;
    owner_state = P4_USB_PORT_READY;
    readargs_fails = 0;
    resource_available = 1;
    bridge_open_fails = 0;
    print_fault_count = free_args_count = 0;
    open_resource_count = bridge_open_count = bridge_close_count = 0;
    bridge_reset_count = bridge_poll_count = bridge_feed_count = 0;
    bridge_peek_count = bridge_consume_count = 0;
    claim_count = valid_count = read_count = write_count = release_count = 0;
    delay_count = signal_count = puts_count = 0;
    signal_break_on_call = 0;
    observed_read_count = observed_write_count = observed_release_count = 0;
    last_peek_size = 0;
    for (i = 0; i < EVENT_CAPACITY; ++i) {
        events[i].type = 0;
        events[i].value = 0;
    }
    fake_port.abi = P4_USB_PORT_ABI;
    fake_port.size = (uint32_t)sizeof(fake_port);
    fake_port.state = fake_state;
    fake_port.claim = fake_claim;
    fake_port.valid = fake_valid;
    fake_port.release = fake_release;
    fake_port.read = fake_read;
    fake_port.write = fake_write;
}

static void set_claims(const uint32_t *values, size_t count)
{
    CHECK(count <= VALUE_CAPACITY);
    memcpy(claim_script, values, count * sizeof(values[0]));
    claim_script_count = count;
    claim_script_index = 0;
}

static void set_valids(const int *values, size_t count)
{
    CHECK(count <= VALUE_CAPACITY);
    memcpy(valid_script, values, count * sizeof(values[0]));
    valid_script_count = count;
    valid_script_index = 0;
}

static void set_releases(const int *values, size_t count)
{
    CHECK(count <= VALUE_CAPACITY);
    memcpy(release_script, values, count * sizeof(values[0]));
    release_script_count = count;
    release_script_index = 0;
}

static void set_states(const uint32_t *values, size_t count)
{
    CHECK(count <= VALUE_CAPACITY);
    memcpy(state_script, values, count * sizeof(values[0]));
    state_script_count = count;
    state_script_index = 0;
}

static void expect_no_admission(void)
{
    CHECK(open_resource_count == 0);
    CHECK(bridge_open_count == 0);
    CHECK(claim_count == 0);
    CHECK(bridge_close_count == 1);
    CHECK(free_args_count == 1);
}

static void test_readargs_failure(void)
{
    fixture_reset();
    readargs_fails = 1;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(print_fault_count == 1);
    CHECK(free_args_count == 0);
    CHECK(open_resource_count == 0);
    CHECK(bridge_close_count == 0);
}

static void test_argument_guards(void)
{
    fixture_reset();
    configured_args[0] = 0;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    expect_no_admission();

    fixture_reset();
    configured_args[1] = 0;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    expect_no_admission();

    fixture_reset();
    configured_args[4] = 1;
    configured_args[5] = 0;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    expect_no_admission();

    fixture_reset();
    configured_seconds = 0;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    expect_no_admission();

    fixture_reset();
    configured_seconds = 601;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    expect_no_admission();
}

static void test_resource_availability_and_abi(void)
{
    fixture_reset();
    resource_available = 0;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(open_resource_count == 1);
    CHECK(bridge_open_count == 0);
    CHECK(free_args_count == 1);
    CHECK(bridge_close_count == 1);

    fixture_reset();
    fake_port.abi = P4_USB_PORT_ABI + 1u;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_open_count == 0);
    CHECK(claim_count == 0);
    CHECK(bridge_close_count == 1);

    fixture_reset();
    fake_port.size = (uint32_t)offsetof(struct P4USBPort, snapshot) - 1u;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_open_count == 0);
    CHECK(bridge_close_count == 1);

    fixture_reset();
    owner_state = P4_USB_PORT_FAULTED;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_open_count == 0);
    CHECK(claim_count == 0);
    CHECK(bridge_close_count == 1);

    fixture_reset();
    fake_port.write = NULL;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_open_count == 0);
    CHECK(claim_count == 0);
    CHECK(bridge_close_count == 1);
}

static void test_worker_failure_and_busy_claim(void)
{
    fixture_reset();
    bridge_open_fails = 1;
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_open_count == 1);
    CHECK(claim_count == 0);
    CHECK(bridge_close_count == 1);
    CHECK(find_event(EVENT_BRIDGE_OPEN, 0) < find_event(EVENT_BRIDGE_CLOSE, 0));

    fixture_reset();
    {
        const uint32_t claims[] = { 0u };
        set_claims(claims, 1);
    }
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(claim_count == 1);
    CHECK(bridge_open_count == 1);
    CHECK(release_count == 0);
    CHECK(find_event(EVENT_BRIDGE_OPEN, 0) < find_event(EVENT_CLAIM, 0));
    CHECK(find_event(EVENT_CLAIM, 0) < find_event(EVENT_BRIDGE_CLOSE, 0));
}

static void test_timeout_lifecycle_and_release_contention(void)
{
    const uint32_t claims[] = { 0x11u };
    const int releases[] = { -1, -1, 0 };
    int release_event, close_event;

    fixture_reset();
    /* ABI1 without the optional snapshot tail remains a valid remote port. */
    fake_port.size = (uint32_t)offsetof(struct P4USBPort, snapshot);
    set_claims(claims, 1);
    set_releases(releases, 3);
    CHECK(extracted_remote_client_main() == RETURN_OK);
    CHECK(claim_count == 1);
    CHECK(signal_count == 50);
    CHECK(bridge_poll_count == 50);
    CHECK(read_count == 50 && bridge_feed_count == 50);
    CHECK(write_count == 50 && bridge_consume_count == 50);
    CHECK(release_count == 3);
    CHECK(delay_count == 52);
    CHECK(bridge_close_count == 1);
    CHECK(free_args_count == 1);
    CHECK(find_event(EVENT_BRIDGE_OPEN, 0) < find_event(EVENT_CLAIM, 0));
    release_event = find_event(EVENT_RELEASE, 0);
    close_event = find_event(EVENT_BRIDGE_CLOSE, 0);
    CHECK(release_event >= 0 && close_event > release_event);
    CHECK(observed_release_count == 3);
    CHECK(observed_release_tokens[0] == 0x11u);
    CHECK(observed_release_tokens[1] == 0x11u);
    CHECK(observed_release_tokens[2] == 0x11u);
}

static void test_ctrl_c_shutdown(void)
{
    const uint32_t claims[] = { 0x21u };
    int release_event, close_event;

    fixture_reset();
    set_claims(claims, 1);
    signal_break_on_call = 1;
    CHECK(extracted_remote_client_main() == RETURN_OK);
    CHECK(signal_count == 1);
    CHECK(bridge_poll_count == 0);
    CHECK(read_count == 0 && write_count == 0);
    CHECK(release_count == 1);
    CHECK(delay_count == 0);
    release_event = find_event(EVENT_RELEASE, 0);
    close_event = find_event(EVENT_BRIDGE_CLOSE, 0);
    CHECK(release_event >= 0 && close_event > release_event);
}

static void test_valid_lock_contention(void)
{
    const uint32_t claims[] = { 0x31u };
    const int valid[] = { -1 };

    fixture_reset();
    set_claims(claims, 1);
    set_valids(valid, 1);
    CHECK(extracted_remote_client_main() == RETURN_OK);
    CHECK(bridge_poll_count == 49);
    CHECK(read_count == 49 && write_count == 49);
    CHECK(delay_count == 50);
    CHECK(release_count == 1);
}

static void test_reset_failed_reclaim_then_success(void)
{
    const uint32_t claims[] = { 0x41u, 0u, 0x42u };
    const int valid[] = { 0 };
    const int releases[] = { 0 };
    int reset_event, second_claim_event;
    size_t i;

    fixture_reset();
    set_claims(claims, 3);
    set_valids(valid, 1);
    set_releases(releases, 1);
    expected_io_token = 0x42u;
    CHECK(extracted_remote_client_main() == RETURN_OK);
    CHECK(claim_count == 3);
    CHECK(bridge_reset_count == 1);
    CHECK(bridge_poll_count >= 51);
    CHECK(read_count == 49 && write_count == 49);
    CHECK(release_count == 1);
    CHECK(observed_release_tokens[0] == 0x42u);
    CHECK(observed_read_count == 49 && observed_write_count == 49);
    for (i = 0; i < observed_read_count; ++i)
        CHECK(observed_read_tokens[i] == 0x42u);
    for (i = 0; i < observed_write_count; ++i)
        CHECK(observed_write_tokens[i] == 0x42u);
    reset_event = find_event(EVENT_RESET, 0);
    second_claim_event = find_event(EVENT_CLAIM,
                                    (size_t)find_event(EVENT_CLAIM, 0) + 1u);
    CHECK(reset_event >= 0 && second_claim_event > reset_event);
    CHECK(find_event(EVENT_RELEASE, 0) < find_event(EVENT_BRIDGE_CLOSE, 0));
}

static void test_owner_fault_during_tokenless_retry(void)
{
    const uint32_t claims[] = { 0x51u, 0u };
    const int valid[] = { 0 };
    const uint32_t states[] = {
        P4_USB_PORT_READY,
        P4_USB_PORT_READY,
        P4_USB_PORT_FAULTED
    };

    fixture_reset();
    set_claims(claims, 2);
    set_valids(valid, 1);
    set_states(states, sizeof(states) / sizeof(states[0]));
    CHECK(extracted_remote_client_main() == RETURN_FAIL);
    CHECK(bridge_reset_count == 1);
    CHECK(claim_count == 2);
    CHECK(read_count == 0 && write_count == 0);
    CHECK(release_count == 0);
    CHECK(bridge_close_count == 1);
    CHECK(find_event(EVENT_RESET, 0) >= 0);
    CHECK(find_event(EVENT_RESET,
                     (size_t)find_event(EVENT_RESET, 0) + 1u) == -1);
}

int main(void)
{
    test_readargs_failure();
    test_argument_guards();
    test_resource_availability_and_abi();
    test_worker_failure_and_busy_claim();
    test_timeout_lifecycle_and_release_contention();
    test_ctrl_c_shutdown();
    test_valid_lock_contention();
    test_reset_failed_reclaim_then_success();
    test_owner_fault_during_tokenless_retry();
    printf("remote_client source-bound API tests: PASS (%u checks)\n", checks);
    return EXIT_SUCCESS;
}
'''


def write_fixture_files(root: Path) -> tuple[Path, Path]:
    include = root / "fake_include"
    for relative, content in FAKE_HEADERS.items():
        target = include / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    fixture = root / "client_api_fixture.c"
    fixture.write_text(FIXTURE, encoding="utf-8")
    return fixture, include


def run_one(root: Path, optimization: str, compiler: list[str]) -> tuple[str, str]:
    fixture, include = write_fixture_files(root)
    binary = root / f"client-api-{optimization}"
    command = compiler + [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        f"-{optimization}",
        "-g",
        f"-I{include}",
        f"-I{USB}",
        f"-I{BRIDGE_INCLUDE}",
        str(fixture),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(command, text=True, capture_output=True)
    command_text = shlex.join(command)
    if compiled.returncode:
        raise RuntimeError(
            f"{optimization} compile failed ({compiled.returncode})\n"
            f"$ {command_text}\n{compiled.stdout}{compiled.stderr}"
        )
    env = os.environ.copy()
    # Apple Clang's AddressSanitizer does not implement LeakSanitizer.
    env["ASAN_OPTIONS"] = "halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    executed = subprocess.run([str(binary)], text=True, capture_output=True,
                              env=env)
    if executed.returncode:
        raise RuntimeError(
            f"{optimization} test failed ({executed.returncode})\n"
            f"$ {binary}\n{executed.stdout}{executed.stderr}"
        )
    return (
        f"$ {command_text}\nexit={compiled.returncode}\n"
        f"{compiled.stdout}{compiled.stderr}",
        f"$ {binary}\nexit={executed.returncode}\n"
        f"{executed.stdout}{executed.stderr}",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        help="save fixture, binaries, commands, and logs in a new evidence directory",
    )
    options = parser.parse_args()

    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        parser.error("CC resolved to an empty command")

    evidence_dir = options.evidence_dir
    if evidence_dir is None:
        with tempfile.TemporaryDirectory(prefix="usb-remote-client-") as name:
            scratch = Path(name)
            outputs = [run_one(scratch / optimization, optimization, compiler)
                       for optimization in ("O0", "O2")]
            for optimization, (compile_log, run_log) in zip(("O0", "O2"), outputs):
                print(f"{optimization}: {run_log.strip()}")
            return 0

    if evidence_dir.exists() and any(evidence_dir.iterdir()):
        parser.error(f"evidence directory is not empty: {evidence_dir}")
    evidence_dir.mkdir(parents=True, exist_ok=True)
    compiler_version = subprocess.run(compiler + ["--version"], text=True,
                                      capture_output=True, check=False)
    (evidence_dir / "source.sha256").write_text(
        f"{hashlib.sha256(CLIENT.read_bytes()).hexdigest()}  {CLIENT.name}\n",
        encoding="utf-8",
    )
    (evidence_dir / "compiler.txt").write_text(
        shlex.join(compiler) + "\n" + compiler_version.stdout +
        compiler_version.stderr,
        encoding="utf-8",
    )
    outputs = []
    for optimization in ("O0", "O2"):
        build_root = evidence_dir / optimization
        build_root.mkdir()
        outputs.append(run_one(build_root, optimization, compiler))
    for optimization, (compile_log, run_log) in zip(("O0", "O2"), outputs):
        (evidence_dir / f"compile-{optimization}.log").write_text(
            compile_log, encoding="utf-8")
        (evidence_dir / f"run-{optimization}.log").write_text(
            run_log, encoding="utf-8")
    (evidence_dir / "summary.txt").write_text(
        "Source-bound client API test; fake Exec/DOS/resource/rd_bridge APIs.\n"
        "No USB device, controller, or hardware was opened.\n"
        f"Source: {CLIENT}\n"
        f"Source SHA-256: {hashlib.sha256(CLIENT.read_bytes()).hexdigest()}\n"
        "Configurations: ASan+UBSan, -O0 and -O2, -Wall -Wextra -Werror.\n"
        "Scenarios: ReadArgs failure; missing ENABLE/NONEAUTH; mouse without\n"
        "UNATTENDED; seconds bounds; unavailable/wrong ABI/short ABI/not-ready\n"
        "resource; worker failure; busy claim; timeout; Ctrl-C; valid()\n"
        "contention; bus reset with one failed reclaim then success; owner fault\n"
        "while tokenless; release retries and release-before-bridge-close.\n"
        + "\n".join(run_log.strip() for _, run_log in zip(
            ("O0", "O2"), (pair[1] for pair in outputs))) + "\n",
        encoding="utf-8",
    )
    for optimization, (_, run_log) in zip(("O0", "O2"), outputs):
        print(f"{optimization}: {run_log.strip()}")
    print(f"evidence: {evidence_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
