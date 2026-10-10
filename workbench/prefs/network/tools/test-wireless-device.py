#!/usr/bin/env python3
"""Host tests for wireless_device.c and the wireless selection branch.

This compiles the production source itself. All hardware-facing calls are
explicit mocks; passing these tests does not claim device-runtime behavior.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


NETWORK_DIR = Path(__file__).resolve().parents[1]
SOURCE = NETWORK_DIR / "wireless_device.c"
PREFSDATA_SOURCE = NETWORK_DIR / "prefsdata.c"


FIXTURE_HEADERS = {
    "exec/types.h": r"""
#ifndef TEST_EXEC_TYPES_H
#define TEST_EXEC_TYPES_H
#include <stdint.h>
typedef void *APTR;
typedef const void *CONST_APTR;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t WORD;
typedef uint16_t UWORD;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef intptr_t IPTR;
typedef int BOOL;
typedef const char *CONST_STRPTR;
typedef char *STRPTR;
#define TRUE 1
#define FALSE 0
#endif
""",
    "exec/io.h": r"""
#ifndef TEST_EXEC_IO_H
#define TEST_EXEC_IO_H
#include <exec/types.h>
struct MsgPort;
struct Message {
    APTR fixture_message_storage[4];
    struct MsgPort *mn_ReplyPort;
    UWORD mn_Length;
};
struct IORequest {
    struct Message io_Message;
    APTR io_Device;
    APTR io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};
struct IOStdReq {
    struct Message io_Message;
    APTR io_Device;
    APTR io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
    ULONG io_Actual;
    ULONG io_Length;
    APTR io_Data;
    ULONG io_Offset;
};
#define CMD_NONSTD 9
#endif
""",
    "exec/ports.h": r"""
#ifndef TEST_EXEC_PORTS_H
#define TEST_EXEC_PORTS_H
#include <exec/types.h>
struct MsgPort {
    UBYTE mp_SigBit;
};
#endif
""",
    "devices/newstyle.h": r"""
#ifndef TEST_DEVICES_NEWSTYLE_H
#define TEST_DEVICES_NEWSTYLE_H
#include <exec/types.h>
#define NSCMD_DEVICEQUERY 0x4000
#define NSDEVTYPE_SANA2 7
struct NSDeviceQueryResult {
    ULONG DevQueryFormat;
    ULONG SizeAvailable;
    UWORD DeviceType;
    UWORD DeviceSubType;
    UWORD *SupportedCommands;
};
#endif
""",
    "devices/sana2.h": r"""
#ifndef TEST_DEVICES_SANA2_H
#define TEST_DEVICES_SANA2_H
#include <exec/io.h>
#define S2_Dummy 0x8000
#define S2_CopyToBuff (S2_Dummy + 1)
#define S2_CopyFromBuff (S2_Dummy + 2)
#define S2_GETNETWORKS 0xc011
#define S2_ONLINE (CMD_NONSTD + 16)
#define SANA2_MAX_ADDR_BYTES 16
struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;
    ULONG ios2_PacketType;
    UBYTE ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
    UBYTE ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
    ULONG ios2_DataLength;
    APTR ios2_Data;
    APTR ios2_StatData;
    APTR ios2_BufferManagement;
};
#endif
""",
    "devices/sana2wireless.h": r"""
#ifndef TEST_DEVICES_SANA2WIRELESS_H
#define TEST_DEVICES_SANA2WIRELESS_H
#include <devices/sana2.h>
#endif
""",
    "devices/timer.h": r"""
#ifndef TEST_DEVICES_TIMER_H
#define TEST_DEVICES_TIMER_H
#include <exec/io.h>
#define TIMERNAME "timer.device"
#define UNIT_MICROHZ 0
#define TR_ADDREQUEST (CMD_NONSTD + 0)
struct AROSTestTimeval {
    LONG tv_secs;
    LONG tv_micro;
};
#define timeval AROSTestTimeval
struct timerequest {
    struct IORequest tr_node;
    struct timeval tr_time;
};
#endif
""",
    "utility/tagitem.h": r"""
#ifndef TEST_UTILITY_TAGITEM_H
#define TEST_UTILITY_TAGITEM_H
#include <exec/types.h>
#define TAG_DONE 0
struct TagItem {
    ULONG ti_Tag;
    IPTR ti_Data;
};
#endif
""",
    "proto/exec.h": r"""
#ifndef TEST_PROTO_EXEC_H
#define TEST_PROTO_EXEC_H
#include <exec/io.h>
#include <exec/ports.h>
struct MsgPort *CreateMsgPort(void);
void DeleteMsgPort(struct MsgPort *port);
APTR CreateIORequest(struct MsgPort *port, ULONG size);
void DeleteIORequest(APTR request);
LONG OpenDevice(CONST_STRPTR name, ULONG unit, APTR request, ULONG flags);
void CloseDevice(APTR request);
void SendIO(APTR request);
BOOL CheckIO(APTR request);
void Wait(ULONG signal_mask);
void AbortIO(APTR request);
void WaitIO(APTR request);
#endif
""",
}


HARNESS = r"""
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <devices/newstyle.h>
#include <devices/sana2.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include "wireless_device.c"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(2); \
    } \
} while (0)

