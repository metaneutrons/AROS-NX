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
    ULONG ios2_DataLength;
};

struct p4wlan_unit;

static void FreeVec(APTR memory)
{
    free(memory);
}

static void Signal(struct Task *task, ULONG mask)
{
    (void)task;
    (void)mask;
}

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

static BOOL bytes_zero(const void *memory, size_t size)
{
    const UBYTE *bytes = memory;
    size_t index;
    for (index = 0u; index < size; ++index)
        if (bytes[index] != 0u)
            return FALSE;
    return TRUE;
}

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
    CHECK(p4wlan_event_mask_valid(S2EVENT_CONNECT |
                                  S2EVENT_DISCONNECT |
                                  S2EVENT_CONNECT_FAILED));

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_event_mask_valid(0));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request, 0));
    CHECK(request.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);
    CHECK(request.ios2_WireError == S2WERR_BAD_EVENT);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_event_mask_valid(0x80000000u));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
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
    request.ios2_DataLength = 0xA5A5u;
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
                                           S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == S2EVENT_CONNECT);
    CHECK(request.ios2_DataLength == 0xA5A5u);
    CHECK(fixture.unit.pending_events == 0);

    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);

    /* A state query remains administrative and cannot invent a link edge. */
    fixture.unit.online = TRUE;
    memset(&request, 0, sizeof(request));
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
                                           S2EVENT_ONLINE));
    CHECK(request.ios2_WireError == S2EVENT_ONLINE);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
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
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
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
        CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                               &request,
                                               S2EVENT_DISCONNECT));
        CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
        CHECK(fixture.unit.pending_events == 0);
    }
}

static void test_connect_failed_is_opener_scoped_and_one_shot(void)
{
    struct event_fixture fixture;
    struct p4wlan_request first_waiter, second_waiter, other_waiter;
    struct IOSana2Req request;
    const ULONG combined_mask = S2EVENT_CONNECT | S2EVENT_DISCONNECT |
                                S2EVENT_CONNECT_FAILED;
    const ULONG cookie = 0xF1234567u;

    ++scenarios;
    fixture_init(&fixture);
    fixture.opener.active_cookie = cookie;
    fixture.second_opener.active_cookie = 77u;
    park_event_on(&fixture.opener, &first_waiter, combined_mask);
    park_event_on(&fixture.opener, &second_waiter, S2EVENT_CONNECT_FAILED);
    park_event_on(&fixture.second_opener, &other_waiter, combined_mask);

    p4wlan_association_failed_locked(&fixture.opener, cookie,
                                      &fixture.done);
    CHECK(first_waiter.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(first_waiter.completion_error == 0);
    CHECK(first_waiter.completion_wire == S2EVENT_CONNECT_FAILED);
    CHECK(first_waiter.completion_sets_length == TRUE);
    CHECK(first_waiter.completion_length == cookie);
    CHECK(second_waiter.state == P4WLAN_REQUEST_QUEUED);
    CHECK(other_waiter.state == P4WLAN_REQUEST_QUEUED);
    CHECK(fixture.opener.failed_cookie == 0u);
    CHECK(fixture.second_opener.failed_cookie == 0u);
    CHECK(fixture.done.mlh_TailPred != (struct MinNode *)&fixture.done);

    /* The second parked request and other opener cannot consume this result. */
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit,
                                            &fixture.second_opener, &request,
                                            S2EVENT_CONNECT_FAILED));
    CHECK(request.ios2_WireError == 0u);
    CHECK(request.ios2_DataLength == 0u);
}

static void test_connect_failed_latch_is_standalone_and_consumed_once(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;
    const ULONG cookie = 0xF0000001u;
    const ULONG combined_mask = S2EVENT_CONNECT | S2EVENT_DISCONNECT |
                                S2EVENT_CONNECT_FAILED;

    ++scenarios;
    fixture_init(&fixture);
    fixture.opener.active_cookie = cookie;
    fixture.unit.pending_events = S2EVENT_DISCONNECT;
    p4wlan_association_failed_locked(&fixture.opener, cookie,
                                      &fixture.done);
    CHECK(fixture.opener.active_cookie == 0u);
    CHECK(fixture.opener.failed_cookie == cookie);

    memset(&request, 0, sizeof(request));
    request.ios2_DataLength = 0xDEADBEEFu;
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request, combined_mask));
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(request.ios2_DataLength == cookie);
    CHECK(fixture.opener.failed_cookie == 0u);
    CHECK(fixture.unit.pending_events == S2EVENT_DISCONNECT);

    memset(&request, 0, sizeof(request));
    request.ios2_DataLength = 0xABCDu;
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT_FAILED));
    CHECK(request.ios2_WireError == 0u);
    CHECK(request.ios2_DataLength == 0xABCDu);

    /* The ordinary link edge remains independently available. */
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
                                           S2EVENT_CONNECT | S2EVENT_DISCONNECT));
    CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(request.ios2_DataLength == 0xABCDu);
}

