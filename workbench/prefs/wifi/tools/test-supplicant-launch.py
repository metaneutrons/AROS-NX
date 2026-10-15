#!/usr/bin/env python3
"""Exercise the production WirelessManager launcher with host-side DOS/Exec fakes."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path


WIFI = Path(__file__).resolve().parents[1]
WORKER = WIFI / "worker.c"
DEVICE = "FLASHDISK0P0:Devs/Networks/esp32p4wifi.device"
MANAGER = "FLASHDISK0P0:C/WirelessManager"


PRELUDE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef const char *CONST_STRPTR;
typedef char *STRPTR;
typedef char TEXT;
typedef int32_t LONG;
typedef uintptr_t IPTR;
typedef uintptr_t BPTR;
typedef int BOOL;
typedef uint32_t ULONG;

struct Task { int marker; };
struct TagItem { ULONG ti_Tag; IPTR ti_Data; };

#define TRUE 1
#define FALSE 0
#define BNULL ((BPTR)0)
#define TAG_DONE 0UL
#define SYS_Input 10UL
#define SYS_Output 11UL
#define SYS_Error 12UL
#define SYS_Asynch 13UL
#define NP_StackSize 14UL

BPTR LoadSeg(CONST_STRPTR name);
void UnLoadSeg(BPTR segment);
LONG IoErr(void);
LONG SystemTagList(CONST_STRPTR command, struct TagItem *tags);
struct Task *FindTask(CONST_STRPTR name);
void Delay(ULONG ticks);
'''


HARNESS = r'''
static unsigned checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static BPTR load_result;
static LONG io_error;
static LONG system_result;
static unsigned load_calls;
static unsigned unload_calls;
static unsigned ioerr_calls;
static unsigned system_calls;
static unsigned find_calls;
static unsigned delay_calls;
static unsigned task_available_on_call;
static char loaded_path[96];
static char find_name[96];
static char system_command[256];
static ULONG observed_tags[8];
static IPTR observed_values[8];
static size_t observed_tag_count;
static BPTR unloaded_segment;
static struct Task running_task = { 1 };
static const char secret[] = "known-wifi-passphrase-sentinel";
static const char expected_device[] = "FLASHDISK0P0:Devs/Networks/esp32p4wifi.device";
static const char expected_manager[] = "FLASHDISK0P0:C/WirelessManager";

static void copy_text(char *destination, size_t capacity, CONST_STRPTR source)
{
    size_t length = strlen(source);
    CHECK(length < capacity);
    memcpy(destination, source, length + 1u);
}

BPTR LoadSeg(CONST_STRPTR name)
{
    ++load_calls;
    copy_text(loaded_path, sizeof(loaded_path), name);
    return load_result;
}

void UnLoadSeg(BPTR segment)
{
    ++unload_calls;
    unloaded_segment = segment;
}

LONG IoErr(void)
{
    ++ioerr_calls;
    return io_error;
}

LONG SystemTagList(CONST_STRPTR command, struct TagItem *tags)
{
    size_t index = 0u;

    ++system_calls;
    copy_text(system_command, sizeof(system_command), command);
    while (tags[index].ti_Tag != TAG_DONE) {
        CHECK(index + 1u < sizeof(observed_tags) / sizeof(observed_tags[0]));
        observed_tags[index] = tags[index].ti_Tag;
        observed_values[index] = tags[index].ti_Data;
        ++index;
    }
    observed_tags[index] = TAG_DONE;
    observed_values[index] = tags[index].ti_Data;
    observed_tag_count = index + 1u;
    return system_result;
}

struct Task *FindTask(CONST_STRPTR name)
{
    ++find_calls;
    copy_text(find_name, sizeof(find_name), name);
    if (task_available_on_call != 0u && find_calls >= task_available_on_call)
        return &running_task;
    return NULL;
}

void Delay(ULONG ticks)
{
    CHECK(ticks == 25u);
    ++delay_calls;
}

static void reset_mocks(void)
{
    load_result = (BPTR)0x123u;
    io_error = 0;
    system_result = 0;
    load_calls = 0u;
    unload_calls = 0u;
    ioerr_calls = 0u;
    system_calls = 0u;
    find_calls = 0u;
    delay_calls = 0u;
    task_available_on_call = 1u;
    loaded_path[0] = '\0';
    find_name[0] = '\0';
    system_command[0] = '\0';
    memset(observed_tags, 0, sizeof(observed_tags));
    memset(observed_values, 0, sizeof(observed_values));
    observed_tag_count = 0u;
    unloaded_segment = BNULL;
}

static void check_command_and_tags(LONG unit)
{
    char expected[256];
    int length = snprintf(expected, sizeof(expected),
        "%s \"%s\" UNIT %ld NOGUI\n", expected_manager,
        expected_device, (long)unit);

    CHECK(length > 0 && (size_t)length < sizeof(expected));
    CHECK(strcmp(system_command, expected) == 0);
    CHECK(strstr(system_command, secret) == NULL);
    CHECK(strcmp(loaded_path, expected_manager) == 0);
    CHECK(observed_tag_count == 6u);
    CHECK(observed_tags[0] == SYS_Input && observed_values[0] == 0u);
    CHECK(observed_tags[1] == SYS_Output && observed_values[1] == 0u);
    CHECK(observed_tags[2] == SYS_Error && observed_values[2] == 0u);
    CHECK(observed_tags[3] == SYS_Asynch && observed_values[3] == (IPTR)TRUE);
    CHECK(observed_tags[4] == NP_StackSize && observed_values[4] == 65536u);
    CHECK(observed_tags[5] == TAG_DONE && observed_values[5] == 0u);
}

static void check_no_secret(const char *status)
{
    CHECK(strstr(system_command, secret) == NULL);
    CHECK(strstr(status, secret) == NULL);
}

static void test_loadseg_failure_reports_numeric_dos_error(void)
{
    char status[128] = "unchanged";

    reset_mocks();
    load_result = BNULL;
    io_error = 47;
    CHECK(!StartSupplicant(expected_device, 2, status, sizeof(status)));
    CHECK(strcmp(status, "WirelessManager: LoadSeg failed (47)") == 0);
    CHECK(load_calls == 1u && strcmp(loaded_path, expected_manager) == 0);
    CHECK(ioerr_calls == 1u);
    CHECK(unload_calls == 0u && system_calls == 0u);
    CHECK(find_calls == 0u && delay_calls == 0u);
    check_no_secret(status);
}

static void test_shell_failure_reports_numeric_dos_error(void)
{
    char status[128] = "unchanged";

    reset_mocks();
    system_result = -1;
    io_error = 205;
    CHECK(!StartSupplicant(expected_device, 3, status, sizeof(status)));
    CHECK(strcmp(status, "WirelessManager: shell start failed (205)") == 0);
    CHECK(load_calls == 1u && unload_calls == 1u);
    CHECK(unloaded_segment == load_result);
    CHECK(ioerr_calls == 1u && system_calls == 1u);
    CHECK(find_calls == 0u && delay_calls == 0u);
    check_command_and_tags(3);
    check_no_secret(status);
}

static void test_running_task_succeeds(void)
{
    char status[128] = "unchanged";

    reset_mocks();
    task_available_on_call = 3u;
    CHECK(StartSupplicant(expected_device, 1, status, sizeof(status)));
    CHECK(strcmp(status, "unchanged") == 0);
    CHECK(load_calls == 1u && unload_calls == 1u && system_calls == 1u);
    CHECK(find_calls == 3u && delay_calls == 2u);
    CHECK(strcmp(find_name, expected_manager) == 0);
    check_command_and_tags(1);
    check_no_secret(status);
}

static void test_missing_task_reports_after_twenty_polls(void)
{
    char status[128] = "unchanged";

    reset_mocks();
    task_available_on_call = 0u;
    CHECK(!StartSupplicant(expected_device, 0, status, sizeof(status)));
    CHECK(strcmp(status,
        "WirelessManager loaded; no running task after init") == 0);
    CHECK(load_calls == 1u && unload_calls == 1u && system_calls == 1u);
    CHECK(find_calls == 20u && delay_calls == 20u);
    CHECK(strcmp(find_name, expected_manager) == 0);
    check_command_and_tags(0);
    check_no_secret(status);
}

int main(void)
{
    CHECK(strcmp(WIFI_MANAGER, expected_manager) == 0);
    test_loadseg_failure_reports_numeric_dos_error();
    test_shell_failure_reports_numeric_dos_error();
    test_running_task_succeeds();
    test_missing_task_reports_after_twenty_polls();
    printf("PASS supplicant-launch checks=%u\n", checks);
    return 0;
}
'''


def function_definition(source: str, name: str) -> str:
    match = re.search(rf"^static BOOL {re.escape(name)}\s*\(", source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"could not find production function {name}")
    opening = source.find("{", match.end())
    if opening < 0:
        raise AssertionError(f"could not find body for {name}")
    depth = 1
    position = opening + 1
    while depth and position < len(source):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
        position += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return source[match.start() : position]


def manager_macros(source: str) -> str:
    match = re.search(
        r"^#ifdef WIFI_P4_FLASH_HELPERS\s*$.*?^#endif\s*$",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if match is None or "#define WIFI_MANAGER" not in match.group(0):
        raise AssertionError("could not find production WIFI_MANAGER path selection")
    return "#define WIFI_P4_FLASH_HELPERS 1\n" + match.group(0) + "\n"


def main() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    source = WORKER.read_text(encoding="utf-8")
    manager = manager_macros(source)
    launcher = function_definition(source, "StartSupplicant")
    harness = PRELUDE + manager + launcher + HARNESS

    with tempfile.TemporaryDirectory(prefix="wifi-supplicant-launch-") as temp:
        temp_dir = Path(temp)
        source_file = temp_dir / "test-supplicant-launch.c"
        source_file.write_text(harness, encoding="utf-8")

        for optimization in ("-O0", "-O2"):
            executable = temp_dir / f"test-supplicant-launch-{optimization[2:]}"
            command = compiler + [
                "-std=c99",
                optimization,
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_file),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True)
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            result = subprocess.run(
                [str(executable)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                env=environment,
            )
            print(f"{optimization} ASan/UBSan: {result.stdout.strip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