enum CompletionMode {
    COMPLETE_REQUEST,
    COMPLETE_TIMER,
    COMPLETE_BOTH
};

enum Event {
    EVENT_WAIT,
    EVENT_ABORT_REQUEST,
    EVENT_ABORT_TIMER,
    EVENT_WAIT_REQUEST,
    EVENT_WAIT_TIMER,
    EVENT_CLOSE_DEVICE,
    EVENT_CLOSE_TIMER,
    EVENT_DELETE_REQUEST,
    EVENT_DELETE_TIMER,
    EVENT_DELETE_PORT
};

static int fail_port_call = -1;
static int fail_io_call = -1;
static int fail_open_call = -1;
static int port_calls;
static int io_calls;
static int open_calls;
static int close_calls;
static int port_allocations;
static int port_deletions;
static int io_allocations;
static int io_deletions;
static int live_ports;
static int live_io;
static int device_open;
static int timer_open;
static int request_sent;
static int timer_sent;
static int request_complete;
static int timer_complete;
static int request_aborted;
static int timer_aborted;
static int request_waited;
static int timer_waited;
static int wait_calls;
static int device_send_calls;
static int timer_send_calls;
static enum CompletionMode completion_mode = COMPLETE_REQUEST;
static struct MsgPort *port_slots[2];
static APTR io_slots[2];
static struct IOSana2Req *device_request;
static struct timerequest *timer_request;
static struct NSDeviceQueryResult response_query;
static UWORD response_commands[256];
static ULONG response_actual;
static BYTE response_error;
static struct TagItem *observed_tags;
static CONST_STRPTR expected_device_name = "wifi.device";
static ULONG expected_device_unit = 3;
static enum Event events[64];
static size_t event_count;

#define NAMEBUFLEN 128
typedef char TEXT;
struct TestPrefs {
    STRPTR wirelessDevice;
    LONG wirelessUnit;
};
static struct TestPrefs prefs;
static TEXT wirelessDeviceName[NAMEBUFLEN];

static void record_event(enum Event event)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = event;
}

static size_t event_index(enum Event wanted)
{
    size_t i;
    for (i = 0; i < event_count; ++i)
        if (events[i] == wanted)
            return i;
    return (size_t)-1;
}

static void reset_mocks(void)
{
    CHECK(live_ports == 0);
    CHECK(live_io == 0);
    fail_port_call = -1;
    fail_io_call = -1;
    fail_open_call = -1;
    port_calls = 0;
    io_calls = 0;
    open_calls = 0;
    close_calls = 0;
    port_allocations = 0;
    port_deletions = 0;
    io_allocations = 0;
    io_deletions = 0;
    live_ports = 0;
    live_io = 0;
    device_open = 0;
    timer_open = 0;
    request_sent = 0;
    timer_sent = 0;
    request_complete = 0;
    timer_complete = 0;
    request_aborted = 0;
    timer_aborted = 0;
    request_waited = 0;
    timer_waited = 0;
    wait_calls = 0;
    device_send_calls = 0;
    timer_send_calls = 0;
    completion_mode = COMPLETE_REQUEST;
    memset(port_slots, 0, sizeof(port_slots));
    memset(io_slots, 0, sizeof(io_slots));
    device_request = NULL;
    timer_request = NULL;
    memset(&response_query, 0, sizeof(response_query));
    memset(response_commands, 0, sizeof(response_commands));
    response_actual = sizeof(struct NSDeviceQueryResult);
    response_error = 0;
    observed_tags = NULL;
    expected_device_name = "wifi.device";
    expected_device_unit = 3;
    memset(events, 0, sizeof(events));
    event_count = 0;
}

static void prepare_response(const UWORD *commands, ULONG size_available,
                             ULONG format, UWORD device_type, BYTE error)
{
    response_query.DevQueryFormat = format;
    response_query.SizeAvailable = size_available;
    response_query.DeviceType = device_type;
    response_query.DeviceSubType = 0;
    response_query.SupportedCommands = (UWORD *)commands;
    response_error = error;
    response_actual = sizeof(struct NSDeviceQueryResult);
}

static void assert_clean(void)
{
    CHECK(live_ports == 0);
    CHECK(live_io == 0);
    CHECK(port_allocations == port_deletions);
    CHECK(io_allocations == io_deletions);
    CHECK(device_open == 0);
    CHECK(timer_open == 0);
    if (request_sent)
        CHECK(request_waited);
    if (timer_sent)
        CHECK(timer_waited);
}

struct MsgPort *CreateMsgPort(void)
{
    struct MsgPort *port;
    int call = ++port_calls;
    CHECK(call <= 2);
    if (call == fail_port_call)
        return NULL;
    port = (struct MsgPort *)calloc(1, sizeof(*port));
    CHECK(port != NULL);
    port->mp_SigBit = (UBYTE)(call + 1);
    port_slots[call - 1] = port;
    ++port_allocations;
    ++live_ports;
    return port;
}

