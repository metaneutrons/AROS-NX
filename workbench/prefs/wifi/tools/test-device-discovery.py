#!/usr/bin/env python3
"""Run sanitized host regressions against production Wi-Fi device discovery.

The directory API and device open/query boundary are mocked. Passing this test
is source-level evidence only; it does not establish native, board, or RF
acceptance.
"""

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
WIFIGUI = WIFI / "wifigui.h"
INSERT_LIMITS = "/* INSERT_WIFI_LIMITS */"
INSERT_PRODUCTION = "/* INSERT_PRODUCTION_SOURCE */"


FIXTURE_PREFIX = r'''#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef unsigned char TEXT;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef void *BPTR;
typedef TEXT *STRPTR;
typedef const TEXT *CONST_STRPTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define DOS_FIB 1u
#define SHARED_LOCK 2
#define BNULL ((BPTR)0)
#define _(text) (text)
#define Stricmp(a, b) strcasecmp((const char *)(a), (const char *)(b))
#define MSG_ST_NO_DEVS_NETWORKS "No network drivers"
#define MSG_ST_NO_WIRELESS_DEVICE "No wireless devices"
#define MSG_ST_CANNOT_OPEN "Cannot open %s unit %ld"

#ifdef WIFI_P4_FLASH_HELPERS
#define WIFI_DEVICE_DIR "FLASHDISK0P0:Devs/Networks"
#else
#define WIFI_DEVICE_DIR "DEVS:Networks"
#endif

/* These limits are extracted from the real wifigui.h below. */
/* INSERT_WIFI_LIMITS */

struct MsgPort { unsigned marker; };
struct Library { UBYTE lib_Version; UBYTE lib_Revision; };
struct Device { struct Library dd_Library; };
struct IORequest { struct Device *io_Device; };
struct IOSana2Req { struct IORequest ios2_Req; };
struct FileInfoBlock {
    TEXT fib_FileName[256];
    LONG fib_DirEntryType;
};
struct WifiMsg {
    TEXT wm_Status[96];
    TEXT wm_Device[WIFI_DEV_MAX];
    LONG wm_Unit;
    LONG wm_DeviceCount;
    TEXT wm_Devices[WIFI_MAX_DEVS][WIFI_DEV_MAX];
    LONG wm_DeviceUnits[WIFI_MAX_DEVS];
};

static unsigned checks;
static unsigned cases;
static const char *case_name = "startup";

static void check(int ok, int line, const char *expression)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL %s line %d: %s\n",
            case_name, line, expression);
        exit(1);
    }
}
#define CHECK(expression) check((expression), __LINE__, #expression)

struct WifiDev;
static struct MsgPort *CreateMsgPort(void);
static void DeleteMsgPort(struct MsgPort *port);
static APTR AllocVec(ULONG size, ULONG flags);
static void FreeVec(APTR memory);
static void CloseDevice(struct IORequest *request);
#ifdef WIFI_ACCESS_DIAGNOSTICS
static void bug(const char *format, ...);
#endif
static BOOL WifiAccessFailure(struct WifiDev *dev, LONG stage, LONG error,
    ULONG version, ULONG revision);
static BOOL OpenWifiDev(struct WifiDev *dev, CONST_STRPTR device, LONG unit);
static BOOL ProbeWireless(struct WifiDev *dev);

static unsigned created_ports;
static unsigned deleted_ports;
static unsigned live_ports;
static unsigned allocated_requests;
static unsigned freed_requests;
static unsigned live_requests;
static unsigned close_device_calls;
static unsigned successful_opens;
static unsigned open_attempts;
static unsigned probe_calls;
static char observer_log[4096];
static size_t observer_used;

static unsigned lock_calls;
static unsigned fib_alloc_calls;
static unsigned allocated_fibs;
static unsigned freed_fibs;
static unsigned examine_calls;
static unsigned exnext_calls;
static unsigned unlock_calls;
static unsigned ioerr_calls;
static unsigned sequence;
static unsigned ioerr_sequence;
static unsigned free_fib_sequence;
static unsigned unlock_sequence;
static LONG ioerr_result;
static BOOL fail_lock;
static BOOL fail_fib_alloc;
static BOOL fail_examine;
static struct MsgPort mock_port;
static struct Device mock_device;

struct MockEntry { const char *name; LONG type; };
static struct MockEntry entries[64];
static size_t entry_count;
static size_t next_entry;
static BOOL fail_open;
static LONG fail_open_unit;
static LONG fail_open_error;
static LONG wireless_unit_limit;
static LONG current_open_unit;
static char current_open_name[WIFI_DEV_MAX];
static LONG last_failure_stage;
static LONG last_failure_error;
static ULONG last_failure_version;
static ULONG last_failure_revision;

static struct MsgPort *Lock(CONST_STRPTR name, LONG mode);
static LONG IoErr(void);
static APTR AllocDosObject(ULONG type, APTR tags);
static void FreeDosObject(ULONG type, APTR object);
static BOOL Examine(BPTR lock, struct FileInfoBlock *fib);
static BOOL ExNext(BPTR lock, struct FileInfoBlock *fib);
static void UnLock(BPTR lock);

/* INSERT_PRODUCTION_SOURCE */

static unsigned create_msg_port_calls;
static unsigned alloc_vec_calls;
static unsigned next_call_sequence;

static void add_entry(const char *name, LONG type)
{
    CHECK(entry_count < sizeof(entries) / sizeof(entries[0]));
    entries[entry_count].name = name;
    entries[entry_count].type = type;
    ++entry_count;
}

static void reset_fixture(const char *name)
{
    CHECK(live_ports == 0);
    CHECK(live_requests == 0);
    case_name = name;
    created_ports = 0;
    deleted_ports = 0;
    live_ports = 0;
    allocated_requests = 0;
    freed_requests = 0;
    live_requests = 0;
    close_device_calls = 0;
    successful_opens = 0;
    open_attempts = 0;
    probe_calls = 0;
    observer_used = 0;
    observer_log[0] = '\0';
    lock_calls = 0;
    fib_alloc_calls = 0;
    allocated_fibs = 0;
    freed_fibs = 0;
    examine_calls = 0;
    exnext_calls = 0;
    unlock_calls = 0;
    ioerr_calls = 0;
    sequence = 0;
    ioerr_sequence = 0;
    free_fib_sequence = 0;
    unlock_sequence = 0;
    ioerr_result = 0;
    fail_lock = FALSE;
    fail_fib_alloc = FALSE;
    fail_examine = FALSE;
    entry_count = 0;
    next_entry = 0;
    fail_open = FALSE;
    fail_open_unit = 0;
    fail_open_error = 0;
    wireless_unit_limit = 1;
    current_open_unit = -1;
    current_open_name[0] = '\0';
    last_failure_stage = 0;
    last_failure_error = 0;
    last_failure_version = 0;
    last_failure_revision = 0;
    create_msg_port_calls = 0;
    alloc_vec_calls = 0;
    next_call_sequence = 0;
}

static void expect_observer(const char *expected)
{
#ifdef WIFI_ACCESS_DIAGNOSTICS
    CHECK(strcmp(observer_log, expected) == 0);
#else
    (void)expected;
    CHECK(observer_used == 0);
#endif
    CHECK(strstr(observer_log, "SSID") == NULL);
    CHECK(strstr(observer_log, "BSSID") == NULL);
    CHECK(strstr(observer_log, "PASSWORD") == NULL);
}

static void expect_capped_observer(void)
{
#ifdef WIFI_ACCESS_DIAGNOSTICS
    char expected[512];
    size_t used = (size_t)snprintf(expected, sizeof(expected),
        "[WiFiAccess] discovery=1\n");
    LONG i;
    for (i = 1; i < WIFI_MAX_DEVS; ++i)
        used += (size_t)snprintf(expected + used, sizeof(expected) - used,
            "[WiFiAccess] s=6 e=0 v=0 r=0\n");
    (void)snprintf(expected + used, sizeof(expected) - used,
        "[WiFiAccess] devices=%ld\n", (long)WIFI_MAX_DEVS);
    expect_observer(expected);
#else
    expect_observer("");
#endif
}

static void expect_tree_cleanup(unsigned expected_fibs,
    unsigned expected_frees, unsigned expected_unlocks)
{
    CHECK(lock_calls == 1);
    CHECK(fib_alloc_calls == expected_fibs);
    CHECK(allocated_fibs == expected_frees);
    CHECK(freed_fibs == expected_frees);
    CHECK(unlock_calls == expected_unlocks);
}

static void expect_device_cleanup(unsigned attempts, unsigned successes)
{
    CHECK(open_attempts == attempts);
    CHECK(successful_opens == successes);
    CHECK(close_device_calls == successes);
    CHECK(created_ports == attempts);
    CHECK(deleted_ports == attempts);
    CHECK(allocated_requests == attempts);
    CHECK(freed_requests == attempts);
    CHECK(live_ports == 0);
    CHECK(live_requests == 0);
}

static void expect_no_entries(const struct WifiMsg *m)
{
    size_t i;
    CHECK(m->wm_DeviceCount == 0);
    for (i = 0; i < WIFI_MAX_DEVS; ++i)
        CHECK(m->wm_Devices[i][0] == '\0');
}
'''


