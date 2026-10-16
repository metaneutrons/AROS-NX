#!/usr/bin/env python3
"""Compile a mocked model around the actual rejected-R5 observer."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import sys
import tempfile


PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
RUNTIME_SOURCE = PORT / "network" / "runtime_probe.c"
ADAPTER_SOURCE = PORT / "tools" / "test-wifi-register-adapter.py"
INSERT_CONTEXT = "/* INSERT_ACTUAL_WIFI_CONTEXT */"
INSERT_OBSERVER = "/* INSERT_ACTUAL_WIFI_OBSERVER */"
INITIAL_REJECTED = "UINT32_C(0xdeadbeef)"


FIXTURE = r'''/* Generated in a temporary directory by test-wifi-rejected-r5.py. */
#include <hardware/mmc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "registers.h"

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define P4_WIFI_SDIO_RESET_QUERY 1
#define P4_BOARD_WIFI_SLOT 1U

enum P4HostCommandObservation
{
    P4HOST_OBSERVE_START,
    P4HOST_OBSERVE_SAMPLE,
    P4HOST_OBSERVE_FINISH
};

enum P4HostCommandStatus
{
    P4HOST_COMMAND_OK,
    P4HOST_COMMAND_REFUSED,
    P4HOST_COMMAND_TIMEOUT,
    P4HOST_COMMAND_ERROR,
    P4HOST_COMMAND_CARD_ERROR,
    P4HOST_COMMAND_DMA_ERROR
};

struct P4HostCommand
{
    unsigned slot;
    ULONG index;
    ULONG response_flags;
    void *data;
    ULONG length;
    BOOL writing;
};

struct P4HostCommandResult
{
    enum P4HostCommandStatus status;
    BOOL started;
    ULONG raw;
};

struct P4SDMMCHost;

/* INSERT_ACTUAL_WIFI_CONTEXT */

static unsigned checks;
static unsigned cases;
static unsigned wifi_read_calls;
static IPTR wifi_read_address;
static struct wifi_probe_context test_context;
static struct P4HostCommand test_request;
static struct P4HostCommandResult test_outcome;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static ULONG wifi_read(IPTR address)
{
    ++wifi_read_calls;
    wifi_read_address = address;
    CHECK(address == (IPTR)(P4_SDMMC_BASE + P4SD_RESP0));
    return UINT32_C(0x8000);
}

/* INSERT_ACTUAL_WIFI_OBSERVER */

static void begin_case(void)
{
    ++cases;
    memset(&test_context, 0, sizeof(test_context));
    test_context.rejected_r5 = UINT32_C(0xdeadbeef);
    test_context.rejected_r5_valid = FALSE;
    memset(&test_request, 0, sizeof(test_request));
    test_request.slot = P4_BOARD_WIFI_SLOT;
    test_request.index = 52U;
    test_request.response_flags = MMC_RSP_R5;
    test_request.data = NULL;
    test_request.length = 0U;
    test_request.writing = FALSE;
    memset(&test_outcome, 0, sizeof(test_outcome));
    test_outcome.started = TRUE;
    test_outcome.status = P4HOST_COMMAND_CARD_ERROR;
    test_outcome.raw = P4SD_INT_CMD_DONE;
    wifi_read_calls = 0U;
    wifi_read_address = 0U;
}

static void observe(enum P4HostCommandObservation stage)
{
    wifi_observe_rejected_r5(&test_context, &test_request, stage,
                             &test_outcome);
}

static void check_refused(void)
{
    observe(P4HOST_OBSERVE_FINISH);
    CHECK(wifi_read_calls == 0U);
    CHECK(test_context.rejected_r5 == UINT32_C(0xdeadbeef));
    CHECK(test_context.rejected_r5_valid == FALSE);
}

static void test_qualified_rejected_r5_is_published(void)
{
    begin_case();
    observe(P4HOST_OBSERVE_FINISH);
    CHECK(wifi_read_calls == 1U);
    CHECK(wifi_read_address == (IPTR)(P4_SDMMC_BASE + P4SD_RESP0));
    CHECK(test_context.rejected_r5 == UINT32_C(0x8000));
    CHECK(test_context.rejected_r5_valid == TRUE);
}

static void test_other_observation_stages_are_ignored(void)
{
    enum P4HostCommandObservation stage;
    for (stage = P4HOST_OBSERVE_START; stage < P4HOST_OBSERVE_FINISH;
         stage = (enum P4HostCommandObservation)(stage + 1))
    {
        begin_case();
        observe(stage);
        CHECK(wifi_read_calls == 0U);
        CHECK(test_context.rejected_r5 == UINT32_C(0xdeadbeef));
        CHECK(test_context.rejected_r5_valid == FALSE);
    }
}

static void test_all_other_command_statuses_are_ignored(void)
{
    static const enum P4HostCommandStatus statuses[] = {
        P4HOST_COMMAND_OK,
        P4HOST_COMMAND_REFUSED,
        P4HOST_COMMAND_TIMEOUT,
        P4HOST_COMMAND_ERROR,
        P4HOST_COMMAND_DMA_ERROR
    };
    unsigned index;
    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]); ++index)
    {
        begin_case();
        test_outcome.status = statuses[index];
        check_refused();
    }
}

static void test_wrong_request_fields_are_ignored(void)
{
    begin_case();
    test_request.slot = 0U;
    check_refused();

    begin_case();
    test_request.slot = 2U;
    check_refused();

    begin_case();
    test_request.index = 5U;
    check_refused();

    begin_case();
    test_request.response_flags = MMC_RSP_R4;
    check_refused();

    begin_case();
    test_request.data = &test_outcome;
    check_refused();

    begin_case();
    test_request.length = 4U;
    check_refused();

    begin_case();
    test_request.writing = TRUE;
    check_refused();
}

static void test_unstarted_or_incomplete_results_are_ignored(void)
{
    begin_case();
    test_outcome.started = FALSE;
    check_refused();

    begin_case();
    test_outcome.raw = 0U;
    check_refused();
}

static void test_each_command_error_bit_is_ignored(void)
{
    static const ULONG error_bits[] = {
        P4SD_INT_RESP_ERR,
        P4SD_INT_RCRC,
        P4SD_INT_RTO,
        P4SD_INT_HLE
    };
    unsigned index;
    for (index = 0U; index < sizeof(error_bits) / sizeof(error_bits[0]); ++index)
    {
        begin_case();
        CHECK((P4SD_INT_CMD_ERRORS & error_bits[index]) != 0U);
        test_outcome.raw = P4SD_INT_CMD_DONE | error_bits[index];
        check_refused();
    }

    begin_case();
    test_outcome.raw = P4SD_INT_CMD_DONE | P4SD_INT_CMD_ERRORS;
    check_refused();
}

int main(void)
{
    test_qualified_rejected_r5_is_published();
    test_other_observation_stages_are_ignored();
    test_all_other_command_statuses_are_ignored();
    test_wrong_request_fields_are_ignored();
    test_unstarted_or_incomplete_results_are_ignored();
    test_each_command_error_bit_is_ignored();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def assemble() -> tuple[str, str, str]:
    runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
    utilities = runpy.run_path(str(ADAPTER_SOURCE))
    extract_function = utilities["extract_function"]
    extract_struct = utilities["extract_struct"]
    context = extract_struct(runtime, "wifi_probe_context")
    observer = extract_function(runtime, "wifi_observe_rejected_r5")
    if FIXTURE.count(INSERT_CONTEXT) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_CONTEXT}")
    if FIXTURE.count(INSERT_OBSERVER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_OBSERVER}")
    source = FIXTURE.replace(INSERT_CONTEXT, context).replace(
        INSERT_OBSERVER, observer
    )
    return source, context, observer


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, context, observer = assemble()
    print("Extracted production source (SHA-256):")
    print(f"  runtime_probe.c: {sha256(RUNTIME_SOURCE.read_bytes())}")
    print(f"  wifi_probe_context: {sha256(context.encode('utf-8'))}")
    print(f"  wifi_observe_rejected_r5: {sha256(observer.encode('utf-8'))}")
    print("Coverage: actual context and observer; mocked read-only RESP0 access.")

    with tempfile.TemporaryDirectory(prefix="p4-wifi-rejected-r5-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_wifi_rejected_r5.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_wifi_rejected_r5_O{optimization}"
            command = compiler + [
                "-std=c99",
                f"-O{optimization}",
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Wconversion",
                "-Wshadow",
                "-Wstrict-prototypes",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I",
                str(ROOT / "compiler" / "include"),
                "-I",
                str(PORT / "sdmmc"),
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict C99 warnings")
            try:
                build = subprocess.run(
                    command,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=60,
                )
            except subprocess.TimeoutExpired:
                print("FAIL: compiler exceeded 60 seconds", file=sys.stderr)
                return 1
            sys.stdout.write(build.stdout)
            sys.stderr.write(build.stderr)
            if build.returncode != 0:
                return build.returncode or 1

            print(f"Running O{optimization} (30-second timeout)")
            try:
                result = subprocess.run(
                    [str(binary)],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=30,
                    env=environment,
                )
            except subprocess.TimeoutExpired:
                print("FAIL: test binary exceeded 30 seconds", file=sys.stderr)
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: test binary did not report its counts", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