void DeleteMsgPort(struct MsgPort *port)
{
    int found = 0;
    size_t i;
    CHECK(port != NULL);
    for (i = 0; i < 2; ++i)
    {
        if (port_slots[i] == port)
        {
            port_slots[i] = NULL;
            found = 1;
            break;
        }
    }
    CHECK(found);
    CHECK(live_io == 0);
    record_event(EVENT_DELETE_PORT);
    ++port_deletions;
    --live_ports;
    free(port);
}

APTR CreateIORequest(struct MsgPort *port, ULONG size)
{
    APTR request;
    int call = ++io_calls;
    CHECK(port != NULL);
    CHECK(call <= 2);
    if (call == fail_io_call)
        return NULL;
    if (size == sizeof(struct IOSana2Req))
    {
        CHECK(device_request == NULL);
        request = calloc(1, size);
        CHECK(request != NULL);
        device_request = (struct IOSana2Req *)request;
    }
    else
    {
        CHECK(size == sizeof(struct timerequest));
        CHECK(timer_request == NULL);
        request = calloc(1, size);
        CHECK(request != NULL);
        timer_request = (struct timerequest *)request;
    }
    io_slots[call - 1] = request;
    ++io_allocations;
    ++live_io;
    return request;
}

void DeleteIORequest(APTR request)
{
    size_t i;
    int found = 0;
    CHECK(request != NULL);
    if (request == (APTR)device_request)
    {
        CHECK(!device_open);
        CHECK(!request_sent || request_waited);
        record_event(EVENT_DELETE_REQUEST);
    }
    else
    {
        CHECK(request == (APTR)timer_request);
        CHECK(!timer_open);
        CHECK(!timer_sent || timer_waited);
        record_event(EVENT_DELETE_TIMER);
    }
    for (i = 0; i < 2; ++i)
    {
        if (io_slots[i] == request)
        {
            io_slots[i] = NULL;
            found = 1;
            break;
        }
    }
    CHECK(found);
    ++io_deletions;
    --live_io;
    free(request);
}

LONG OpenDevice(CONST_STRPTR name, ULONG unit, APTR request, ULONG flags)
{
    int call = ++open_calls;
    CHECK(flags == 0);
    if (call == 1)
    {
        CHECK(strcmp(name, TIMERNAME) == 0);
        CHECK(unit == UNIT_MICROHZ);
        CHECK(request == (APTR)timer_request);
    }
    else
    {
        CHECK(call == 2);
        CHECK(strcmp(name, expected_device_name) == 0);
        CHECK(unit == expected_device_unit);
        CHECK(request == (APTR)device_request);
        observed_tags = (struct TagItem *)device_request->ios2_BufferManagement;
        CHECK(observed_tags != NULL);
        CHECK(observed_tags[0].ti_Tag == S2_CopyToBuff);
        CHECK(observed_tags[1].ti_Tag == S2_CopyFromBuff);
        CHECK(observed_tags[2].ti_Tag == TAG_DONE);
        CHECK(observed_tags[0].ti_Data != 0);
        CHECK(observed_tags[1].ti_Data != 0);
    }
    if (call == fail_open_call)
        return 1;
    if (call == 1)
        timer_open = 1;
    else
        device_open = 1;
    return 0;
}

void CloseDevice(APTR request)
{
    ++close_calls;
    if (request == (APTR)device_request)
    {
        CHECK(device_open);
        CHECK(!request_sent || request_waited);
        device_open = 0;
        record_event(EVENT_CLOSE_DEVICE);
    }
    else
    {
        CHECK(request == (APTR)timer_request);
        CHECK(timer_open);
        CHECK(!timer_sent || timer_waited);
        timer_open = 0;
        record_event(EVENT_CLOSE_TIMER);
    }
}

void SendIO(APTR request)
{
    if (request == (APTR)timer_request)
    {
        CHECK(timer_open);
        CHECK(!timer_sent);
        CHECK(timer_request->tr_node.io_Command == TR_ADDREQUEST);
        CHECK(timer_request->tr_time.tv_secs == 2);
        CHECK(timer_request->tr_time.tv_micro == 0);
        timer_sent = 1;
        ++timer_send_calls;
        return;
    }

    CHECK(request == (APTR)device_request);
    CHECK(device_open);
    CHECK(!request_sent);
    {
        struct IOStdReq *standard = (struct IOStdReq *)request;
        struct NSDeviceQueryResult *out;
        CHECK(standard->io_Command == NSCMD_DEVICEQUERY);
        CHECK(standard->io_Command != S2_GETNETWORKS);
        CHECK(standard->io_Command != S2_ONLINE);
        CHECK(standard->io_Length == sizeof(struct NSDeviceQueryResult));
        out = (struct NSDeviceQueryResult *)standard->io_Data;
        CHECK(out != NULL);
        *out = response_query;
        standard->io_Actual = response_actual;
        device_request->ios2_Req.io_Error = response_error;
    }
    request_sent = 1;
    ++device_send_calls;
}