FIXTURE_MOCKS = r'''
static struct MsgPort *CreateMsgPort(void)
{
    ++create_msg_port_calls;
    ++created_ports;
    ++live_ports;
    return (struct MsgPort *)calloc(1, sizeof(struct MsgPort));
}

static void DeleteMsgPort(struct MsgPort *port)
{
    CHECK(port != NULL);
    CHECK(live_ports != 0);
    --live_ports;
    ++deleted_ports;
    free(port);
}

static APTR AllocVec(ULONG size, ULONG flags)
{
    CHECK((flags & MEMF_CLEAR) != 0);
    ++alloc_vec_calls;
    ++allocated_requests;
    ++live_requests;
    return calloc(1, size);
}

static void FreeVec(APTR memory)
{
    CHECK(memory != NULL);
    CHECK(live_requests != 0);
    --live_requests;
    ++freed_requests;
    free(memory);
}

static void CloseDevice(struct IORequest *request)
{
    CHECK(request != NULL);
    CHECK(request->io_Device == &mock_device);
    ++close_device_calls;
}

static struct MsgPort *Lock(CONST_STRPTR name, LONG mode)
{
    ++lock_calls;
    CHECK(strcmp((const char *)name, WIFI_DEVICE_DIR) == 0);
    CHECK(mode == SHARED_LOCK);
    if (fail_lock)
        return NULL;
    return &mock_port;
}

static LONG IoErr(void)
{
    ++ioerr_calls;
    ioerr_sequence = ++sequence;
    return ioerr_result;
}

static APTR AllocDosObject(ULONG type, APTR tags)
{
    ++fib_alloc_calls;
    CHECK(type == DOS_FIB);
    CHECK(tags == NULL);
    if (fail_fib_alloc)
        return NULL;
    ++allocated_fibs;
    return calloc(1, sizeof(struct FileInfoBlock));
}

static void FreeDosObject(ULONG type, APTR object)
{
    CHECK(type == DOS_FIB);
    CHECK(object != NULL);
    ++freed_fibs;
    free_fib_sequence = ++sequence;
    free(object);
}

static BOOL Examine(BPTR lock, struct FileInfoBlock *fib)
{
    ++examine_calls;
    CHECK(lock == (BPTR)&mock_port);
    CHECK(fib != NULL);
    return !fail_examine;
}

static BOOL ExNext(BPTR lock, struct FileInfoBlock *fib)
{
    ++exnext_calls;
    CHECK(lock == (BPTR)&mock_port);
    CHECK(fib != NULL);
    if (next_entry >= entry_count)
        return FALSE;
    snprintf((char *)fib->fib_FileName, sizeof(fib->fib_FileName), "%s",
        entries[next_entry].name);
    fib->fib_DirEntryType = entries[next_entry].type;
    ++next_entry;
    return TRUE;
}

static void UnLock(BPTR lock)
{
    CHECK(lock == (BPTR)&mock_port);
    ++unlock_calls;
    unlock_sequence = ++sequence;
}

#ifdef WIFI_ACCESS_DIAGNOSTICS
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
#endif

/* Mock only the open/query edge. Production cleanup and status code above
 * remain active so discovery owns the real boundary behavior. */
static BOOL OpenWifiDev(struct WifiDev *dev, CONST_STRPTR device, LONG unit)
{
    char expected_prefix[WIFI_DEV_MAX];
    memset(dev, 0, sizeof(*dev));
    ++open_attempts;
    current_open_unit = unit;
    snprintf(current_open_name, sizeof(current_open_name), "%s",
        (const char *)device);
    snprintf(expected_prefix, sizeof(expected_prefix), "%s/", WIFI_DEVICE_DIR);
    CHECK(strncmp(current_open_name, expected_prefix,
        strlen(expected_prefix)) == 0);
    dev->wd_Port = CreateMsgPort();
    CHECK(dev->wd_Port != NULL);
    dev->wd_Req = AllocVec(sizeof(*dev->wd_Req), MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(dev->wd_Req != NULL);
    if (fail_open && unit == fail_open_unit) {
        (void)WifiAccessFailure(dev, 3, fail_open_error, 0, 0);
        last_failure_stage = dev->wd_Failure;
        last_failure_error = dev->wd_Error;
        last_failure_version = dev->wd_Version;
        last_failure_revision = dev->wd_Revision;
        return FALSE;
    }
    dev->wd_Opened = TRUE;
    dev->wd_Req->ios2_Req.io_Device = &mock_device;
    ++successful_opens;
    return TRUE;
}

static BOOL ProbeWireless(struct WifiDev *dev)
{
    BOOL wireless = current_open_unit < wireless_unit_limit;
    ++probe_calls;
    if (!wireless) {
        dev->wd_Failure = 6;
        dev->wd_Error = 0;
#ifdef WIFI_ACCESS_DIAGNOSTICS
        bug("[WiFiAccess] s=6 e=0 v=0 r=0\n");
#endif
    }
    return wireless;
}
'''