static void test_stale_failure_and_lifecycle_clear(void)
{
    struct event_fixture fixture;
    struct MinList done;

    ++scenarios;
    fixture_init(&fixture);
    fixture.opener.active_cookie = 91u;
    p4wlan_association_failed_locked(&fixture.opener, 90u, &fixture.done);
    CHECK(fixture.opener.active_cookie == 91u);
    CHECK(fixture.opener.failed_cookie == 0u);

    fixture.opener.failed_cookie = 92u;
    fixture.second_opener.active_cookie = 93u;
    fixture.second_opener.failed_cookie = 94u;
    p4wlan_cancel_opener_association_locked(&fixture.unit, &fixture.opener);
    CHECK(fixture.opener.active_cookie == 0u);
    CHECK(fixture.opener.failed_cookie == 0u);
    CHECK(fixture.second_opener.active_cookie == 93u);
    CHECK(fixture.second_opener.failed_cookie == 94u);

    fixture.opener.active_cookie = 95u;
    fixture.opener.failed_cookie = 96u;
    NEWLIST(&done);
    p4wlan_invalidate_selection_locked(&fixture.unit);
    CHECK(fixture.opener.active_cookie == 0u);
    CHECK(fixture.opener.failed_cookie == 0u);
    CHECK(fixture.second_opener.active_cookie == 0u);
    CHECK(fixture.second_opener.failed_cookie == 0u);

    fixture.opener.active_cookie = 97u;
    fixture.opener.failed_cookie = 98u;
    fixture.second_opener.active_cookie = 99u;
    fixture.second_opener.failed_cookie = 100u;
    p4wlan_set_offline_locked(&fixture.unit, &done, FALSE);
    CHECK(fixture.opener.active_cookie == 0u);
    CHECK(fixture.opener.failed_cookie == 0u);
    CHECK(fixture.second_opener.active_cookie == 0u);
    CHECK(fixture.second_opener.failed_cookie == 0u);
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
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
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
    fixture.unit.link_committed = TRUE;
    memset(&fixture.unit.link_info, 0xa5, sizeof(fixture.unit.link_info));
    fixture.unit.pending_events = S2EVENT_CONNECT;
    flush_calls = 0;
    p4wlan_set_offline_locked(&fixture.unit, &fixture.done, FALSE);

    CHECK(!fixture.unit.online);
    CHECK(fixture.unit.hardware_ready);
    CHECK(!fixture.unit.link_committed);
    CHECK(bytes_zero(&fixture.unit.link_info, sizeof(fixture.unit.link_info)));
    CHECK(fixture.unit.selection_token == 1u);
    CHECK(fixture.unit.pending_events == 0);
    CHECK(flush_calls == 1);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request,
                                            S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == 0);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                           &request,
                                           S2EVENT_OFFLINE));
    CHECK(request.ios2_WireError == S2EVENT_OFFLINE);

    /* A prior observed DISCONNECT is not erased or fabricated by admin-off. */
    fixture.unit.pending_events = S2EVENT_DISCONNECT;
    p4wlan_set_offline_locked(&fixture.unit, &fixture.done, TRUE);
    CHECK(fixture.unit.pending_events == S2EVENT_DISCONNECT);
    CHECK(!fixture.unit.hardware_ready);
    CHECK(flush_calls == 2);
}