BOOL CheckIO(APTR request)
{
    if (request == (APTR)device_request)
    {
        CHECK(request_sent);
        return request_complete ? TRUE : FALSE;
    }
    CHECK(request == (APTR)timer_request);
    CHECK(timer_sent);
    return timer_complete ? TRUE : FALSE;
}

void Wait(ULONG signal_mask)
{
    ULONG expected;
    CHECK(!wait_calls);
    CHECK(port_slots[0] != NULL && port_slots[1] != NULL);
    expected = (1UL << port_slots[0]->mp_SigBit)
        | (1UL << port_slots[1]->mp_SigBit);
    CHECK(signal_mask == expected);
    ++wait_calls;
    record_event(EVENT_WAIT);
    if (completion_mode == COMPLETE_REQUEST || completion_mode == COMPLETE_BOTH)
        request_complete = 1;
    if (completion_mode == COMPLETE_TIMER || completion_mode == COMPLETE_BOTH)
        timer_complete = 1;
}

void AbortIO(APTR request)
{
    if (request == (APTR)device_request)
    {
        CHECK(request_sent);
        CHECK(!request_waited);
        CHECK(!request_aborted);
        request_aborted = 1;
        record_event(EVENT_ABORT_REQUEST);
        return;
    }
    CHECK(request == (APTR)timer_request);
    CHECK(timer_sent);
    CHECK(!timer_waited);
    CHECK(!timer_aborted);
    timer_aborted = 1;
    record_event(EVENT_ABORT_TIMER);
}

void WaitIO(APTR request)
{
    if (request == (APTR)device_request)
    {
        CHECK(request_sent);
        CHECK(!request_waited);
        CHECK(request_complete || request_aborted);
        request_complete = 1;
        request_waited = 1;
        record_event(EVENT_WAIT_REQUEST);
        return;
    }
    CHECK(request == (APTR)timer_request);
    CHECK(timer_sent);
    CHECK(!timer_waited);
    CHECK(timer_complete || timer_aborted);
    timer_complete = 1;
    timer_waited = 1;
    record_event(EVENT_WAIT_TIMER);
}

static void test_query_kind_metadata_and_bounds(void)
{
    struct NSDeviceQueryResult query;
    UWORD commands[256];
    ULONG required = (ULONG)(offsetof(struct NSDeviceQueryResult, SupportedCommands)
        + sizeof(query.SupportedCommands));
    ULONG i;

    memset(commands, 0, sizeof(commands));
    memset(&query, 0, sizeof(query));
    query.DevQueryFormat = 0;
    query.SizeAvailable = required;
    query.DeviceType = NSDEVTYPE_SANA2;
    query.SupportedCommands = commands;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_NO);
    CHECK(query_kind(&query, sizeof(query)) == NETPREFS_WIRELESS_NO);

    CHECK(query_kind(&query, required - 1) == NETPREFS_WIRELESS_UNKNOWN);
    CHECK(query_kind(&query, sizeof(query) + 1) == NETPREFS_WIRELESS_UNKNOWN);
    query.SizeAvailable = required - 1;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    query.SizeAvailable = required + 1;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    query.SizeAvailable = required;
    query.DevQueryFormat = 1;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    query.DevQueryFormat = 0;
    query.DeviceType = NSDEVTYPE_SANA2 + 1;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    query.DeviceType = NSDEVTYPE_SANA2;
    query.SupportedCommands = NULL;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    query.SupportedCommands = commands;

    commands[0] = S2_GETNETWORKS;
    commands[1] = 0;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_YES);
    commands[0] = 0x1234;
    commands[1] = 0;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_NO);

    for (i = 0; i < 256; ++i)
        commands[i] = 0x1234;
    commands[254] = S2_GETNETWORKS;
    commands[255] = 0;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_YES);
    commands[254] = 0x1234;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_NO);

    for (i = 0; i < 256; ++i)
        commands[i] = 0x1234;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
    commands[255] = S2_GETNETWORKS;
    CHECK(query_kind(&query, required) == NETPREFS_WIRELESS_UNKNOWN);
}

static void test_invalid_inputs_short_circuit(void)
{
    reset_mocks();
    CHECK(NetPrefs_QueryWireless(NULL, 0) == NETPREFS_WIRELESS_UNKNOWN);
    CHECK(NetPrefs_QueryWireless("", 0) == NETPREFS_WIRELESS_UNKNOWN);
    CHECK(NetPrefs_QueryWireless("wifi.device", -1) == NETPREFS_WIRELESS_UNKNOWN);
    CHECK(port_calls == 0);
    CHECK(io_calls == 0);
    CHECK(open_calls == 0);
    assert_clean();
}

static void test_each_setup_failure(void)
{
    int i;
    for (i = 1; i <= 6; ++i)
    {
        reset_mocks();
        if (i <= 2)
            fail_port_call = i;
        else if (i <= 4)
            fail_io_call = i - 2;
        else
            fail_open_call = i - 4;
        CHECK(NetPrefs_QueryWireless("wifi.device", 3) == NETPREFS_WIRELESS_UNKNOWN);
        CHECK(port_calls == (i <= 2 ? 2 : 2));
        CHECK(io_calls == (i <= 2 ? 0 : 2));
        CHECK(open_calls == (i <= 4 ? 0 : i - 4));
        if (i == 5)
            CHECK(close_calls == 0);
        else if (i == 6)
            CHECK(close_calls == 1);
        else
            CHECK(close_calls == 0);
        assert_clean();
    }
}