FIXTURE_TESTS = r'''
static void test_lock_failure(void)
{
    struct WifiMsg m;
    reset_fixture("lock failure stage 7");
    memset(&m, 0, sizeof(m));
    fail_lock = TRUE;
    ioerr_result = 73;
    DoDevices(&m);
    CHECK(ioerr_calls == 1 && ioerr_result == 73);
    CHECK(fib_alloc_calls == 0 && examine_calls == 0 && exnext_calls == 0);
    CHECK(freed_fibs == 0 && unlock_calls == 0);
    expect_no_entries(&m);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status,
        "WiFi discovery: directory error=73") == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_DEVS_NETWORKS) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=7 e=73 v=0 r=0\n");
    expect_tree_cleanup(0, 0, 0);
    expect_device_cleanup(0, 0);
    ++cases;
}

static void test_file_info_allocation_failure(void)
{
    struct WifiMsg m;
    reset_fixture("file-info allocation stage 8");
    memset(&m, 0, sizeof(m));
    fail_fib_alloc = TRUE;
    DoDevices(&m);
    CHECK(examine_calls == 0 && exnext_calls == 0);
    expect_no_entries(&m);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status,
        "WiFi discovery: no file-info memory") == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=8 e=0 v=0 r=0\n"
        "[WiFiAccess] devices=0\n");
    expect_tree_cleanup(1, 0, 1);
    expect_device_cleanup(0, 0);
    ++cases;
}

static void test_examine_failure_ioerr_before_cleanup(void)
{
    struct WifiMsg m;
    reset_fixture("Examine failure stage 9");
    memset(&m, 0, sizeof(m));
    fail_examine = TRUE;
    ioerr_result = 91;
    DoDevices(&m);
    CHECK(ioerr_calls == 1 && ioerr_result == 91);
    CHECK(ioerr_sequence != 0);
    CHECK(ioerr_sequence < free_fib_sequence);
    CHECK(ioerr_sequence < unlock_sequence);
    expect_no_entries(&m);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status,
        "WiFi discovery: examine error=91") == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=9 e=91 v=0 r=0\n"
        "[WiFiAccess] devices=0\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(0, 0);
    ++cases;
}

static void test_empty_directory(void)
{
    struct WifiMsg m;
    reset_fixture("empty directory");
    memset(&m, 0, sizeof(m));
    DoDevices(&m);
    CHECK(examine_calls == 1 && exnext_calls == 1);
    expect_no_entries(&m);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] devices=0\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(0, 0);
    ++cases;
}

static void test_directory_non_device_and_case_insensitive_suffix(void)
{
    struct WifiMsg m;
    char expected[WIFI_DEV_MAX];
    reset_fixture("directory and filename filtering");
    memset(&m, 0, sizeof(m));
    add_entry("dir.DEVICE", 1);
    add_entry("ethernet.driver", -1);
    add_entry("backup.device.old", -1);
    add_entry("readme.txt", -1);
    add_entry("wifi.DeViCe", -1);
    add_entry("without-suffix", -1);
    wireless_unit_limit = 1;
    DoDevices(&m);
    CHECK(m.wm_DeviceCount == 1);
    snprintf(expected, sizeof(expected), "%s/%s", WIFI_DEVICE_DIR,
        "wifi.DeViCe");
    CHECK(strcmp((const char *)m.wm_Devices[0], expected) == 0);
    CHECK(m.wm_DeviceUnits[0] == 0);
    CHECK(open_attempts == 2 && probe_calls == 2);
    CHECK(exnext_calls == 7);
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=6 e=0 v=0 r=0\n"
        "[WiFiAccess] devices=1\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(2, 2);
    ++cases;
}

static void test_unit_zero_open_failure_preserves_metadata(void)
{
    struct WifiMsg m;
    reset_fixture("unit-zero OpenDevice failure");
    memset(&m, 0, sizeof(m));
    add_entry("radio.device", -1);
    fail_open = TRUE;
    fail_open_unit = 0;
    fail_open_error = 37;
    DoDevices(&m);
    expect_no_entries(&m);
    CHECK(last_failure_stage == 3 && last_failure_error == 37);
    CHECK(last_failure_version == 0 && last_failure_revision == 0);
    CHECK(probe_calls == 0);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status,
        "WiFi access: stage=3 error=37 version=0.0") == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=3 e=37 v=0 r=0\n"
        "[WiFiAccess] devices=0\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(1, 0);
    ++cases;
}

static void test_query_refusal_does_not_add_chooser_entry(void)
{
    struct WifiMsg m;
    reset_fixture("query refusal");
    memset(&m, 0, sizeof(m));
    add_entry("ethernet.device", -1);
    wireless_unit_limit = 0;
    DoDevices(&m);
    expect_no_entries(&m);
    CHECK(probe_calls == 1);
#ifdef WIFI_P4_FLASH_HELPERS
    CHECK(strcmp((const char *)m.wm_Status,
        "WiFi access: stage=6 error=0 version=0.0") == 0);
#else
    CHECK(strcmp((const char *)m.wm_Status, MSG_ST_NO_WIRELESS_DEVICE) == 0);
#endif
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] s=6 e=0 v=0 r=0\n"
        "[WiFiAccess] devices=0\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(1, 1);
    ++cases;
}

static void test_device_count_cap(void)
{
    struct WifiMsg m;
    unsigned i;
    char expected[WIFI_DEV_MAX];
    reset_fixture("WIFI_MAX_DEVS cap");
    memset(&m, 0, sizeof(m));
    for (i = 0; i < WIFI_MAX_DEVS + 4; ++i) {
        char name[32];
        static char names[32][32];
        snprintf(name, sizeof(name), "radio%u.device", i);
        snprintf(names[i], sizeof(names[i]), "%s", name);
        add_entry(names[i], -1);
    }
    wireless_unit_limit = 1;
    DoDevices(&m);
    CHECK(m.wm_DeviceCount == WIFI_MAX_DEVS);
    for (i = 0; i < WIFI_MAX_DEVS; ++i) {
        char file[32];
        snprintf(file, sizeof(file), "radio%u.device", i);
        snprintf(expected, sizeof(expected), "%s/%s", WIFI_DEVICE_DIR, file);
        CHECK(strcmp((const char *)m.wm_Devices[i], expected) == 0);
        CHECK(m.wm_DeviceUnits[i] == 0);
    }
    CHECK(open_attempts == WIFI_MAX_DEVS * 2 - 1);
    CHECK(probe_calls == WIFI_MAX_DEVS * 2 - 1);
    CHECK(exnext_calls == WIFI_MAX_DEVS + 1);
    expect_capped_observer();
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(WIFI_MAX_DEVS * 2 - 1, WIFI_MAX_DEVS * 2 - 1);
    ++cases;
}

static void test_unit_count_cap(void)
{
    struct WifiMsg m;
    LONG unit;
    char expected[WIFI_DEV_MAX];
    reset_fixture("WIFI_MAX_UNITS cap");
    memset(&m, 0, sizeof(m));
    add_entry("multiunit.device", -1);
    wireless_unit_limit = WIFI_MAX_UNITS + 1;
    DoDevices(&m);
    CHECK(m.wm_DeviceCount == WIFI_MAX_UNITS);
    snprintf(expected, sizeof(expected), "%s/%s", WIFI_DEVICE_DIR,
        "multiunit.device");
    for (unit = 0; unit < WIFI_MAX_UNITS; ++unit) {
        CHECK(strcmp((const char *)m.wm_Devices[unit], expected) == 0);
        CHECK(m.wm_DeviceUnits[unit] == unit);
    }
    CHECK(open_attempts == WIFI_MAX_UNITS);
    CHECK(probe_calls == WIFI_MAX_UNITS);
    CHECK(exnext_calls == 2);
    expect_observer("[WiFiAccess] discovery=1\n"
        "[WiFiAccess] devices=4\n");
    expect_tree_cleanup(1, 1, 1);
    expect_device_cleanup(WIFI_MAX_UNITS, WIFI_MAX_UNITS);
    ++cases;
}

int main(void)
{
    test_lock_failure();
    test_file_info_allocation_failure();
    test_examine_failure_ioerr_before_cleanup();
    test_empty_directory();
    test_directory_non_device_and_case_insensitive_suffix();
    test_unit_zero_open_failure_preserves_metadata();
    test_query_refusal_does_not_add_chooser_entry();
    test_device_count_cap();
    test_unit_count_cap();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
'''


