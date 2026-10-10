/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host model for the actual SANA-II event/configuration helpers extracted
 * from sana_device.c. Lists and IOSana2Req fields are small Exec/ABI fakes. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../network/hosted_scan.h"

typedef void *APTR;
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int BOOL;

#define TRUE 1
#define FALSE 0

typedef BOOL (*p4wlan_copy_fn)(APTR, APTR, ULONG);

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE ln_Type;
};

struct MinList {
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

struct List {
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct SignalSemaphore { ULONG unused; };
struct Task { ULONG unused; };
struct Hook { ULONG unused; };
struct P4WifiBase;
struct Sana2DeviceStats { ULONG words[16]; };
struct Sana2DeviceQuery { ULONG words[16]; };

struct IORequest { BYTE io_Error; };
struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;
};

struct p4wlan_unit;
static void p4wlan_flush_all_locked(struct p4wlan_unit *unit,
                                    struct MinList *done, BYTE error,
                                    ULONG wire_error, BOOL flush_events);

static void minlist_init(struct MinList *list)
{
    list->mlh_Head = (struct MinNode *)&list->mlh_Tail;
    list->mlh_Tail = NULL;
    list->mlh_TailPred = (struct MinNode *)list;
}

static void minlist_addtail(struct MinList *list, struct MinNode *node)
{
    node->mln_Succ = (struct MinNode *)&list->mlh_Tail;
    node->mln_Pred = list->mlh_TailPred;
    list->mlh_TailPred->mln_Succ = node;
    list->mlh_TailPred = node;
}

#define NEWLIST(list) minlist_init((struct MinList *)(list))
#define ADDTAIL(list, node) \
    minlist_addtail((struct MinList *)(list), (struct MinNode *)(node))

static ULONG checks;
static ULONG scenarios;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

#include "sana_events_extract.inc"

static ULONG flush_calls;

static void p4wlan_flush_all_locked(struct p4wlan_unit *unit,
                                    struct MinList *done, BYTE error,
                                    ULONG wire_error, BOOL flush_events)
{
    (void)unit;
    (void)done;
    (void)error;
    (void)wire_error;
    (void)flush_events;
    ++flush_calls;
}

struct event_fixture {
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct p4wlan_opener second_opener;
    struct MinList done;
};

static void fixture_init(struct event_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    NEWLIST(&fixture->unit.openers);
    NEWLIST(&fixture->opener.requests);
    NEWLIST(&fixture->second_opener.requests);
    NEWLIST(&fixture->done);
    ADDTAIL(&fixture->unit.openers,
            (struct Node *)&fixture->opener.unit_node);
    ADDTAIL(&fixture->unit.openers,
            (struct Node *)&fixture->second_opener.unit_node);
}

static void park_event_on(struct p4wlan_opener *opener,
                       struct p4wlan_request *item, ULONG event_mask)
{
    memset(item, 0, sizeof(*item));
    item->kind = P4WLAN_REQUEST_EVENT;
    item->state = P4WLAN_REQUEST_QUEUED;
    item->event_mask = event_mask;
    ADDTAIL(&opener->requests, (struct Node *)&item->owner_node);
}

static void park_event(struct event_fixture *fixture,
                       struct p4wlan_request *item, ULONG event_mask)
{
    park_event_on(&fixture->opener, item, event_mask);
}

static void test_supported_masks_and_error_mapping(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;
    const ULONG supported[] = {
        S2EVENT_ONLINE, S2EVENT_OFFLINE, S2EVENT_CONNECT,
        S2EVENT_DISCONNECT,
        S2EVENT_ONLINE | S2EVENT_CONNECT,
        S2EVENT_OFFLINE | S2EVENT_DISCONNECT
    };
    size_t index;

    ++scenarios;
    fixture_init(&fixture);
    for (index = 0; index < sizeof(supported) / sizeof(supported[0]);
         ++index) {
        CHECK(p4wlan_event_mask_valid(supported[index]));
    }

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_event_mask_valid(0));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request, 0));
    CHECK(request.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);
    CHECK(request.ios2_WireError == S2WERR_BAD_EVENT);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_event_mask_valid(0x80000000u));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           0x80000000u));
    CHECK(request.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);
    CHECK(request.ios2_WireError == S2WERR_BAD_EVENT);
}

static void test_edge_latch_and_one_shot_rearm(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;

    ++scenarios;
    fixture_init(&fixture);
    p4wlan_event_locked(&fixture.unit, S2EVENT_CONNECT, &fixture.done);
    CHECK(fixture.unit.pending_events == S2EVENT_CONNECT);

    memset(&request, 0, sizeof(request));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == S2EVENT_CONNECT);
    CHECK(fixture.unit.pending_events == 0);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);

    /* A state query remains administrative and cannot invent a link edge. */
    fixture.unit.online = TRUE;
    memset(&request, 0, sizeof(request));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           S2EVENT_ONLINE));
    CHECK(request.ios2_WireError == S2EVENT_ONLINE);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
}

