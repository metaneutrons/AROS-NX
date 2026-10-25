#!/usr/bin/env python3
"""Run sanitized host regressions against the production Wi-Fi access helpers."""

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


ROOT = Path(__file__).resolve().parents[4]
WIFI = Path(__file__).resolve().parents[1]
EXTRACTOR = ROOT / "arch" / "riscv-esp32p4" / "tools" / "test-sdmmc-host.py"
WORKER = WIFI / "worker.c"
DEVICE_PATH = "PRIVATE-DEVICE-PATH"
PASSWORD = "SECRET_PASSWORD_TOKEN"


FIXTURE_PREFIX = r'''#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int8_t BYTE;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef const char *CONST_STRPTR;
typedef char *STRPTR;
typedef char TEXT;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define NSCMD_DEVICEQUERY 0x4000u
#define S2_GETNETWORKS 0xc011u
#define WIFI_DEV_MAX 64
#define MSG_ST_CANNOT_OPEN "Cannot open %s unit %ld"
#define _(text) (text)
#define Stricmp strcasecmp

struct MsgPort { unsigned marker; };
struct Message {
    struct MsgPort *mn_ReplyPort;
    UWORD mn_Length;
};
struct Library {
    UBYTE lib_Version;
    UBYTE lib_Revision;
};
struct Device { struct Library dd_Library; };
struct Unit { unsigned marker; };
struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};
struct IOStdReq {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
    ULONG io_Actual;
    ULONG io_Length;
    APTR io_Data;
    ULONG io_Offset;
};
struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;
    ULONG ios2_PacketType;
    UBYTE ios2_SrcAddr[16];
    UBYTE ios2_DstAddr[16];
    ULONG ios2_DataLength;
    APTR ios2_Data;
    APTR ios2_StatData;
    APTR ios2_BufferManagement;
};
struct NSDeviceQueryResult {
    ULONG DevQueryFormat;
    ULONG SizeAvailable;
    UWORD DeviceType;
    UWORD DeviceSubType;
    UWORD *SupportedCommands;
};
struct WifiMsg {
    TEXT wm_Status[96];
    TEXT wm_Device[64];
    LONG wm_Unit;
    TEXT wm_Key[64];
};

static unsigned checks;
static unsigned cases;

static void check(int ok, int line)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL line %d\n", line);
        exit(1);
    }
}
#define CHECK(expression) check((expression), __LINE__)

static unsigned create_port_calls;
static unsigned created_ports;
static unsigned deleted_ports;
static unsigned live_ports;
static unsigned alloc_calls;
static unsigned allocated_requests;
static unsigned freed_requests;
static unsigned live_requests;
static unsigned close_calls;
static unsigned open_calls;
static unsigned doio_calls;
static unsigned fail_port_create;
static unsigned fail_request_alloc;
static LONG open_result;
static LONG doio_result;
static ULONG resident_version;
static ULONG resident_revision;
static ULONG last_open_unit;
static ULONG last_open_flags;
static ULONG close_seen_version;
static ULONG close_seen_revision;
static UWORD *query_commands;
static char observer_log[4096];
static size_t observer_used;
static struct Device mock_device;

static struct MsgPort *CreateMsgPort(void);
static void DeleteMsgPort(struct MsgPort *port);
static APTR AllocVec(ULONG size, ULONG flags);
static void FreeVec(APTR memory);
static void CloseDevice(struct IORequest *request);
static LONG OpenDevice(CONST_STRPTR name, ULONG unit,
    struct IORequest *request, ULONG flags);
static LONG DoIO(struct IORequest *request);
static void bug(const char *format, ...);

static const unsigned char buffer_tags[1] = { 0 };

static struct MsgPort *CreateMsgPort(void)
{
    struct MsgPort *port;
    ++create_port_calls;
    if (fail_port_create)
        return NULL;
    port = (struct MsgPort *)calloc(1, sizeof(*port));
    CHECK(port != NULL);
    ++created_ports;
    ++live_ports;
    return port;
}

static void DeleteMsgPort(struct MsgPort *port)
{
    CHECK(port != NULL);
    CHECK(live_ports != 0);
    ++deleted_ports;
    --live_ports;
    free(port);
}

static APTR AllocVec(ULONG size, ULONG flags)
{
    void *memory;
    ++alloc_calls;
    if (fail_request_alloc)
        return NULL;
    CHECK((flags & MEMF_CLEAR) != 0);
    memory = calloc(1, (size_t)size);
    CHECK(memory != NULL);
    ++allocated_requests;
    ++live_requests;
    return memory;
}

static void FreeVec(APTR memory)
{
    if (memory == NULL)
        return;
    CHECK(live_requests != 0);
    ++freed_requests;
    --live_requests;
    free(memory);
}

static void CloseDevice(struct IORequest *request)
{
    CHECK(request != NULL);
    CHECK(request->io_Device == &mock_device);
    ++close_calls;
    close_seen_version = request->io_Device->dd_Library.lib_Version;
    close_seen_revision = request->io_Device->dd_Library.lib_Revision;
}

static LONG OpenDevice(CONST_STRPTR name, ULONG unit,
    struct IORequest *request, ULONG flags)
{
    struct IOStdReq *standard = (struct IOStdReq *)request;
    ++open_calls;
    CHECK(strcmp(name, "PRIVATE-DEVICE-PATH") == 0);
    CHECK(unit == 7);
    CHECK(flags == 0);
    CHECK(request->io_Message.mn_ReplyPort != NULL);
    CHECK(request->io_Message.mn_Length == sizeof(struct IOSana2Req));
    CHECK(standard->io_Device == NULL);
    last_open_unit = unit;
    last_open_flags = flags;
    mock_device.dd_Library.lib_Version = (UBYTE)resident_version;
    mock_device.dd_Library.lib_Revision = (UBYTE)resident_revision;
    request->io_Device = &mock_device;
    return open_result;
}

static LONG DoIO(struct IORequest *request)
{
    struct IOStdReq *standard = (struct IOStdReq *)request;
    struct NSDeviceQueryResult *query;
    ++doio_calls;
    CHECK(request != NULL);
    CHECK(request->io_Device == &mock_device);
    CHECK(standard->io_Command == NSCMD_DEVICEQUERY);
    CHECK(standard->io_Length == sizeof(struct NSDeviceQueryResult));
    CHECK(standard->io_Data != NULL);
    query = (struct NSDeviceQueryResult *)standard->io_Data;
    if (doio_result == 0)
        query->SupportedCommands = query_commands;
    return doio_result;
}

static void bug(const char *format, ...)
{
    va_list args;
    int written;
    CHECK(observer_used < sizeof(observer_log));
    va_start(args, format);
    written = vsnprintf(observer_log + observer_used,
        sizeof(observer_log) - observer_used, format, args);
    va_end(args);
    CHECK(written >= 0);
    CHECK((size_t)written < sizeof(observer_log) - observer_used);
    observer_used += (size_t)written;
}

static void reset_case(void)
{
    CHECK(live_ports == 0);
    CHECK(live_requests == 0);
    create_port_calls = 0;
    created_ports = 0;
    deleted_ports = 0;
    live_ports = 0;
    alloc_calls = 0;
    allocated_requests = 0;
    freed_requests = 0;
    live_requests = 0;
    close_calls = 0;
    open_calls = 0;
    doio_calls = 0;
    fail_port_create = 0;
    fail_request_alloc = 0;
    open_result = 0;
    doio_result = 0;
    resident_version = 1;
    resident_revision = 3;
    last_open_unit = 0;
    last_open_flags = 0;
    close_seen_version = 0;
    close_seen_revision = 0;
    query_commands = NULL;
    mock_device.dd_Library.lib_Version = 0;
    mock_device.dd_Library.lib_Revision = 0;
}

static void check_balanced(void)
{
    CHECK(live_ports == 0);
    CHECK(live_requests == 0);
    CHECK(created_ports == deleted_ports);
    CHECK(allocated_requests == freed_requests);
}
'''