def extract_sources() -> tuple[str, str]:
    helpers = runpy.run_path(str(EXTRACTOR))
    extract_definition = helpers["extract_definition"]
    extract_constant_lines = helpers["extract_constant_lines"]
    limits = extract_constant_lines(
        WIFIGUI, ("WIFI_MAX_DEVS", "WIFI_MAX_UNITS", "WIFI_DEV_MAX")
    )
    definitions = [
        extract_definition(WORKER, "WifiDev", r"(?m)^struct\s+WifiDev\b")
        + ";",
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
            WORKER, "DoDevices", r"(?m)^static\s+void\s+DoDevices\s*\("
        ),
    ]
    source = limits + "\n\n" + "\n\n".join(definitions)
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    return source, digest


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    production, digest = extract_sources()
    print(
        "Extracted actual DoDevices, WifiAccessStatus, WifiAccessFailure, "
        "CloseWifiDev, WifiDev and chooser limits "
        f"(SHA-256 {digest})"
    )
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

    limits, definitions = production.split("\n\n", 1)
    fixture = FIXTURE_PREFIX.replace(INSERT_LIMITS, limits, 1)
    fixture = fixture.replace(INSERT_PRODUCTION, definitions, 1)
    fixture += "\n" + FIXTURE_MOCKS + "\n" + FIXTURE_TESTS
    with tempfile.TemporaryDirectory(prefix="wifi-device-discovery-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_device_discovery.c"
        source_path.write_text(fixture, encoding="utf-8")
        profiles = (
            ("generic", []),
            ("flash", ["-DWIFI_P4_FLASH_HELPERS"]),
        )
        for profile, profile_defines in profiles:
            for optimization in (0, 2):
                for observer in (False, True):
                    mode = "observer-on" if observer else "observer-off"
                    binary = directory / (
                        f"test_device_discovery_{profile}_O{optimization}_{mode}"
                    )
                    command = compiler + [
                        "-std=c99",
                        f"-O{optimization}",
                        "-g",
                        "-Wall",
                        "-Wextra",
                        "-Wpedantic",
                        "-Werror",
                        "-Wno-pointer-sign",
                        "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer",
                        *(["-DWIFI_ACCESS_DIAGNOSTICS"] if observer else []),
                        *profile_defines,
                        str(source_path),
                        "-o",
                        str(binary),
                    ]
                    print(
                        f"Building {profile} O{optimization} {mode} "
                        "with ASan/UBSan"
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
                            f"FAIL: {profile} O{optimization} {mode} "
                            "fixture timed out",
                            file=sys.stderr,
                        )
                        return 1
                    sys.stdout.write(result.stdout)
                    sys.stderr.write(result.stderr)
                    if result.returncode != 0:
                        return result.returncode
                    if not re.search(
                        r"(?m)^PASS: 9 cases, \d+ checks$", result.stdout
                    ):
                        print(
                            f"FAIL: {profile} O{optimization} {mode} did not "
                            "report all nine cases",
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