static void test_success_result(const UWORD *commands, ULONG size_available,
                                ULONG format, UWORD device_type, BYTE error,
                                enum CompletionMode completion,
                                enum NetPrefsWirelessKind expected)
{
    enum NetPrefsWirelessKind result;
    reset_mocks();
    prepare_response(commands, size_available, format, device_type, error);
    completion_mode = completion;
    result = NetPrefs_QueryWireless("wifi.device", 3);
    CHECK(result == expected);
    CHECK(open_calls == 2);
    CHECK(close_calls == 2);
    CHECK(device_send_calls == 1);
    CHECK(timer_send_calls == 1);
    CHECK(observed_tags != NULL);
    assert_clean();
}

static void test_timeout_cleanup_order(void)
{
    UWORD commands[] = { S2_GETNETWORKS, 0 };
    size_t abort_request;
    size_t wait_request;
    size_t close_device;
    size_t delete_request;
    reset_mocks();
    prepare_response(commands, sizeof(struct NSDeviceQueryResult), 0,
                     NSDEVTYPE_SANA2, 0);
    completion_mode = COMPLETE_TIMER;
    CHECK(NetPrefs_QueryWireless("wifi.device", 3) == NETPREFS_WIRELESS_UNKNOWN);
    CHECK(wait_calls == 1);
    CHECK(request_aborted);
    CHECK(request_waited);
    CHECK(timer_aborted);
    CHECK(timer_waited);
    abort_request = event_index(EVENT_ABORT_REQUEST);
    wait_request = event_index(EVENT_WAIT_REQUEST);
    close_device = event_index(EVENT_CLOSE_DEVICE);
    delete_request = event_index(EVENT_DELETE_REQUEST);
    CHECK(abort_request < wait_request);
    CHECK(wait_request < close_device);
    CHECK(close_device < delete_request);
    CHECK(event_index(EVENT_WAIT_TIMER) < event_index(EVENT_CLOSE_TIMER));
    assert_clean();
}

static void test_simultaneous_completion(void)
{
    UWORD commands[] = { 0x1234, S2_GETNETWORKS, 0 };
    reset_mocks();
    prepare_response(commands, sizeof(struct NSDeviceQueryResult), 0,
                     NSDEVTYPE_SANA2, 0);
    completion_mode = COMPLETE_BOTH;
    CHECK(NetPrefs_QueryWireless("wifi.device", 3) == NETPREFS_WIRELESS_YES);
    CHECK(request_complete);
    CHECK(timer_complete);
    CHECK(!request_aborted);
    CHECK(timer_aborted);
    CHECK(request_waited);
    CHECK(timer_waited);
    CHECK(event_index(EVENT_WAIT_REQUEST) < event_index(EVENT_CLOSE_DEVICE));
    CHECK(event_index(EVENT_WAIT_TIMER) < event_index(EVENT_CLOSE_TIMER));
    assert_clean();
}

struct TestSelectionInterface {
    STRPTR device;
    LONG unit;
    BOOL tunnel;
};

struct SelectionResult {
    STRPTR device;
    LONG unit;
};

static struct TestSelectionInterface selection_interface;

static struct TestSelectionInterface *GetInterface(LONG index)
{
    CHECK(index == 0);
    return &selection_interface;
}

static BOOL GetIsTunnel(struct TestSelectionInterface *iface)
{
    return iface->tunnel;
}

static STRPTR GetDevice(struct TestSelectionInterface *iface)
{
    return iface->device;
}

static LONG GetUnit(struct TestSelectionInterface *iface)
{
    return iface->unit;
}

static int Stricmp(CONST_STRPTR left, CONST_STRPTR right)
{
    unsigned char a;
    unsigned char b;
    do
    {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (tolower(a) != tolower(b))
            return tolower(a) - tolower(b);
    } while (a != 0 && b != 0);
    return 0;
}

static struct SelectionResult execute_actual_selection_loop(
    STRPTR device, LONG unit, BOOL tunnel,
    STRPTR previous_device, LONG previous_unit)
{
    struct TestSelectionInterface *iface = NULL;
    LONG interfacecount = 1;
    LONG i;
    STRPTR previousWirelessDevice = previous_device;
    LONG previousWirelessUnit = previous_unit;
    STRPTR wirelessDevice = NULL;
    LONG wirelessUnit = 0;
    struct SelectionResult result;

    selection_interface.device = device;
    selection_interface.unit = unit;
    selection_interface.tunnel = tunnel;
    /* SELECTION_LOOP */
    result.device = wirelessDevice;
    result.unit = wirelessUnit;
    return result;
}

static struct SelectionResult run_selection_case(
    STRPTR device, LONG unit, BOOL tunnel,
    STRPTR previous_device, LONG previous_unit,
    enum NetPrefsWirelessKind response_kind)
{
    UWORD wire[] = { 0x1234, 0 };
    UWORD wireless[] = { S2_GETNETWORKS, 0 };
    struct SelectionResult result;