FIXTURE_TESTS = r'''
static void test_port_creation_failure(void)
{
    struct WifiDev dev;
    reset_case();
    fail_port_create = 1;
    CHECK(!OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(dev.wd_Failure == 1 && dev.wd_Error == 0);
    CHECK(dev.wd_Version == 0 && dev.wd_Revision == 0);
    CHECK(dev.wd_Port == NULL && dev.wd_Req == NULL && !dev.wd_Opened);
    CHECK(create_port_calls == 1 && created_ports == 0 && deleted_ports == 0);
    CHECK(alloc_calls == 0 && open_calls == 0 && close_calls == 0);
    check_balanced();
    ++cases;
}

static void test_request_allocation_failure(void)
{
    struct WifiDev dev;
    reset_case();
    fail_request_alloc = 1;
    CHECK(!OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(dev.wd_Failure == 2 && dev.wd_Error == 0);
    CHECK(dev.wd_Version == 0 && dev.wd_Revision == 0);
    CHECK(dev.wd_Port == NULL && dev.wd_Req == NULL && !dev.wd_Opened);
    CHECK(created_ports == 1 && deleted_ports == 1);
    CHECK(alloc_calls == 1 && allocated_requests == 0 && freed_requests == 0);
    CHECK(open_calls == 0 && close_calls == 0);
    check_balanced();
    ++cases;
}

static void test_open_device_error_preserved(void)
{
    struct WifiDev dev;
    reset_case();
    open_result = 41;
    CHECK(!OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(dev.wd_Failure == 3 && dev.wd_Error == 41);
    CHECK(dev.wd_Version == 0 && dev.wd_Revision == 0);
    CHECK(dev.wd_Port == NULL && dev.wd_Req == NULL && !dev.wd_Opened);
    CHECK(open_calls == 1 && last_open_unit == 7 && last_open_flags == 0);
    CHECK(close_calls == 0);
    CHECK(created_ports == 1 && deleted_ports == 1);
    CHECK(allocated_requests == 1 && freed_requests == 1);
    check_balanced();
    ++cases;
}

static void test_resident_generations(void)
{
    static const ULONG versions[] = { 1, 1, 1, 2 };
    static const ULONG revisions[] = { 3, 2, 4, 3 };
    size_t i;
    for (i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
        struct WifiDev dev;
        int expected_open;
        reset_case();
        resident_version = versions[i];
        resident_revision = revisions[i];
#ifdef WIFI_P4_FLASH_HELPERS
        expected_open = versions[i] == 1 && revisions[i] == 3;
#else
        expected_open = 1;
#endif
        CHECK((OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7) != FALSE) ==
            expected_open);
        if (expected_open) {
            CHECK(dev.wd_Opened);
            CHECK(dev.wd_Failure == 0 && dev.wd_Error == 0);
            CHECK(close_calls == 0);
            CHECK(dev.wd_Version == 0 && dev.wd_Revision == 0);
            CloseWifiDev(&dev);
        } else {
            CHECK(dev.wd_Failure == 4 && dev.wd_Error == 0);
            CHECK(dev.wd_Version == versions[i]);
            CHECK(dev.wd_Revision == revisions[i]);
            CHECK(dev.wd_Port == NULL && dev.wd_Req == NULL && !dev.wd_Opened);
            CHECK(close_calls == 1);
            CHECK(close_seen_version == versions[i]);
            CHECK(close_seen_revision == revisions[i]);
        }
        CHECK(open_calls == 1);
        CHECK(created_ports == 1 && deleted_ports == 1);
        CHECK(allocated_requests == 1 && freed_requests == 1);
        check_balanced();
        ++cases;
    }
}

static void test_wireless_query_success(void)
{
    static UWORD commands[] = { 0x0020, S2_GETNETWORKS, 0 };
    struct WifiDev dev;
    reset_case();
    query_commands = commands;
    CHECK(OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(ProbeWireless(&dev));
    CHECK(doio_calls == 1 && close_calls == 0);
    CHECK(dev.wd_Opened && dev.wd_Failure == 0 && dev.wd_Error == 0);
    CHECK(last_open_unit == 7);
    CloseWifiDev(&dev);
    CHECK(close_calls == 1);
    check_balanced();
    ++cases;
}

static void test_wireless_query_error(void)
{
    struct WifiDev dev;
    reset_case();
    doio_result = 73;
    CHECK(OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(!ProbeWireless(&dev));
    CHECK(dev.wd_Failure == 5 && dev.wd_Error == 73);
    CHECK(dev.wd_Opened && dev.wd_Req != NULL && dev.wd_Port != NULL);
    CHECK(doio_calls == 1 && close_calls == 0);
    CHECK(live_ports == 1 && live_requests == 1);
    CloseWifiDev(&dev);
    CHECK(close_calls == 1);
    check_balanced();
    ++cases;
}

static void test_device_wireless_cache(void)
{
    static UWORD wireless_commands[] = { S2_GETNETWORKS, 0 };
    struct WifiDev first;
    struct WifiDev cached_failure;
    struct WifiDev different_name;
    struct WifiDev cached_success;
    reset_case();
    doio_result = 73;
    CHECK(OpenWifiDev(&first, "PRIVATE-DEVICE-PATH", 7));
    CHECK(!DeviceIsWireless(&first, "Wifi0"));
    CHECK(first.wd_Failure == 5 && first.wd_Error == 73);
    CHECK(doio_calls == 1);
    CloseWifiDev(&first);

    CHECK(OpenWifiDev(&cached_failure, "PRIVATE-DEVICE-PATH", 7));
    CHECK(!DeviceIsWireless(&cached_failure, "wIFI0"));
    CHECK(cached_failure.wd_Failure == 5 && cached_failure.wd_Error == 73);
    CHECK(doio_calls == 1);
    CloseWifiDev(&cached_failure);

    doio_result = 0;
    query_commands = wireless_commands;
    CHECK(OpenWifiDev(&different_name, "PRIVATE-DEVICE-PATH", 7));
    CHECK(DeviceIsWireless(&different_name, "radio1"));
    CHECK(different_name.wd_Failure == 0 && different_name.wd_Error == 0);
    CHECK(doio_calls == 2);
    CloseWifiDev(&different_name);

    CHECK(OpenWifiDev(&cached_success, "PRIVATE-DEVICE-PATH", 7));
    CHECK(DeviceIsWireless(&cached_success, "RADIO1"));
    CHECK(cached_success.wd_Failure == 0 && cached_success.wd_Error == 0);
    CHECK(doio_calls == 2);
    CloseWifiDev(&cached_success);
    CHECK(close_calls == 4);
    CHECK(created_ports == 4 && deleted_ports == 4);
    CHECK(allocated_requests == 4 && freed_requests == 4);
    check_balanced();
    ++cases;
}

static void test_wireless_command_absent(void)
{
    static UWORD commands[] = { 0x0020, 0, S2_GETNETWORKS, 0 };
    struct WifiDev dev;
    reset_case();
    query_commands = commands;
    CHECK(OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(!ProbeWireless(&dev));
    CHECK(dev.wd_Failure == 6 && dev.wd_Error == 0);
    CHECK(dev.wd_Opened && doio_calls == 1 && close_calls == 0);
    CloseWifiDev(&dev);
    CHECK(close_calls == 1);
    check_balanced();
    ++cases;
}

static void test_wireless_command_list_missing(void)
{
    struct WifiDev dev;
    reset_case();
    query_commands = NULL;
    CHECK(OpenWifiDev(&dev, "PRIVATE-DEVICE-PATH", 7));
    CHECK(!ProbeWireless(&dev));
    CHECK(dev.wd_Failure == 6 && dev.wd_Error == 0);
    CHECK(dev.wd_Opened && doio_calls == 1 && close_calls == 0);
    CloseWifiDev(&dev);
    CHECK(close_calls == 1);
    check_balanced();
    ++cases;
}

static void test_bounded_status(void)
{
    struct WifiMsg m;
    struct WifiDev dev;
    char saved_device[sizeof(m.wm_Device)];
    memset(&m, 0, sizeof(m));
    memset(&dev, 0, sizeof(dev));
    snprintf(m.wm_Device, sizeof(m.wm_Device), "%s",
        "DEVS:Networks/esp32p4wifi.device");
    snprintf(m.wm_Key, sizeof(m.wm_Key), "%s", "SECRET_PASSWORD_TOKEN");
    m.wm_Unit = INT32_MIN;
    snprintf(saved_device, sizeof(saved_device), "%s", m.wm_Device);
    dev.wd_Failure = INT32_MIN;
    dev.wd_Error = INT32_MIN;
    dev.wd_Version = UINT32_MAX;
    dev.wd_Revision = UINT32_MAX;
    WifiAccessStatus(&m, &dev);
    CHECK(m.wm_Status[sizeof(m.wm_Status) - 1] == '\0');
    CHECK(strlen(m.wm_Status) < sizeof(m.wm_Status));
    CHECK(strcmp(m.wm_Device, saved_device) == 0);
    CHECK(strstr(m.wm_Status, "SECRET_PASSWORD_TOKEN") == NULL);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp(m.wm_Status,
        "WiFi access: stage=-2147483648 error=-2147483648 "
        "version=4294967295.4294967295") == 0);
    CHECK(strstr(m.wm_Status, "DEVS:Networks/esp32p4wifi.device") == NULL);
#else
    CHECK(strcmp(m.wm_Status,
        "Cannot open DEVS:Networks/esp32p4wifi.device unit -2147483648") == 0);
#endif
    ++cases;
}

static void test_numeric_observer_whitelist(void)
{
    static const char *expected[] = {
        "[WiFiAccess] s=1 e=0 v=0 r=0",
        "[WiFiAccess] s=2 e=0 v=0 r=0",
        "[WiFiAccess] s=3 e=41 v=0 r=0",
#ifdef WIFI_P4_FLASH_HELPERS
        "[WiFiAccess] s=4 e=0 v=1 r=2",
        "[WiFiAccess] s=4 e=0 v=1 r=4",
        "[WiFiAccess] s=4 e=0 v=2 r=3",
#endif
        "[WiFiAccess] s=5 e=73 v=0 r=0",
        "[WiFiAccess] s=5 e=73 v=0 r=0",
        "[WiFiAccess] s=6 e=0 v=0 r=0",
        "[WiFiAccess] s=6 e=0 v=0 r=0"
    };
    char *copy;
    char *line;
    size_t index = 0;
    CHECK(strstr(observer_log, "PRIVATE-DEVICE-PATH") == NULL);
    CHECK(strstr(observer_log, "SECRET_PASSWORD_TOKEN") == NULL);
    copy = (char *)malloc(observer_used + 1);
    CHECK(copy != NULL);
    memcpy(copy, observer_log, observer_used + 1);
    line = strtok(copy, "\n");
    while (line != NULL) {
        CHECK(index < sizeof(expected) / sizeof(expected[0]));
        CHECK(strcmp(line, expected[index]) == 0);
        ++index;
        line = strtok(NULL, "\n");
    }
    CHECK(index == sizeof(expected) / sizeof(expected[0]));
    free(copy);
    ++cases;
}

int main(void)
{
    test_port_creation_failure();
    test_request_allocation_failure();
    test_open_device_error_preserved();
    test_resident_generations();
    test_wireless_query_success();
    test_wireless_query_error();
    test_device_wireless_cache();
    test_wireless_command_absent();
    test_wireless_command_list_missing();
    test_bounded_status();
    test_numeric_observer_whitelist();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


def extract_sources() -> tuple[str, str]:
    extract_definition = runpy.run_path(str(EXTRACTOR))["extract_definition"]
    pieces = [
        extract_definition(WORKER, "WifiDev", r"(?m)^struct\s+WifiDev\b") + ";",
        extract_definition(
            WORKER, "CloseWifiDev", r"(?m)^static\s+void\s+CloseWifiDev\s*\("
        ),
        extract_definition(
            WORKER,
            "WifiAccessFailure",
            r"(?m)^static\s+BOOL\s+WifiAccessFailure\s*\(",
        ),
        extract_definition(
            WORKER,
            "WifiAccessStatus",
            r"(?m)^static\s+void\s+WifiAccessStatus\s*\(",
        ),
        extract_definition(
            WORKER, "OpenWifiDev", r"(?m)^static\s+BOOL\s+OpenWifiDev\s*\("
        ),
        extract_definition(
            WORKER, "ProbeWireless", r"(?m)^static\s+BOOL\s+ProbeWireless\s*\("
        ),
        extract_definition(
            WORKER,
            "DeviceIsWireless",
            r"(?m)^static\s+BOOL\s+DeviceIsWireless\s*\(",
        ),
    ]
    source = "\n\n".join(pieces)
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    return source, digest


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    production, digest = extract_sources()
    print(
        "Extracted WifiDev and six production helpers "
        f"(SHA-256 {digest})"
    )
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

    with tempfile.TemporaryDirectory(prefix="wifi-device-access-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_device_access.c"
        source_path.write_text(
            FIXTURE_PREFIX + "\n" + production + "\n" + FIXTURE_TESTS,
            encoding="utf-8",
        )

        profiles = (
            ("generic", []),
            ("flash", ["-DWIFI_P4_FLASH_HELPERS"]),
        )
        for profile, defines in profiles:
            for optimization in (0, 2):
                binary = directory / f"test_device_access_{profile}_O{optimization}"
                command = compiler + [
                    "-std=c99",
                    f"-O{optimization}",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    "-DWIFI_ACCESS_DIAGNOSTICS",
                    *defines,
                    str(source_path),
                    "-o",
                    str(binary),
                ]
                print(
                    f"Building {profile} O{optimization} with ASan/UBSan "
                    "and numeric observer enabled"
                )
                build = subprocess.run(
                    command,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=60,
                )
                if build.returncode != 0:
                    sys.stderr.write(build.stdout)
                    sys.stderr.write(build.stderr)
                    return build.returncode or 1
                if build.stdout:
                    sys.stdout.write(build.stdout)
                if build.stderr:
                    sys.stderr.write(build.stderr)

                try:
                    result = subprocess.run(
                        [str(binary)],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=20,
                        env=environment,
                    )
                except subprocess.TimeoutExpired:
                    print(
                        f"FAIL: {profile} O{optimization} fixture timed out",
                        file=sys.stderr,
                    )
                    return 1
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                if result.returncode != 0:
                    return result.returncode
                if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                    print(
                        f"FAIL: {profile} O{optimization} did not report its "
                        "full case count",
                        file=sys.stderr,
                    )
                    return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
