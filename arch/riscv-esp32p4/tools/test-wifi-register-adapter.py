#!/usr/bin/env python3
"""Compile strict extracted-source tests for the native Wi-Fi CMD52 adapter."""

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
COMMAND_SOURCE = PORT / "network" / "sdio_commands.c"
COMMAND_HEADER = PORT / "sdmmc" / "command.h"
REGISTER_HEADER = PORT / "sdmmc" / "registers.h"
INSERT_MARKER = "/* INSERT_ACTUAL_WIFI_ADAPTER */"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def code_char(source: str, start: int, wanted: str) -> int:
    """Find a C token outside comments, strings and character literals."""
    state = "code"
    escaped = False
    index = start
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == wanted:
                return index
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise ValueError(f"could not find C token {wanted!r}")


def braced_end(source: str, opening: int) -> int:
    state = "code"
    escaped = False
    depth = 0
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index + 1
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise ValueError("unterminated C braced definition")


def extract_function(source: str, name: str) -> str:
    signature = re.compile(
        rf"(?m)^static\s+(?:int|void)\s+{re.escape(name)}\s*\("
    )
    matches = list(signature.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"expected one static {name} definition, found {len(matches)}")
    opening = code_char(source, matches[0].end(), "{")
    return source[matches[0].start() : braced_end(source, opening)]


def extract_struct(source: str, name: str) -> str:
    signature = re.compile(rf"\bstruct\s+{re.escape(name)}\s*\{{")
    matches = list(signature.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"expected one struct {name}, found {len(matches)}")
    opening = code_char(source, matches[0].end() - 1, "{")
    end = braced_end(source, opening)
    semicolon = code_char(source, end, ";")
    return source[matches[0].start() : semicolon + 1]


def assemble() -> tuple[str, dict[str, str]]:
    runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
    extractors = runpy.run_path(str(PORT / "tools" / "test-sdmmc-host.py"))
    extract_definition = extractors["extract_definition"]
    extract_constant_lines = extractors["extract_constant_lines"]
    register_constants = extract_constant_lines(
        REGISTER_HEADER,
        (
            "P4SD_INT_CMD_DATA_MASK",
            "P4SD_INT_CMD_DONE",
            "P4SD_INT_RCRC",
            "P4SD_INT_RTO",
        ),
    )
    command_fault = extract_definition(
        COMMAND_HEADER,
        "P4HostCommandFault",
        r"(?m)^enum\s+P4HostCommandFault\b",
    ) + ";"
    fixture = FIXTURE
    pieces = {
        "wifi_probe_context": extract_struct(runtime, "wifi_probe_context"),
        "register_constants": register_constants,
        "P4HostCommandFault": command_fault,
        "wifi_budget": extract_function(runtime, "wifi_budget"),
        "wifi_command_failure": extract_function(runtime, "wifi_command_failure"),
        "wifi_command": extract_function(runtime, "wifi_command"),
        "wifi_register_read": extract_function(runtime, "wifi_register_read"),
        "wifi_register_write": extract_function(runtime, "wifi_register_write"),
    }
    if fixture.count(INSERT_MARKER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_MARKER}")
    definitions = "\n\n".join(pieces.values())
    return fixture.replace(INSERT_MARKER, definitions), pieces


