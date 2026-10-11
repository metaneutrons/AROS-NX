/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_SANA_DEVICE_H
#define P4_SANA_DEVICE_H

#include <exec/devices.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <devices/sana2.h>
#include <devices/sana2wireless.h>
#include <utility/hooks.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include "runtime_probe.h"

#define P4WLAN_MAX_FRAME          1518u
#define P4WLAN_ETH_HEADER         14u
#define P4WLAN_MTU                1500u
#define P4WLAN_MAX_OPENERS        8u
#define P4WLAN_MAX_REQUESTS       32u
#define P4WLAN_MAX_READS          24u
#define P4WLAN_MAX_WRITES         8u
#define P4WLAN_MAX_EVENTS         16u

typedef BOOL (*p4wlan_copy_fn)(APTR, APTR, ULONG);

enum p4wlan_request_kind
{
    P4WLAN_REQUEST_READ = 1,
    P4WLAN_REQUEST_WRITE,
    P4WLAN_REQUEST_EVENT,
    P4WLAN_REQUEST_SCAN
};

enum p4wlan_request_state
{
    P4WLAN_REQUEST_QUEUED = 1,
    P4WLAN_REQUEST_ACTIVE,
    P4WLAN_REQUEST_COMPLETING
};

/* Every asynchronous IO has an explicit descriptor state.  NT_MESSAGE is
 * maintained for Exec's IO conventions, never used to infer list ownership. */
struct p4wlan_request
{
    struct MinNode owner_node;
    struct MinNode write_node;
    struct MinNode done_node;
    struct p4wlan_opener *opener;
    struct IOSana2Req *request;
    ULONG kind;
    ULONG state;
    ULONG capacity;
    ULONG event_mask;
    ULONG packet_type;
    ULONG io_flags;
    ULONG completion_length;
    ULONG completion_wire;
    BYTE completion_error;
    BOOL completion_sets_length;
    BOOL cancelled;
    BOOL tx_committed;
    BOOL scan_filter_set;
    UBYTE scan_filter[33];
    UBYTE destination[6];
    APTR data;
    ULONG offered_frame;
    ULONG link_token;
};

/* Buffer hooks and filter are copied from the OpenDevice tag list.  The list
 * itself is not retained.  Hooks must be synchronous, nonblocking and
 * non-reentrant; in particular they must not CloseDevice from a bridge
 * callback. */
struct p4wlan_opener
{
    struct MinNode unit_node;
    struct MinList requests;
    p4wlan_copy_fn copy_to;
    p4wlan_copy_fn copy_to16;
    p4wlan_copy_fn copy_to32;
    p4wlan_copy_fn copy_from;
    p4wlan_copy_fn copy_from16;
    p4wlan_copy_fn copy_from32;
    struct Hook *filter;
    ULONG refs;
    ULONG request_count;
    ULONG read_count;
    ULONG write_count;
    ULONG event_count;
    struct Task *close_task;
    ULONG close_signal;
    BOOL exclusive;
    BOOL closing;
    BOOL close_waiter;
    ULONG last_rx_frame;
    /* Correlation metadata belongs to this OpenDevice lifetime, not the radio.
     * Cookie non-reuse is the caller's public-contract obligation. */
    ULONG active_cookie;
    ULONG failed_cookie;
    /* Typed terminal delivery is opener-local. Future admission must set this
     * for the current typed selection/link, not capability negotiation, and
     * restore legacy mode when admitting legacy options on this opener. */
    BOOL fullmac_events;
    ULONG terminal_event;
    ULONG terminal_cookie;
};

/* SETOPTIONS is acknowledged before radio work. This allocation owns every
 * input byte and an opener pin, never an IOSana2Req or borrowed tag pointer. */
struct p4wlan_association
{
    struct p4wlan_unit *unit;
    struct p4wlan_opener *opener;
    struct p4_wifi_association_target target;
    ULONG token;
    ULONG cookie;
    BOOL acknowledged;
    BOOL cancelled;
};

/* This unit and its callback table live for the full noexpunge device
 * lifetime.  Its radio registration is made once and intentionally survives
 * the last CloseDevice; Close retires opener-owned requests but never detaches
 * from the resource. */
struct p4wlan_unit
{
    struct SignalSemaphore lock;
    struct MinList openers;
    struct MinList write_queue;
    struct MinList scan_queue;
    struct P4WifiBase *radio;
    struct p4wlan_association *pending_association;
    struct p4wlan_association *active_association;
    struct p4wlan_opener *link_opener;
    struct p4_wifi_link_info link_info;
    ULONG selection_token;
    BOOL link_committed;
    BOOL teardown_pending;
    BOOL teardown_notify;
    struct Sana2DeviceStats stats;
    struct Sana2DeviceQuery query;
    UBYTE mac[6];
    UBYTE tx_frame[P4WLAN_MAX_FRAME];
    /* Only the serialized owner-service scan uses this owned scratch. */
    struct p4_wifi_scan_result scan_result;
    ULONG scan_count;
    ULONG open_count;
    ULONG request_count;
    ULONG read_count;
    ULONG write_count;
    ULONG event_count;
    /* CONNECT/DISCONNECT are coalesced edge observations. Broadcast to all
     * matching parked waiters; with none parked, the first later matching
     * S2_ONEVENT consumes the unit-wide latch. Repeated same-edge transitions
     * coalesce because this is a bitmask, not an event counter. */
    ULONG pending_events;
    ULONG rx_sequence;
    ULONG rx_dropped_frames;
    ULONG rx_unmatched_frames;
    ULONG rx_bytes;
    ULONG tx_bytes;
    ULONG tx_dropped_frames;
    BOOL initialized;
    BOOL attaching;
    BOOL attached;
    BOOL hardware_ready;
    BOOL configured;
    BOOL online;
    BOOL exclusive_open;
    volatile BOOL callback_fault;
};

struct esp32p4wlan_base
{
    struct Device device;
    struct ExecBase *sysbase;
    struct p4wlan_unit unit;
};

#include LC_LIBDEFS_FILE

#endif