    reset_mocks();
    expected_device_name = device;
    expected_device_unit = (ULONG)unit;
    if (response_kind == NETPREFS_WIRELESS_YES)
        prepare_response(wireless, sizeof(struct NSDeviceQueryResult), 0,
                         NSDEVTYPE_SANA2, 0);
    else if (response_kind == NETPREFS_WIRELESS_NO)
        prepare_response(wire, sizeof(struct NSDeviceQueryResult), 0,
                         NSDEVTYPE_SANA2, 0);
    else
        prepare_response(wireless, sizeof(struct NSDeviceQueryResult), 1,
                         NSDEVTYPE_SANA2, 0);

    result = execute_actual_selection_loop(device, unit, tunnel,
                                           previous_device, previous_unit);
    if (tunnel)
    {
        CHECK(open_calls == 0);
        CHECK(device_send_calls == 0);
    }
    else
    {
        CHECK(open_calls == 2);
        CHECK(device_send_calls == 1);
    }
    assert_clean();
    return result;
}

static void test_actual_selection_loop(void)
{
    struct SelectionResult result;

    result = run_selection_case("new.device", 1, FALSE, NULL, 0,
                                NETPREFS_WIRELESS_UNKNOWN);
    CHECK(result.device == NULL);

    result = run_selection_case("WIFI.DEVICE", 2, FALSE, "wifi.device", 2,
                                NETPREFS_WIRELESS_UNKNOWN);
    CHECK(result.device != NULL);
    CHECK(strcmp(result.device, "WIFI.DEVICE") == 0);
    CHECK(result.unit == 2);

    result = run_selection_case("wifi.device", 3, FALSE, "wifi.device", 2,
                                NETPREFS_WIRELESS_UNKNOWN);
    CHECK(result.device == NULL);

    result = run_selection_case("wired.device", 4, FALSE, "wired.device", 4,
                                NETPREFS_WIRELESS_NO);
    CHECK(result.device == NULL);

    result = run_selection_case("new.wireless", 7, FALSE, "old.device", 1,
                                NETPREFS_WIRELESS_YES);
    CHECK(result.device != NULL);
    CHECK(strcmp(result.device, "new.wireless") == 0);
    CHECK(result.unit == 7);

    result = run_selection_case("tunnel.device", 9, TRUE, NULL, 0,
                                NETPREFS_WIRELESS_YES);
    CHECK(result.device == NULL);
}