FIXTURE = r'''/* Generated in a temporary directory by test-wifi-register-adapter.py. */
#include <hardware/mmc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdio_commands.h"
#include "sdio_probe.h"

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define P4_BOARD_WIFI_SLOT 1U

enum P4HostCommandStatus
{
    P4HOST_COMMAND_OK,
    P4HOST_COMMAND_REFUSED,
    P4HOST_COMMAND_TIMEOUT,
    P4HOST_COMMAND_ERROR,
    P4HOST_COMMAND_CARD_ERROR,
    P4HOST_COMMAND_DMA_ERROR
};

struct P4HostCommandResult;

struct P4HostCommand
{
    unsigned slot;
    ULONG generation, index, argument, response_flags, response_error_mask;
    UBYTE *data;
    ULONG length, block_size;
    BOOL writing, early_cmd18;
    int diagnostic_fault;
    void (*observe)(void *, const struct P4HostCommand *, int,
                    const struct P4HostCommandResult *);
    void *observe_context;
};

struct P4HostCommandResult
{
    enum P4HostCommandStatus status;
    BOOL started;
    ULONG raw, idstatus;
    ULONG response[4];
    int injected_fault;
};

struct P4SDMMCHost;
typedef BOOL (*host_command_fn)(struct P4SDMMCHost *,
    const struct P4HostCommand *, struct P4HostCommandResult *);
typedef BOOL (*host_reset_fn)(struct P4SDMMCHost *);

struct P4SDMMCHost
{
    ULONG generation;
    host_command_fn command;
    host_reset_fn reset;
    BOOL budget_open;
    unsigned budget_checks;
    unsigned budget_fail_on_check;
    unsigned reset_calls;
    BOOL dma_faulted;
};

static unsigned checks;
static unsigned cases;
static unsigned zero_calls;
static unsigned command_calls;
static BOOL command_return;
static ULONG command_response;
static BOOL change_generation_in_command;
static BOOL close_budget_in_command;
static struct P4HostCommand captured_request;
static struct P4HostCommandResult command_result;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void p4host_zero(void *destination, size_t length)
{
    unsigned char *bytes = (unsigned char *)destination;
    size_t index;
    ++zero_calls;
    for (index = 0; index < length; ++index)
        bytes[index] = 0;
}

static BOOL p4host_budget_ok(struct P4SDMMCHost *host)
{
    ++host->budget_checks;
    return host->budget_open &&
        (!host->budget_fail_on_check ||
         host->budget_checks != host->budget_fail_on_check);
}

static BOOL mock_reset(struct P4SDMMCHost *host)
{
    ++host->reset_calls;
    return TRUE;
}

static BOOL mock_command(struct P4SDMMCHost *host,
                         const struct P4HostCommand *request,
                         struct P4HostCommandResult *result)
{
    ++command_calls;
    captured_request = *request;
    *result = command_result;
    result->response[0] = command_response;
    if (change_generation_in_command)
        ++host->generation;
    if (close_budget_in_command)
        host->budget_open = FALSE;
    return command_return;
}

/* INSERT_ACTUAL_WIFI_ADAPTER */

static struct P4SDMMCHost host;
static struct wifi_probe_context context;

static void reset_case(void)
{
    memset(&host, 0, sizeof(host));
    memset(&context, 0, sizeof(context));
    memset(&captured_request, 0xa5, sizeof(captured_request));
    memset(&command_result, 0, sizeof(command_result));
    host.generation = UINT32_C(0x5a17c0de);
    host.command = mock_command;
    host.reset = mock_reset;
    host.budget_open = TRUE;
    context.host = &host;
    context.generation = host.generation;
    command_return = TRUE;
    command_response = UINT32_C(0x000000a7);
    change_generation_in_command = FALSE;
    close_budget_in_command = FALSE;
    zero_calls = 0;
    command_calls = 0;
}

static void begin_case(void)
{
    ++cases;
    reset_case();
}

static void check_request(unsigned index, ULONG argument, ULONG response_flags,
                          ULONG error_mask)
{
    CHECK(captured_request.slot == P4_BOARD_WIFI_SLOT);
    CHECK(captured_request.generation == context.generation);
    CHECK(captured_request.index == index);
    CHECK(captured_request.argument == argument);
    CHECK(captured_request.response_flags == response_flags);
    CHECK(captured_request.response_error_mask == error_mask);
    CHECK(captured_request.data == NULL);
    CHECK(captured_request.length == 0);
    CHECK(captured_request.block_size == 0);
    CHECK(captured_request.writing == FALSE);
    CHECK(captured_request.early_cmd18 == FALSE);
    CHECK(captured_request.diagnostic_fault == 0);
    CHECK(captured_request.observe == NULL);
    CHECK(captured_request.observe_context == NULL);
}

static void test_read_request_and_data(void)
{
    uint8_t value = UINT8_C(0x11);
    uint32_t expected_argument = 0;
    begin_case();
    CHECK(p4_sdio_cmd52(0, 3, 0, UINT32_C(0x1234), 0,
                        &expected_argument));
    CHECK(wifi_register_read(&context, 3, UINT32_C(0x1234), &value));
    CHECK(value == UINT8_C(0xa7));
    CHECK(command_calls == 1);
    CHECK(host.budget_checks == 2);
    CHECK(zero_calls == 1);
    check_request(52, expected_argument, MMC_RSP_R5,
                  P4_SDIO_PROBE_R5_ERROR_MASK);
    CHECK(host.reset_calls == 0);
}

static void test_write_uses_cmdarg_not_data_phase(void)
{
    uint32_t expected_argument = 0;
    begin_case();
    CHECK(p4_sdio_cmd52(1, 0, 0, UINT32_C(0x110), UINT8_C(0x02),
                        &expected_argument));
    CHECK((expected_argument & UINT32_C(0x80000000)) != 0);
    CHECK(wifi_register_write(&context, 0, UINT32_C(0x110), UINT8_C(0x02)));
    CHECK(command_calls == 1);
    CHECK(host.budget_checks == 2);
    check_request(52, expected_argument, MMC_RSP_R5,
                  P4_SDIO_PROBE_R5_ERROR_MASK);
    CHECK(host.reset_calls == 0);
}

static void test_response_flags_and_forwarded_command(void)
{
    static const p4_sdio_response_type_t types[] = {
        P4_SDIO_RESP_NONE, P4_SDIO_RESP_R4, P4_SDIO_RESP_R6,
        P4_SDIO_RESP_R1, P4_SDIO_RESP_R5
    };
    static const ULONG flags[] = {
        MMC_RSP_NONE, MMC_RSP_R4, MMC_RSP_R6, MMC_RSP_R1, MMC_RSP_R5
    };
    unsigned index;
    for (index = 0; index < sizeof(types) / sizeof(types[0]); ++index)
    {
        uint32_t word = 0;
        begin_case();
        CHECK(wifi_command(&context, 7, UINT32_C(0xdeadbeef), types[index],
                           UINT32_C(0x00abcdef), &word));
        CHECK(word == UINT32_C(0x000000a7));
        CHECK(command_calls == 1);
        CHECK(host.budget_checks == 2);
        CHECK(captured_request.index == 7);
        CHECK(captured_request.argument == UINT32_C(0xdeadbeef));
        CHECK(captured_request.response_flags == flags[index]);
        CHECK(captured_request.response_error_mask == UINT32_C(0x00abcdef));
        CHECK(captured_request.slot == P4_BOARD_WIFI_SLOT);
        CHECK(captured_request.generation == context.generation);
        CHECK(captured_request.data == NULL && captured_request.length == 0);
        CHECK(captured_request.writing == FALSE);
        CHECK(host.reset_calls == 0);
    }
}

static void test_failure_preserves_outputs(void)
{
    uint8_t value = UINT8_C(0x5c);
    uint32_t word = UINT32_C(0x76543210);
    begin_case();
    command_return = FALSE;
    CHECK(!wifi_register_read(&context, 0, 2, &value));
    CHECK(value == UINT8_C(0x5c));
    CHECK(command_calls == 1);
    CHECK(!wifi_command(&context, 52, 0, P4_SDIO_RESP_R5,
                        P4_SDIO_PROBE_R5_ERROR_MASK, &word));
    CHECK(word == UINT32_C(0x76543210));
    CHECK(command_calls == 2);
    CHECK(host.reset_calls == 0);
}

static void expect_classified_failure(enum P4HostCommandStatus status,
                                      BOOL started, ULONG raw,
                                      ULONG idstatus, int injected_fault,
                                      int expected)
{
    uint32_t word = UINT32_C(0x76543210);
    int returned;

    begin_case();
    command_return = FALSE;
    command_result.status = status;
    command_result.started = started;
    command_result.raw = raw;
    command_result.idstatus = idstatus;
    command_result.injected_fault = injected_fault;
    returned = wifi_command(&context, 52, UINT32_C(0x80000c08),
                            P4_SDIO_RESP_R5,
                            P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == expected);
    CHECK(word == UINT32_C(0x76543210));
    CHECK(command_calls == 1);
    CHECK(host.budget_checks == 2);
}

static void test_exact_reset_failure_classification(void)
{
    expect_classified_failure(P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0, 0,
        P4_SDIO_COMMAND_RESPONSE_TIMEOUT);
    expect_classified_failure(P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RCRC, 0, 0,
        P4_SDIO_COMMAND_RESPONSE_CRC);
    expect_classified_failure(P4HOST_COMMAND_TIMEOUT, TRUE, 0, 0, 0,
        P4_SDIO_COMMAND_RESPONSE_TIMEOUT);
    expect_classified_failure(P4HOST_COMMAND_TIMEOUT, FALSE, 0, 0, 0, 0);
    expect_classified_failure(P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 1, 0, 0);
    expect_classified_failure(P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0, 1, 0);
}

static void test_dma_fault_rejects_completed_command(void)
{
    uint8_t value = UINT8_C(0x5c);

    begin_case();
    host.dma_faulted = TRUE;
    CHECK(!wifi_register_read(&context, 0, 2, &value));
    CHECK(value == UINT8_C(0x5c));
    CHECK(command_calls == 1);
    CHECK(host.budget_checks == 2);
}

static void test_budget_pre_and_post_failures_preserve_outputs(void)
{
    uint8_t value = UINT8_C(0x39);
    uint32_t word = UINT32_C(0x89abcdef);
    begin_case();
    host.budget_open = FALSE;
    CHECK(!wifi_register_read(&context, 0, 2, &value));
    CHECK(value == UINT8_C(0x39));
    CHECK(command_calls == 0);
    CHECK(zero_calls == 0);

    begin_case();
    host.budget_fail_on_check = 2;
    CHECK(!wifi_register_read(&context, 0, 2, &value));
    CHECK(value == UINT8_C(0x39));
    CHECK(command_calls == 1);
    CHECK(host.budget_checks == 2);

    begin_case();
    host.budget_open = FALSE;
    CHECK(!wifi_command(&context, 52, 0, P4_SDIO_RESP_R5,
                        P4_SDIO_PROBE_R5_ERROR_MASK, &word));
    CHECK(word == UINT32_C(0x89abcdef));
    CHECK(command_calls == 0);
}

static void test_generation_change_rejects_result(void)
{
    uint32_t word = UINT32_C(0x10203040);
    begin_case();
    change_generation_in_command = TRUE;
    CHECK(!wifi_command(&context, 52, 0, P4_SDIO_RESP_R5,
                        P4_SDIO_PROBE_R5_ERROR_MASK, &word));
    CHECK(word == UINT32_C(0x10203040));
    CHECK(command_calls == 1);
    CHECK(host.reset_calls == 0);
}

static void test_r5_error_bits_are_rejected(void)
{
    static const ULONG bits[] = {
        UINT32_C(1) << 15, UINT32_C(1) << 14, UINT32_C(1) << 11,
        UINT32_C(1) << 9, UINT32_C(1) << 8
    };
    unsigned index;
    for (index = 0; index < sizeof(bits) / sizeof(bits[0]); ++index)
    {
        uint8_t value = UINT8_C(0x44);
        begin_case();
        command_response = bits[index] | UINT32_C(0x000000a7);
        CHECK(!wifi_register_read(&context, 1, UINT32_C(0x111), &value));
        CHECK(value == UINT8_C(0x44));
        CHECK(command_calls == 1);
        CHECK(captured_request.response_error_mask ==
              P4_SDIO_PROBE_R5_ERROR_MASK);
    }

    begin_case();
    {
        uint8_t value = 0;
        command_response = (UINT32_C(1) << 10) | UINT32_C(0x000000a7);
        CHECK(wifi_register_read(&context, 1, UINT32_C(0x111), &value));
        CHECK(value == UINT8_C(0xa7));
    }
}

static void test_write_r5_error_and_bad_arguments(void)
{
    uint8_t value = UINT8_C(0x73);
    begin_case();
    command_response = UINT32_C(0x00000800);
    CHECK(!wifi_register_write(&context, 1, UINT32_C(0x111), UINT8_C(0x22)));
    CHECK(command_calls == 1);

    begin_case();
    CHECK(!wifi_register_read(&context, 8, 0, &value));
    CHECK(!wifi_register_read(&context, 0, UINT32_C(0x20000), &value));
    CHECK(!wifi_register_read(&context, 0, 0, NULL));
    CHECK(!wifi_register_write(&context, 8, 0, UINT8_C(0x22)));
    CHECK(!wifi_register_write(&context, 0, UINT32_C(0x20000), UINT8_C(0x22)));
    CHECK(command_calls == 0);
    CHECK(zero_calls == 0);
}

static void test_invalid_response_type_preserves_word(void)
{
    uint32_t word = UINT32_C(0xcafef00d);
    begin_case();
    CHECK(!wifi_command(&context, 52, 0,
                        (p4_sdio_response_type_t)-1, 0, &word));
    CHECK(word == UINT32_C(0xcafef00d));
    CHECK(!wifi_command(&context, 52, 0,
                        (p4_sdio_response_type_t)(P4_SDIO_RESP_R5 + 1), 0,
                        &word));
    CHECK(word == UINT32_C(0xcafef00d));
    CHECK(command_calls == 0);
    CHECK(zero_calls == 0);
}

int main(void)
{
    test_read_request_and_data();
    test_write_uses_cmdarg_not_data_phase();
    test_response_flags_and_forwarded_command();
    test_failure_preserves_outputs();
    test_exact_reset_failure_classification();
    test_dma_fault_rejects_completed_command();
    test_budget_pre_and_post_failures_preserve_outputs();
    test_generation_change_rejects_result();
    test_r5_error_bits_are_rejected();
    test_write_r5_error_and_bad_arguments();
    test_invalid_response_type_preserves_word();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, extracted = assemble()
    print("Extracted production source (SHA-256):")
    print(f"  runtime_probe.c: {sha256(RUNTIME_SOURCE.read_bytes())}")
    for name, body in extracted.items():
        print(f"  {name}: {sha256(body.encode('utf-8'))}")
    print(f"  sdio_commands.c: {sha256(COMMAND_SOURCE.read_bytes())}")
    print("Coverage: actual wifi_budget/wifi_command/wifi_command_failure/CMD52 wrappers; extracted register constants/fault enum; actual CMD52 argument builder.")

    with tempfile.TemporaryDirectory(prefix="p4-wifi-register-adapter-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_wifi_register_adapter.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_wifi_register_adapter_O{optimization}"
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
                str(PORT / "network"),
                str(source_path),
                str(COMMAND_SOURCE),
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
            if not re.search(r"(?m)^PASS: 28 cases, \d+ checks$", result.stdout):
                print("FAIL: test binary did not report its complete case count", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