static void test_new_edge_supersedes_opposite(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;

    ++scenarios;
    fixture_init(&fixture);
    p4wlan_event_locked(&fixture.unit, S2EVENT_CONNECT, &fixture.done);
    p4wlan_event_locked(&fixture.unit, S2EVENT_DISCONNECT, &fixture.done);
    CHECK(fixture.unit.pending_events == S2EVENT_DISCONNECT);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           S2EVENT_DISCONNECT));
    CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(fixture.unit.pending_events == 0);

    p4wlan_event_locked(&fixture.unit, S2EVENT_DISCONNECT, &fixture.done);
    p4wlan_event_locked(&fixture.unit, S2EVENT_CONNECT, &fixture.done);
    CHECK(fixture.unit.pending_events == S2EVENT_CONNECT);
}

static void test_parked_requests_receive_edges_once(void)
{
    struct event_fixture fixture;
    struct p4wlan_request connect_item;
    struct p4wlan_request disconnect_item;
    struct p4wlan_request other_opener_item;

    ++scenarios;
    fixture_init(&fixture);
    park_event(&fixture, &connect_item, S2EVENT_CONNECT | S2EVENT_DISCONNECT);
    park_event(&fixture, &disconnect_item, S2EVENT_DISCONNECT);
    park_event_on(&fixture.second_opener, &other_opener_item,
                  S2EVENT_CONNECT);
    p4wlan_event_locked(&fixture.unit, S2EVENT_CONNECT, &fixture.done);
    CHECK(connect_item.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(connect_item.completion_wire == S2EVENT_CONNECT);
    CHECK(disconnect_item.state == P4WLAN_REQUEST_QUEUED);
    CHECK(other_opener_item.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(other_opener_item.completion_wire == S2EVENT_CONNECT);
    CHECK(fixture.done.mlh_TailPred != (struct MinNode *)&fixture.done);
    CHECK(fixture.unit.pending_events == 0);

    /* No parked listener: the next arm consumes a latched edge. */
    fixture_init(&fixture);
    p4wlan_event_locked(&fixture.unit, S2EVENT_DISCONNECT, &fixture.done);
    CHECK(fixture.unit.pending_events == S2EVENT_DISCONNECT);
    {
        struct IOSana2Req request;
        memset(&request, 0, sizeof(request));
        CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                               S2EVENT_DISCONNECT));
        CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
        CHECK(fixture.unit.pending_events == 0);
    }
}

static void test_configure_admin_online_without_association(void)
{
    struct event_fixture fixture;
    struct p4wlan_request online_item;
    struct p4wlan_request connect_item;
    struct IOSana2Req request;

    ++scenarios;
    fixture_init(&fixture);
    park_event(&fixture, &online_item, S2EVENT_ONLINE);
    park_event(&fixture, &connect_item, S2EVENT_CONNECT);
    p4wlan_configure_interface_locked(&fixture.unit, &fixture.done);

    CHECK(fixture.unit.configured);
    CHECK(fixture.unit.online);
    CHECK(online_item.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(online_item.completion_wire == S2EVENT_ONLINE);
    CHECK(connect_item.state == P4WLAN_REQUEST_QUEUED);
    CHECK(fixture.unit.pending_events == 0);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           S2EVENT_ONLINE));
    CHECK(request.ios2_WireError == S2EVENT_ONLINE);

    /* Repeated ONLINE is idempotent and does not create another edge. */
    p4wlan_online_locked(&fixture.unit, &fixture.done);
    CHECK(fixture.unit.online);
    CHECK(fixture.unit.pending_events == 0);
}

static void test_offline_discards_stale_connect_only(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;

    ++scenarios;
    fixture_init(&fixture);
    fixture.unit.online = TRUE;
    fixture.unit.hardware_ready = TRUE;
    fixture.unit.pending_events = S2EVENT_CONNECT;
    flush_calls = 0;
    p4wlan_set_offline_locked(&fixture.unit, &fixture.done, FALSE);

    CHECK(!fixture.unit.online);
    CHECK(fixture.unit.hardware_ready);
    CHECK(fixture.unit.pending_events == 0);
    CHECK(flush_calls == 1);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &request,
                                           S2EVENT_OFFLINE));
    CHECK(request.ios2_WireError == S2EVENT_OFFLINE);

    /* A prior observed DISCONNECT is not erased or fabricated by admin-off. */
    fixture.unit.pending_events = S2EVENT_DISCONNECT;
    p4wlan_set_offline_locked(&fixture.unit, &fixture.done, TRUE);
    CHECK(fixture.unit.pending_events == S2EVENT_DISCONNECT);
    CHECK(!fixture.unit.hardware_ready);
    CHECK(flush_calls == 2);
}

int main(void)
{
    test_supported_masks_and_error_mapping();
    test_edge_latch_and_one_shot_rearm();
    test_new_edge_supersedes_opposite();
    test_parked_requests_receive_edges_once();
    test_configure_admin_online_without_association();
    test_offline_discards_stale_connect_only();
    printf("SANA-II event model: %lu scenarios, %lu checks\n",
           (unsigned long)scenarios, (unsigned long)checks);
    return 0;
}