static size_t fixture_strlcpy(char *dst, const char *src, size_t size)
{
    size_t source_length = strlen(src);
    size_t copied = source_length < size - (size != 0)
        ? source_length : size - (size != 0);
    if (size != 0)
    {
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return source_length;
}

#undef strlcpy
#define strlcpy fixture_strlcpy
/* ACTUAL_SETTER_FUNCTIONS */
#undef strlcpy

static STRPTR GetWirelessDevice(void)
{
    return prefs.wirelessDevice;
}

static LONG GetWirelessUnit(void)
{
    return prefs.wirelessUnit;
}

static void CombinePath2P(STRPTR dstbuffer, ULONG dstbufferlen,
                          CONST_STRPTR part1, CONST_STRPTR part2)
{
    int written = snprintf(dstbuffer, dstbufferlen, "%s/%s", part1, part2);
    CHECK(written >= 0);
    CHECK((ULONG)written < dstbufferlen);
}

static void write_wireless_record(FILE *ConfFile)
{
    /* ACTUAL_WRITER_STATEMENT */
}

static void load_wireless_record(CONST_STRPTR directory)
{
    ULONG filenamelen = (ULONG)(strlen(directory) + 4 + 20);
    TEXT filename[filenamelen];
    /* ACTUAL_LOADER_BLOCK */
}

static void selection_record_path(STRPTR path, ULONG path_size,
                                  CONST_STRPTR directory)
{
    CombinePath2P(path, path_size, directory, "WirelessDevice");
}

static void save_wireless_record(CONST_STRPTR directory, CONST_STRPTR contents)
{
    char path[512];
    FILE *file;
    selection_record_path(path, sizeof(path), directory);
    file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    CHECK(fclose(file) == 0);
}

static void remove_wireless_record(CONST_STRPTR directory)
{
    char path[512];
    selection_record_path(path, sizeof(path), directory);
    (void)unlink(path);
}

static void expect_loaded_wireless_record(CONST_STRPTR directory,
                                          CONST_STRPTR contents,
                                          CONST_STRPTR device,
                                          LONG unit)
{
    if (contents == NULL)
        remove_wireless_record(directory);
    else
        save_wireless_record(directory, contents);
    SetWirelessDevice("stale.device");
    SetWirelessUnit(99);
    load_wireless_record(directory);
    if (device == NULL)
    {
        CHECK(GetWirelessDevice() == NULL);
        CHECK(GetWirelessUnit() == 0);
    }
    else
    {
        CHECK(GetWirelessDevice() != NULL);
        CHECK(strcmp(GetWirelessDevice(), device) == 0);
        CHECK(GetWirelessUnit() == unit);
    }
}

static void test_setter_owns_selection(void)
{
    char edited_interface_device[] = "wireless.device";
    char long_device[NAMEBUFLEN + 16];

    SetWirelessDevice(edited_interface_device);
    CHECK(GetWirelessDevice() == wirelessDeviceName);
    CHECK(GetWirelessDevice() != edited_interface_device);
    CHECK(strcmp(GetWirelessDevice(), "wireless.device") == 0);
    edited_interface_device[0] = 'x';
    CHECK(strcmp(GetWirelessDevice(), "wireless.device") == 0);

    memset(long_device, 'x', sizeof(long_device));
    long_device[sizeof(long_device) - 1] = '\0';
    SetWirelessDevice(long_device);
    CHECK(strlen(GetWirelessDevice()) == NAMEBUFLEN - 1);
    CHECK(wirelessDeviceName[NAMEBUFLEN - 1] == '\0');
    SetWirelessUnit(17);
    CHECK(GetWirelessUnit() == 17);

    SetWirelessDevice(NULL);
    CHECK(GetWirelessDevice() == NULL);
    CHECK(wirelessDeviceName[0] == '\0');
}

static void test_loader_and_writer(CONST_STRPTR directory)
{
    char path[512];
    char contents[256];
    FILE *file;

    expect_loaded_wireless_record(directory, NULL, NULL, 0);
    expect_loaded_wireless_record(directory, "", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device OTHER 7", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device unit 7", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT 7 extra", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT -1", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT 2147483648", NULL, 0);
    expect_loaded_wireless_record(directory,
        "wifi.device UNIT 99999999999999999999", NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT 7x", NULL, 0);
    expect_loaded_wireless_record(directory,
        "wifi.device UNIT 999999999999999999999999999999999999999999",
        NULL, 0);
    expect_loaded_wireless_record(directory, "wifi.device UNIT 7",
                                  "wifi.device", 7);
    expect_loaded_wireless_record(directory, "wifi.device UNIT 2147483647",
                                  "wifi.device", 2147483647L);

    SetWirelessDevice("wifi.device");
    SetWirelessUnit(7);
    selection_record_path(path, sizeof(path), directory);
    file = fopen(path, "w");
    CHECK(file != NULL);
    write_wireless_record(file);
    CHECK(fclose(file) == 0);
    file = fopen(path, "r");
    CHECK(file != NULL);
    CHECK(fgets(contents, sizeof(contents), file) != NULL);
    CHECK(fclose(file) == 0);
    CHECK(strcmp(contents, "wifi.device UNIT 7") == 0);
    load_wireless_record(directory);
    CHECK(strcmp(GetWirelessDevice(), "wifi.device") == 0);
    CHECK(GetWirelessUnit() == 7);
    remove_wireless_record(directory);
}

int main(int argc, char **argv)
{
    UWORD wire[] = { 0x1234, 0 };
    UWORD wireless[] = { S2_GETNETWORKS, 0 };
    UWORD unterminated[256];
    ULONG i;

    test_query_kind_metadata_and_bounds();
    test_invalid_inputs_short_circuit();
    test_each_setup_failure();

    test_success_result(wire, sizeof(struct NSDeviceQueryResult), 0,
                        NSDEVTYPE_SANA2, 0, COMPLETE_REQUEST,
                        NETPREFS_WIRELESS_NO);
    test_success_result(wireless, sizeof(struct NSDeviceQueryResult), 0,
                        NSDEVTYPE_SANA2, 0, COMPLETE_REQUEST,
                        NETPREFS_WIRELESS_YES);
    for (i = 0; i < 256; ++i)
        unterminated[i] = 0x1234;
    test_success_result(unterminated, sizeof(struct NSDeviceQueryResult), 0,
                        NSDEVTYPE_SANA2, 0, COMPLETE_REQUEST,
                        NETPREFS_WIRELESS_UNKNOWN);
    test_success_result(wireless, sizeof(struct NSDeviceQueryResult), 1,
                        NSDEVTYPE_SANA2, 0, COMPLETE_REQUEST,
                        NETPREFS_WIRELESS_UNKNOWN);
    test_success_result(wireless, sizeof(struct NSDeviceQueryResult), 0,
                        NSDEVTYPE_SANA2, 5, COMPLETE_REQUEST,
                        NETPREFS_WIRELESS_UNKNOWN);
    test_timeout_cleanup_order();
    test_simultaneous_completion();
    test_actual_selection_loop();
    test_setter_owns_selection();
    CHECK(argc == 2);
    test_loader_and_writer(argv[1]);

    puts("host fixture passed");
    return 0;
}
"""


def assert_source_requests_only_query_and_timer() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    assigned_commands = re.findall(
        r"\b(?:io_Command|ios2_Command)\s*=\s*([A-Za-z_]\w*)", source
    )
    expected = {"NSCMD_DEVICEQUERY", "TR_ADDREQUEST"}
    if len(assigned_commands) != 2 or set(assigned_commands) != expected:
        raise AssertionError(
            "Unexpected request command assignments in wireless_device.c: "
            + repr(assigned_commands)
        )
    if any(command in {"S2_GETNETWORKS", "S2_ONLINE"} for command in assigned_commands):
        raise AssertionError("The query must not issue scan or ONLINE commands")


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"Could not locate {signature!r} in prefsdata.c")
    opening = source.find("{", start)
    if opening < 0:
        raise AssertionError(f"Could not locate body for {signature!r}")
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start : offset + 1]
    raise AssertionError(f"Unterminated body for {signature!r}")


def extract_selection_loop(source: str) -> str:
    anchor = source.find("/* Resolve before opening/truncating output files.")
    marker = "    for (i = 0; i < interfacecount; ++i)\n    {\n        enum NetPrefsWirelessKind kind;"
    start = source.find(marker, anchor)
    if anchor < 0 or start < 0:
        raise AssertionError("Could not locate the wireless candidate selection loop")
    end_marker = "\n    }\n\n    CombinePath2P(destdbdir"
    end = source.find(end_marker, start)
    if end < 0:
        raise AssertionError("Could not locate the end of the wireless selection loop")
    return source[start : end + len("\n    }")]


def extract_loader_block(source: str) -> str:
    anchor = source.find("/* WirelessDevice is a scalar written alongside db/.")
    opening = source.find("    {", anchor)
    end_marker = "\n    }\n\n    /* This function will not fail."
    end = source.find(end_marker, opening)
    if anchor < 0 or opening < 0 or end < 0:
        raise AssertionError("Could not locate the wireless scalar loader block")
    return source[opening : end + len("\n    }")]


def extract_writer_statement(source: str) -> str:
    start_marker = '        fprintf(ConfFile, "%s UNIT %ld", GetWirelessDevice(),'
    end_marker = "(long int)GetWirelessUnit());"
    start = source.find(start_marker)
    end = source.find(end_marker, start)
    if start < 0 or end < 0:
        raise AssertionError("Could not locate the WirelessDevice writer statement")
    return source[start : end + len(end_marker)]


def write_fixture(root: Path) -> None:
    for relative, contents in FIXTURE_HEADERS.items():
        target = root / "include" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents, encoding="utf-8")
    source = PREFSDATA_SOURCE.read_text(encoding="utf-8")
    replacements = {
        "/* SELECTION_LOOP */": extract_selection_loop(source),
        "/* ACTUAL_SETTER_FUNCTIONS */": "\n\n".join(
            (
                extract_function(source, "void SetWirelessDevice(STRPTR w)"),
                extract_function(source, "void SetWirelessUnit(LONG w)"),
            )
        ),
        "/* ACTUAL_LOADER_BLOCK */": extract_loader_block(source),
        "/* ACTUAL_WRITER_STATEMENT */": extract_writer_statement(source),
    }
    harness = HARNESS
    for marker, replacement in replacements.items():
        harness = harness.replace(marker, replacement)
    if any(marker in harness for marker in replacements):
        raise AssertionError("A prefsdata.c fragment was not inserted into the fixture")
    (root / "test_wireless_device.c").write_text(harness, encoding="utf-8")


def compile_and_run(compiler: list[str], root: Path, optimization: str,
                    sanitized: bool) -> None:
    executable = root / (
        "test-" + optimization[1:] + ("-san" if sanitized else "")
    )
    command = [
        *compiler,
        "-std=c11",
        optimization,
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fno-omit-frame-pointer",
        "-I",
        str(root / "include"),
        "-I",
        str(NETWORK_DIR),
        str(root / "test_wireless_device.c"),
        "-o",
        str(executable),
    ]
    if sanitized:
        command.insert(1, "-fsanitize=address,undefined")
    built = subprocess.run(command, text=True, capture_output=True)
    if built.returncode:
        raise RuntimeError(
            "Compile failed for "
            + " ".join(command)
            + "\n"
            + built.stdout
            + built.stderr
        )
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    ran = subprocess.run(
        [str(executable), str(root)], text=True, capture_output=True, env=env
    )
    if ran.returncode:
        raise RuntimeError(
            f"Host test exited {ran.returncode} for {optimization}"
            + (" with ASan/UBSan" if sanitized else "")
            + "\n"
            + ran.stdout
            + ran.stderr
        )
    print(
        f"PASS {optimization}"
        + (" ASan/UBSan" if sanitized else "")
        + ": "
        + ran.stdout.strip()
    )


def main() -> int:
    if not SOURCE.is_file():
        raise FileNotFoundError(SOURCE)
    assert_source_requests_only_query_and_timer()
    cc = shutil.which("cc")
    if cc is None:
        raise RuntimeError("cc is required to run the host tests")
    compiler = shlex.split(cc)
    with tempfile.TemporaryDirectory(prefix="aros-wireless-device-test-") as temp:
        root = Path(temp)
        write_fixture(root)
        for optimization in ("-O0", "-O2"):
            compile_and_run(compiler, root, optimization, sanitized=False)
            compile_and_run(compiler, root, optimization, sanitized=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