static void test_fullmac_terminal_delivery(void)
{
    struct event_fixture fixture;
    struct p4wlan_request first, second, foreign;
    struct IOSana2Req request;
    ++scenarios;
    fixture_init(&fixture);
    fixture.opener.fullmac_events = TRUE;
    park_event(&fixture, &first, S2EVENT_CONNECT | S2EVENT_OFFLINE);
    park_event(&fixture, &second, S2EVENT_CONNECT);
    park_event_on(&fixture.second_opener, &foreign, S2EVENT_CONNECT);
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    123u, &fixture.done);
    CHECK(first.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(first.completion_wire == S2EVENT_CONNECT);
    CHECK(first.completion_sets_length && first.completion_length == 123u);
    CHECK(second.state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign.state == P4WLAN_REQUEST_QUEUED);
    CHECK(!fixture.opener.terminal_event && !fixture.opener.terminal_cookie);

    /* Legacy broadcast must neither complete a typed waiter nor expose a
     * cookie. Its own independent edge semantics remain unchanged. */
    p4wlan_event_locked(&fixture.unit, S2EVENT_CONNECT, &fixture.done);
    CHECK(second.state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(!foreign.completion_sets_length);
    fixture.unit.pending_events = S2EVENT_CONNECT;
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request, S2EVENT_CONNECT));
    CHECK(fixture.unit.pending_events == S2EVENT_CONNECT);
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit,
              &fixture.second_opener, &request, S2EVENT_CONNECT));
    CHECK(request.ios2_WireError == S2EVENT_CONNECT);

    /* With no matching waiter, latch exactly one edge for its opener. A
     * later disconnect supersedes unconsumed connect, never combines masks. */
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_DISCONNECT,
                                    123u, &fixture.done);
    CHECK(fixture.opener.terminal_event == S2EVENT_DISCONNECT);
    CHECK(fixture.opener.terminal_cookie == 123u);
    fixture.unit.online = FALSE;
    CHECK(p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
              &request, S2EVENT_DISCONNECT | S2EVENT_OFFLINE));
    CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(request.ios2_DataLength == 123u);
    CHECK(!fixture.opener.terminal_event && !fixture.opener.terminal_cookie);
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request, S2EVENT_DISCONNECT));
}

static void test_fullmac_terminal_refusal_and_retirement(void)
{
    struct event_fixture fixture;
    struct IOSana2Req request;
    ULONG invalid[] = {0, S2EVENT_CONNECT | S2EVENT_DISCONNECT,
                       S2EVENT_OFFLINE, S2EVENT_CONNECT_FAILED};
    size_t i;
    ++scenarios;
    fixture_init(&fixture);
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    1u, &fixture.done);
    CHECK(!fixture.opener.terminal_event); /* no typed admission */
    fixture.opener.fullmac_events = TRUE;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        p4wlan_fullmac_terminal_locked(&fixture.opener, invalid[i],
                                        1u, &fixture.done);
        CHECK(!fixture.opener.terminal_event && !fixture.opener.terminal_cookie);
    }
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    0u, &fixture.done);
    CHECK(!fixture.opener.terminal_event);
    fixture.opener.closing = TRUE;
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    1u, &fixture.done);
    CHECK(!fixture.opener.terminal_event);
    fixture.opener.closing = FALSE;
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    1u, &fixture.done);
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_DISCONNECT,
                                    1u, &fixture.done);
    CHECK(fixture.opener.terminal_event == S2EVENT_DISCONNECT);
    p4wlan_cancel_opener_association_locked(&fixture.unit, &fixture.opener);
    CHECK(!fixture.opener.terminal_event && !fixture.opener.terminal_cookie);
    p4wlan_fullmac_terminal_locked(&fixture.opener, S2EVENT_CONNECT,
                                    2u, &fixture.done);
    p4wlan_invalidate_selection_locked(&fixture.unit);
    CHECK(!fixture.opener.terminal_event && !fixture.opener.terminal_cookie);
    memset(&request, 0, sizeof(request));
    CHECK(!p4wlan_onevent_immediate_locked(&fixture.unit, &fixture.opener,
                                            &request, S2EVENT_CONNECT));
}

int main(void)
{
    test_supported_masks_and_error_mapping();
    test_edge_latch_and_one_shot_rearm();
    test_new_edge_supersedes_opposite();
    test_parked_requests_receive_edges_once();
    test_connect_failed_is_opener_scoped_and_one_shot();
    test_connect_failed_latch_is_standalone_and_consumed_once();
    test_stale_failure_and_lifecycle_clear();
    test_configure_admin_online_without_association();
    test_offline_discards_stale_connect_only();
    test_fullmac_terminal_delivery();
    test_fullmac_terminal_refusal_and_retirement();
    printf("SANA-II event model: %lu scenarios, %lu checks\n",
           (unsigned long)scenarios, (unsigned long)checks);
    return 0;
}
