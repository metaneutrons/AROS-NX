/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host tests for actual source-extracted SANA-II RX and TX functions. The
 * Exec list/semaphore, user-copy hooks, and radio are deterministic fakes;
 * these tests do not qualify scheduling or hardware transport. Write-size
 * checks call the extracted validator directly, not BeginIO dispatch. The
 * close-state test is an interleaving model, not a concurrently scheduled
 * CloseDevice test. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../network/hosted_scan.h"

typedef void *APTR;
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef int32_t LONG;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define IOERR_ABORTED (-2)
#define IOERR_BADLENGTH (-3)
#define IOF_QUICK 0x01u
#define NT_MESSAGE 5u
#define MEMF_PUBLIC 0x01u
#define MEMF_CLEAR 0x02u
#define SANA2_MAX_ADDR_BYTES 16u
#define SANA2IOF_RAW 0x80u
#define SANA2IOF_BCAST 0x20u
#define SANA2IOF_MCAST 0x40u
#define S2ERR_MTU_EXCEEDED 11
#define S2ERR_NO_RESOURCES 12
#define S2ERR_TX_FAILURE 13
#define S2WERR_BUFF_ERROR 21u
#define S2WERR_GENERIC_ERROR 22u
#define P4WLAN_MAX_FRAME 1518u
#define P4WLAN_ETH_HEADER 14u
#define P4WLAN_MTU 1500u
#define S2WERR_UNIT_OFFLINE 23u
#define S2ERR_OUTOFSERVICE 14

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

struct SignalSemaphore {
    unsigned depth;
};

struct Task { ULONG unused; };
struct Device { ULONG unused; };
struct Unit { ULONG unused; };
struct ExecBase { ULONG unused; };
struct Sana2DeviceQuery { ULONG words[16]; };
struct Sana2DeviceStats {
    ULONG BadData;
    ULONG PacketsReceived;
    ULONG PacketsSent;
};
struct P4WifiBase;
struct Hook;
struct IOSana2Req;
struct p4wlan_unit;
struct p4wlan_request;

typedef BOOL (*p4wlan_copy_fn)(APTR, APTR, ULONG);
typedef BOOL (*p4wlan_hook_fn)(struct Hook *, struct IOSana2Req *, APTR);

struct Hook {
    p4wlan_hook_fn invoke;
    APTR user_data;
};

struct P4WifiBase {
    BOOL (*send_frame)(struct P4WifiBase *, const UBYTE *, ULONG);
};

struct Message {
    struct Node mn_Node;
    ULONG mn_Length;
};

struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    UBYTE io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};

struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;
    ULONG ios2_DataLength;
    APTR ios2_Data;
    ULONG ios2_PacketType;
    UBYTE ios2_SrcAddr[SANA2_MAX_ADDR_BYTES];
    UBYTE ios2_DstAddr[SANA2_MAX_ADDR_BYTES];
    APTR ios2_BufferManagement;
};

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

static void minlist_remove(struct MinNode *node)
{
    node->mln_Pred->mln_Succ = node->mln_Succ;
    node->mln_Succ->mln_Pred = node->mln_Pred;
}

static unsigned minlist_count(const struct MinList *list)
{
    const struct MinNode *node;
    unsigned count = 0;
    for (node = list->mlh_Head; node->mln_Succ; node = node->mln_Succ)
        ++count;
    return count;
}

#define NEWLIST(list) minlist_init((struct MinList *)(list))
#define ADDTAIL(list, node) \
    minlist_addtail((struct MinList *)(list), (struct MinNode *)(node))
#define REMOVE(node) minlist_remove((struct MinNode *)(node))
#define IsMinListEmpty(list) \
    ((list)->mlh_TailPred == (struct MinNode *)(list))
#define CALLHOOKPKT(hook, object, packet) \
    ((hook)->invoke((hook), (object), (packet)))

static unsigned long checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static APTR AllocVec(IPTR size, ULONG flags);
static void FreeVec(APTR memory);
static void ReplyMsg(struct Message *message);
static void Signal(struct Task *task, ULONG mask);
static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore);
static void ObtainSemaphore(struct SignalSemaphore *semaphore);
static void ReleaseSemaphore(struct SignalSemaphore *semaphore);

#include "sana_datapath_extract.inc"

struct esp32p4wlan_base {
    struct Device device;
    struct p4wlan_unit unit;
};

#define AROS_LH1(result, name, argument, base_type, base_name, vector, lib) \
    result name(argument, base_type base_name)
#define AROS_LHA(type, name, reg) type name
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define A1 0
#define LIBBASETYPEPTR struct esp32p4wlan_base *
#define GM_UNIQUENAME(name) name

#include "sana_datapath_abort_extract.inc"

struct fixture {
    struct esp32p4wlan_base base;
    struct P4WifiBase radio;
    struct p4wlan_opener openers[2];
};

static struct p4wlan_unit *active_unit;
static struct fixture *active_fixture;
static struct IOSana2Req *copy_abort_request;
static struct IOSana2Req *rx_copy_abort_request;
static struct IOSana2Req *send_abort_request;
static struct p4wlan_request *watched_item;
static BOOL copy_failure;
static BOOL copy_to_failure;
static BOOL abort_during_copy;
static BOOL abort_during_rx_copy;
static BOOL abort_during_send;
static BOOL send_result = TRUE;
static LONG copy_abort_result;
static LONG rx_copy_abort_result;
static LONG send_abort_result;
static BOOL committed_seen_in_send;
static BOOL cancelled_seen_in_send;
static unsigned copy_from_calls;
static unsigned copy_to_calls;
static unsigned filter_calls;
static unsigned send_calls;
static unsigned reply_calls;
static unsigned allocation_calls;
static unsigned free_calls;
static ULONG sent_length;
static UBYTE sent_frame[P4WLAN_MAX_FRAME];
static struct IOSana2Req *replies[32];

static APTR AllocVec(IPTR size, ULONG flags)
{
    void *memory = (flags & MEMF_CLEAR) ? calloc(1u, (size_t)size) :
                                           malloc((size_t)size);
    CHECK(memory != NULL);
    ++allocation_calls;
    return memory;
}

static void FreeVec(APTR memory)
{
    CHECK(memory != NULL);
    ++free_calls;
    free(memory);
}

static void ReplyMsg(struct Message *message)
{
    struct IOSana2Req *request = (struct IOSana2Req *)message;
    CHECK(message != NULL);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 0u);
    CHECK(reply_calls < sizeof(replies) / sizeof(replies[0]));
    replies[reply_calls++] = request;
}

static void Signal(struct Task *task, ULONG mask)
{
    CHECK(task != NULL);
    CHECK(mask != 0u);
}

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore != NULL);
    if (semaphore->depth != 0u)
        return FALSE;
    semaphore->depth = 1u;
    return TRUE;
}

static void ObtainSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore != NULL);
    CHECK(semaphore->depth == 0u);
    semaphore->depth = 1u;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore != NULL);
    CHECK(semaphore->depth == 1u);
    semaphore->depth = 0u;
}

static BOOL mock_copy_from(APTR destination, APTR source, ULONG length)
{
    UBYTE *dst = destination;
    const UBYTE *src = source;
    ULONG i;
    CHECK(active_unit->lock.depth == 0u);
    ++copy_from_calls;
    if (!copy_failure) {
        for (i = 0; i < length; ++i)
            dst[i] = src[i];
    }
    if (abort_during_copy) {
        CHECK(copy_abort_request != NULL);
        copy_abort_result = abort_io(
            (struct IORequest *)&copy_abort_request->ios2_Req,
            &active_fixture->base);
    }
    return copy_failure ? FALSE : TRUE;
}

static BOOL mock_copy_to(APTR destination, APTR source, ULONG length)
{
    UBYTE *dst = destination;
    const UBYTE *src = source;
    ULONG i;
    CHECK(active_unit->lock.depth == 0u);
    ++copy_to_calls;
    if (copy_to_failure)
        return FALSE;
    for (i = 0; i < length; ++i)
        dst[i] = src[i];
    if (abort_during_rx_copy) {
        CHECK(rx_copy_abort_request != NULL);
        rx_copy_abort_result = abort_io(
            (struct IORequest *)&rx_copy_abort_request->ios2_Req,
            &active_fixture->base);
    }
    return TRUE;
}

static BOOL mock_send_frame(struct P4WifiBase *radio, const UBYTE *frame,
                            ULONG length)
{
    ULONG i;
    CHECK(active_unit->lock.depth == 0u);
    CHECK(active_fixture != NULL);
    CHECK(radio == &active_fixture->radio);
    CHECK(length <= sizeof(sent_frame));
    ++send_calls;
    sent_length = length;
    for (i = 0; i < length; ++i)
        sent_frame[i] = frame[i];
    if (abort_during_send) {
        CHECK(send_abort_request != NULL);
        CHECK(watched_item != NULL);
        send_abort_result = abort_io(
            (struct IORequest *)&send_abort_request->ios2_Req,
            &active_fixture->base);
        committed_seen_in_send = watched_item->tx_committed;
        cancelled_seen_in_send = watched_item->cancelled;
    }
    return send_result;
}

static BOOL reject_then_accept_filter(struct Hook *hook,
                                      struct IOSana2Req *request,
                                      APTR packet)
{
    (void)hook;
    (void)request;
    (void)packet;
    ++filter_calls;
    return filter_calls > 1u;
}

struct close_interleave {
    struct p4wlan_unit *unit;
    struct p4wlan_opener *opener;
    unsigned calls;
};

static BOOL close_during_filter(struct Hook *hook,
                                struct IOSana2Req *request, APTR packet)
{
    struct close_interleave *interleave = hook->user_data;
    (void)request;
    (void)packet;
    ++interleave->calls;
    /* Deterministic opener-state interleaving after the callback released
     * unit->lock. This does not model a concurrent Close; hooks may not
     * recursively Close. */
    ObtainSemaphore(&interleave->unit->lock);
    interleave->opener->closing = TRUE;
    ReleaseSemaphore(&interleave->unit->lock);
    return TRUE;
}

static void reset_observations(void)
{
    copy_abort_request = NULL;
    rx_copy_abort_request = NULL;
    send_abort_request = NULL;
    watched_item = NULL;
    copy_failure = FALSE;
    copy_to_failure = FALSE;
    abort_during_copy = FALSE;
    abort_during_rx_copy = FALSE;
    abort_during_send = FALSE;
    send_result = TRUE;
    copy_abort_result = -99;
    rx_copy_abort_result = -99;
    send_abort_result = -99;
    committed_seen_in_send = FALSE;
    cancelled_seen_in_send = TRUE;
    copy_from_calls = 0u;
    copy_to_calls = 0u;
    filter_calls = 0u;
    send_calls = 0u;
    reply_calls = 0u;
    allocation_calls = 0u;
    free_calls = 0u;
    sent_length = 0u;
    memset(sent_frame, 0xa5, sizeof(sent_frame));
    memset(replies, 0, sizeof(replies));
}

static void fixture_init(struct fixture *fixture, unsigned opener_count)
{
    unsigned i;
    memset(fixture, 0, sizeof(*fixture));
    minlist_init(&fixture->base.unit.openers);
    minlist_init(&fixture->base.unit.write_queue);
    fixture->base.unit.initialized = TRUE;
    fixture->base.unit.online = TRUE;
    fixture->base.unit.hardware_ready = TRUE;
    fixture->base.unit.radio = &fixture->radio;
    fixture->base.unit.mac[0] = 0x02;
    fixture->base.unit.mac[1] = 0x11;
    fixture->base.unit.mac[2] = 0x22;
    fixture->base.unit.mac[3] = 0x33;
    fixture->base.unit.mac[4] = 0x44;
    fixture->base.unit.mac[5] = 0x55;
    fixture->radio.send_frame = mock_send_frame;
    for (i = 0; i < opener_count; ++i) {
        struct p4wlan_opener *opener = &fixture->openers[i];
        minlist_init(&opener->requests);
        opener->copy_from = mock_copy_from;
        opener->copy_to = mock_copy_to;
        opener->refs = 1u;
        minlist_addtail(&fixture->base.unit.openers, &opener->unit_node);
    }
    active_fixture = fixture;
    active_unit = &fixture->base.unit;
}

static void request_init(struct fixture *fixture,
                         struct p4wlan_opener *opener,
                         struct IOSana2Req *request)
{
    memset(request, 0, sizeof(*request));
    request->ios2_Req.io_Message.mn_Length = sizeof(*request);
    request->ios2_Req.io_Flags = IOF_QUICK;
    request->ios2_Req.io_Device = (struct Device *)&fixture->base;
    request->ios2_Req.io_Unit = (struct Unit *)&fixture->base.unit;
    request->ios2_BufferManagement = opener;
}

static struct p4wlan_request *queue_write(
    struct fixture *fixture, struct p4wlan_opener *opener,
    struct IOSana2Req *request, const UBYTE *destination,
    UBYTE *data, ULONG length, ULONG packet_type, BOOL raw)
{
    struct p4wlan_request *item;
    ULONG validated_length = 0;
    BOOL validated_raw = FALSE;
    request_init(fixture, opener, request);
    request->ios2_Data = data;
    request->ios2_DataLength = length;
    request->ios2_PacketType = packet_type;
    request->ios2_Req.io_Flags |= raw ? SANA2IOF_RAW : 0u;
    CHECK(p4wlan_validate_write(request, &validated_length,
                                &validated_raw));
    CHECK(validated_length == length);
    CHECK(validated_raw == raw);
    item = p4wlan_alloc_request(opener, request, P4WLAN_REQUEST_WRITE);
    CHECK(item != NULL);
    item->capacity = length;
    item->packet_type = packet_type;
    item->io_flags = request->ios2_Req.io_Flags;
    item->data = data;
    if (!raw)
        p4wlan_copy_bytes(item->destination, destination, 6);
    ObtainSemaphore(&fixture->base.unit.lock);
    p4wlan_enqueue_locked(&fixture->base.unit, opener, item);
    ReleaseSemaphore(&fixture->base.unit.lock);
    return item;
}

static struct p4wlan_request *queue_read(
    struct fixture *fixture, struct p4wlan_opener *opener,
    struct IOSana2Req *request, UBYTE *buffer, ULONG capacity,
    ULONG packet_type, UBYTE initial_flags)
{
    struct p4wlan_request *item;
    request_init(fixture, opener, request);
    request->ios2_Data = buffer;
    request->ios2_DataLength = capacity;
    request->ios2_PacketType = packet_type;
    request->ios2_Req.io_Flags |= initial_flags;
    item = p4wlan_alloc_request(opener, request, P4WLAN_REQUEST_READ);
    CHECK(item != NULL);
    item->capacity = capacity;
    item->packet_type = packet_type;
    item->io_flags = request->ios2_Req.io_Flags;
    item->data = buffer;
    ObtainSemaphore(&fixture->base.unit.lock);
    p4wlan_enqueue_locked(&fixture->base.unit, opener, item);
    ReleaseSemaphore(&fixture->base.unit.lock);
    return item;
}

static void check_tx_frame_wiped(const struct p4wlan_unit *unit)
{
    unsigned i;
    for (i = 0; i < sizeof(unit->tx_frame); ++i)
        CHECK(unit->tx_frame[i] == 0u);
}

static void check_fixture_empty(const struct fixture *fixture)
{
    CHECK(IsMinListEmpty(&fixture->base.unit.write_queue));
    CHECK(IsMinListEmpty(&fixture->openers[0].requests));
    CHECK(fixture->openers[0].refs == 1u);
    CHECK(fixture->openers[0].request_count == 0u);
    CHECK(fixture->base.unit.request_count == 0u);
    CHECK(free_calls == allocation_calls);
}

static void test_cooked_tx_frame_contents(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    const UBYTE destination[6] = {0x00, 0x25, 0x96, 0x12, 0x34, 0x56};
    UBYTE payload[5] = {0xde, 0xad, 0xbe, 0xef, 0x42};
    const UBYTE expected[19] = {
        0x00, 0x25, 0x96, 0x12, 0x34, 0x56,
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x08, 0x00, 0xde, 0xad, 0xbe, 0xef, 0x42
    };

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_write(&fixture, &fixture.openers[0], &request, destination,
                      payload, sizeof(payload), 0x0800u, FALSE);
    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(send_calls == 1u);
    CHECK(sent_length == sizeof(expected));
    CHECK(memcmp(sent_frame, expected, sizeof(expected)) == 0);
    CHECK(copy_from_calls == 1u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0u);
    CHECK(fixture.base.unit.stats.PacketsSent == 1u);
    CHECK(fixture.base.unit.tx_bytes == sizeof(expected));
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void test_cooked_tx_exact_mtu_and_rejects_oversize(void)
{
    struct fixture fixture;
    struct IOSana2Req request, invalid;
    const UBYTE destination[6] = {0x00, 0x19, 0x5b, 0x12, 0x34, 0x56};
    UBYTE payload[P4WLAN_MTU];
    UBYTE expected[P4WLAN_ETH_HEADER + P4WLAN_MTU];
    ULONG length = 0;
    BOOL raw = TRUE;
    ULONG i;

    for (i = 0; i < sizeof(payload); ++i)
        payload[i] = (UBYTE)(i * 7u + 3u);
    reset_observations();
    fixture_init(&fixture, 1u);

    request_init(&fixture, &fixture.openers[0], &invalid);
    invalid.ios2_Data = payload;
    invalid.ios2_DataLength = P4WLAN_MTU + 1u;
    invalid.ios2_PacketType = 0x86ddu;
    /* This qualifies p4wlan_validate_write only, not BeginIO routing. */
    CHECK(!p4wlan_validate_write(&invalid, &length, &raw));
    CHECK(length == P4WLAN_MTU + 1u);
    CHECK(raw == FALSE);
    CHECK(fixture.base.unit.request_count == 0u);
    CHECK(reply_calls == 0u);

    (void)queue_write(&fixture, &fixture.openers[0], &request, destination,
                      payload, P4WLAN_MTU, 0x86ddu, FALSE);
    memcpy(expected, destination, 6u);
    memcpy(expected + 6u, fixture.base.unit.mac, 6u);
    expected[12] = 0x86u;
    expected[13] = 0xddu;
    memcpy(expected + P4WLAN_ETH_HEADER, payload, sizeof(payload));

    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(send_calls == 1u);
    CHECK(sent_length == sizeof(expected));
    CHECK(memcmp(sent_frame, expected, sizeof(expected)) == 0);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(fixture.base.unit.stats.PacketsSent == 1u);
    CHECK(fixture.base.unit.tx_bytes == sizeof(expected));
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void test_raw_tx_boundaries_and_validation(void)
{
    struct fixture fixture;
    struct IOSana2Req request_min, request_max, invalid;
    UBYTE raw_data[P4WLAN_MAX_FRAME];
    ULONG length = 0;
    BOOL raw = FALSE;
    ULONG i;
    const ULONG lengths[] = {P4WLAN_ETH_HEADER, P4WLAN_MAX_FRAME};
    struct IOSana2Req *requests[] = {&request_min, &request_max};

    for (i = 0; i < sizeof(raw_data); ++i)
        raw_data[i] = (UBYTE)(i * 13u + 7u);
    reset_observations();
    fixture_init(&fixture, 1u);

    request_init(&fixture, &fixture.openers[0], &invalid);
    invalid.ios2_Data = raw_data;
    invalid.ios2_Req.io_Flags |= SANA2IOF_RAW;
    invalid.ios2_DataLength = P4WLAN_ETH_HEADER - 1u;
    CHECK(!p4wlan_validate_write(&invalid, &length, &raw));
    invalid.ios2_DataLength = P4WLAN_MAX_FRAME + 1u;
    CHECK(!p4wlan_validate_write(&invalid, &length, &raw));
    invalid.ios2_Data = NULL;
    invalid.ios2_DataLength = P4WLAN_ETH_HEADER;
    CHECK(!p4wlan_validate_write(&invalid, &length, &raw));

    for (i = 0; i < 2u; ++i) {
        (void)queue_write(&fixture, &fixture.openers[0], requests[i], NULL,
                          raw_data, lengths[i], 0u, TRUE);
        CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
        CHECK(send_calls == i + 1u);
        CHECK(sent_length == lengths[i]);
        CHECK(memcmp(sent_frame, raw_data, lengths[i]) == 0);
        CHECK(replies[i] == requests[i]);
        CHECK(requests[i]->ios2_Req.io_Error == 0);
        CHECK(free_calls == i + 1u);
        check_tx_frame_wiped(&fixture.base.unit);
    }
    CHECK(fixture.base.unit.stats.PacketsSent == 2u);
    CHECK(fixture.base.unit.tx_bytes == P4WLAN_ETH_HEADER +
                                       P4WLAN_MAX_FRAME);
    check_fixture_empty(&fixture);
}

static void test_tx_copy_failure_completes_without_send(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    const UBYTE destination[6] = {0x02, 1, 2, 3, 4, 5};
    UBYTE payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_observations();
    fixture_init(&fixture, 1u);
    copy_failure = TRUE;
    (void)queue_write(&fixture, &fixture.openers[0], &request, destination,
                      payload, sizeof(payload), 0x86ddu, FALSE);
    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(copy_from_calls == 1u);
    CHECK(send_calls == 0u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(request.ios2_WireError == S2WERR_BUFF_ERROR);
    CHECK(fixture.base.unit.tx_dropped_frames == 1u);
    CHECK(fixture.base.unit.stats.PacketsSent == 0u);
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void test_radio_send_failure_completes_once_and_wipes(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    const UBYTE destination[6] = {0x02, 0x09, 0x08, 0x07, 0x06, 0x05};
    UBYTE payload[17];

    memset(payload, 0x3c, sizeof(payload));
    reset_observations();
    fixture_init(&fixture, 1u);
    send_result = FALSE;
    (void)queue_write(&fixture, &fixture.openers[0], &request, destination,
                      payload, sizeof(payload), 0x0806u, FALSE);

    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(send_calls == 1u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_TX_FAILURE);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(fixture.openers[0].request_count == 0u);
    CHECK(fixture.openers[0].write_count == 0u);
    CHECK(fixture.base.unit.request_count == 0u);
    CHECK(fixture.base.unit.write_count == 0u);
    CHECK(fixture.base.unit.tx_dropped_frames == 1u);
    CHECK(fixture.base.unit.stats.PacketsSent == 0u);
    CHECK(fixture.base.unit.tx_bytes == 0u);
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void test_abort_during_copy_prevents_wire_send(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    const UBYTE destination[6] = {0x02, 9, 8, 7, 6, 5};
    UBYTE payload[11] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21};

    reset_observations();
    fixture_init(&fixture, 1u);
    watched_item = queue_write(&fixture, &fixture.openers[0], &request,
                               destination, payload, sizeof(payload),
                               0x0806u, FALSE);
    copy_abort_request = &request;
    abort_during_copy = TRUE;
    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(copy_abort_result == 0);
    CHECK(send_calls == 0u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(fixture.base.unit.stats.PacketsSent == 0u);
    CHECK(fixture.base.unit.tx_dropped_frames == 0u);
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void test_abort_refuses_committed_tx(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    const UBYTE destination[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    UBYTE payload[4] = {0x10, 0x20, 0x30, 0x40};

    reset_observations();
    fixture_init(&fixture, 1u);
    watched_item = queue_write(&fixture, &fixture.openers[0], &request,
                               destination, payload, sizeof(payload),
                               0x88b5u, FALSE);
    send_abort_request = &request;
    abort_during_send = TRUE;
    CHECK(p4wlan_send_one(&fixture.base.unit, &fixture.radio));
    CHECK(send_calls == 1u);
    CHECK(send_abort_result == -1);
    CHECK(committed_seen_in_send == TRUE);
    CHECK(cancelled_seen_in_send == FALSE);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0u);
    CHECK(fixture.base.unit.stats.PacketsSent == 1u);
    check_tx_frame_wiped(&fixture.base.unit);
    check_fixture_empty(&fixture);
}

static void build_frame(UBYTE *frame, const UBYTE destination[6],
                        const UBYTE source[6], ULONG packet_type,
                        const UBYTE *payload, ULONG payload_length)
{
    ULONG i;
    for (i = 0; i < 6u; ++i) {
        frame[i] = destination[i];
        frame[i + 6u] = source[i];
    }
    frame[12] = (UBYTE)(packet_type >> 8);
    frame[13] = (UBYTE)packet_type;
    for (i = 0; i < payload_length; ++i)
        frame[P4WLAN_ETH_HEADER + i] = payload[i];
}

static void test_rx_broadcast_multicast_and_multiple_openers(void)
{
    struct fixture fixture;
    struct IOSana2Req reqs[4];
    UBYTE buffers[4][32];
    UBYTE frame[P4WLAN_ETH_HEADER + 8u];
    const UBYTE source[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
    const UBYTE broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const UBYTE multicast[6] = {0x01, 0x00, 0x5e, 0x01, 0x02, 0x03};
    const UBYTE payload[8] = {0xde, 0xad, 0xfa, 0xce, 0, 1, 2, 3};
    ULONG i;

    reset_observations();
    fixture_init(&fixture, 2u);
    (void)queue_read(&fixture, &fixture.openers[0], &reqs[0], buffers[0],
                     sizeof(buffers[0]), 0x0800u,
                     SANA2IOF_BCAST | SANA2IOF_MCAST);
    (void)queue_read(&fixture, &fixture.openers[1], &reqs[1], buffers[1],
                     sizeof(buffers[1]), 0x0800u,
                     SANA2IOF_BCAST | SANA2IOF_MCAST);
    build_frame(frame, broadcast, source, 0x0800u, payload, sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 2u);
    CHECK(fixture.base.unit.stats.PacketsReceived == 1u);
    for (i = 0; i < 2u; ++i) {
        CHECK(replies[i] == &reqs[i]);
        CHECK(reqs[i].ios2_Req.io_Flags & SANA2IOF_BCAST);
        CHECK(!(reqs[i].ios2_Req.io_Flags & SANA2IOF_MCAST));
        CHECK(reqs[i].ios2_DataLength == sizeof(payload));
        CHECK(memcmp(buffers[i], payload, sizeof(payload)) == 0);
        CHECK(memcmp(reqs[i].ios2_SrcAddr, source, 6u) == 0);
        CHECK(memcmp(reqs[i].ios2_DstAddr, broadcast, 6u) == 0);
        CHECK(reqs[i].ios2_PacketType == 0x0800u);
    }

    (void)queue_read(&fixture, &fixture.openers[0], &reqs[2], buffers[2],
                     sizeof(buffers[2]), 0x0800u,
                     SANA2IOF_BCAST | SANA2IOF_MCAST);
    (void)queue_read(&fixture, &fixture.openers[1], &reqs[3], buffers[3],
                     sizeof(buffers[3]), 0x0800u,
                     SANA2IOF_BCAST | SANA2IOF_MCAST);
    build_frame(frame, multicast, source, 0x0800u, payload, sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 4u);
    CHECK(fixture.base.unit.stats.PacketsReceived == 2u);
    for (i = 2u; i < 4u; ++i) {
        CHECK(replies[i] == &reqs[i]);
        CHECK(!(reqs[i].ios2_Req.io_Flags & SANA2IOF_BCAST));
        CHECK(reqs[i].ios2_Req.io_Flags & SANA2IOF_MCAST);
        CHECK(memcmp(buffers[i], payload, sizeof(payload)) == 0);
        CHECK(memcmp(reqs[i].ios2_SrcAddr, source, 6u) == 0);
        CHECK(memcmp(reqs[i].ios2_DstAddr, multicast, 6u) == 0);
    }
    CHECK(fixture.base.unit.rx_unmatched_frames == 0u);
    CHECK(fixture.base.unit.rx_dropped_frames == 0u);
    CHECK(free_calls == 4u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(minlist_count(&fixture.openers[1].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(fixture.openers[1].refs == 1u);
}

static void test_rx_filter_rejects_then_advances_to_next_frame(void)
{
    struct fixture fixture;
    struct IOSana2Req rejected, fallback;
    struct Hook filter;
    UBYTE buffers[2][16];
    UBYTE frame[P4WLAN_ETH_HEADER + 4u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[4] = {1, 2, 3, 4};

    reset_observations();
    fixture_init(&fixture, 2u);
    (void)queue_read(&fixture, &fixture.openers[0], &rejected, buffers[0],
                     sizeof(buffers[0]), 0x0806u, 0u);
    (void)queue_read(&fixture, &fixture.openers[1], &fallback, buffers[1],
                     sizeof(buffers[1]), 0x0806u, 0u);
    filter.invoke = reject_then_accept_filter;
    filter.user_data = NULL;
    fixture.openers[0].filter = &filter;

    build_frame(frame, destination, source, 0x0806u, payload, sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(filter_calls == 1u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &fallback);
    CHECK(fallback.ios2_Req.io_Error == 0);
    CHECK(fallback.ios2_DataLength == sizeof(payload));
    CHECK(memcmp(buffers[1], payload, sizeof(payload)) == 0);
    CHECK(minlist_count(&fixture.openers[0].requests) == 1u);
    CHECK(rejected.ios2_DataLength == sizeof(buffers[0]));
    CHECK(minlist_count(&fixture.openers[1].requests) == 0u);
    CHECK(fixture.base.unit.rx_unmatched_frames == 0u);
    CHECK(fixture.base.unit.rx_dropped_frames == 0u);

    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(filter_calls == 2u);
    CHECK(reply_calls == 2u);
    CHECK(replies[1] == &rejected);
    CHECK(rejected.ios2_Req.io_Error == 0);
    CHECK(rejected.ios2_DataLength == sizeof(payload));
    CHECK(memcmp(buffers[0], payload, sizeof(payload)) == 0);
    CHECK(fixture.base.unit.stats.PacketsReceived == 2u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(minlist_count(&fixture.openers[1].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(fixture.openers[1].refs == 1u);
    CHECK(free_calls == 2u);
}

static void test_rx_short_buffer_completes_once_with_error(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    UBYTE buffer[4];
    UBYTE frame[P4WLAN_ETH_HEADER + 8u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    build_frame(frame, destination, source, 0x0800u, payload, sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_MTU_EXCEEDED);
    CHECK(request.ios2_WireError == S2WERR_BUFF_ERROR);
    CHECK(copy_to_calls == 0u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

static void test_rx_copy_failure_completes_once(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    UBYTE buffer[8];
    UBYTE frame[P4WLAN_ETH_HEADER + 4u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[4] = {0xaa, 0xbb, 0xcc, 0xdd};

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    copy_to_failure = TRUE;
    build_frame(frame, destination, source, 0x0800u, payload, sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(request.ios2_WireError == S2WERR_BUFF_ERROR);
    CHECK(copy_to_calls == 1u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

static void test_active_read_abort_during_copy_completes_once(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    UBYTE buffer[16];
    UBYTE frame[P4WLAN_ETH_HEADER + 4u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[4] = {0x21, 0x43, 0x65, 0x87};

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    rx_copy_abort_request = &request;
    abort_during_rx_copy = TRUE;
    build_frame(frame, destination, source, 0x0800u, payload, sizeof(payload));

    /* The copy mock inserts a deterministic AbortIO at the unlocked RX-copy
     * boundary; this is not a real concurrent task or hook-reentry guarantee. */
    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(rx_copy_abort_result == 0);
    CHECK(copy_to_calls == 1u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(fixture.openers[0].request_count == 0u);
    CHECK(fixture.openers[0].read_count == 0u);
    CHECK(fixture.base.unit.request_count == 0u);
    CHECK(fixture.base.unit.read_count == 0u);
    CHECK(free_calls == 1u);
}

static void test_close_state_interleaving_during_filter_aborts_rx(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct Hook filter;
    struct close_interleave interleave;
    UBYTE buffer[16];
    UBYTE frame[P4WLAN_ETH_HEADER + 4u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[4] = {1, 4, 9, 16};

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    interleave.unit = &fixture.base.unit;
    interleave.opener = &fixture.openers[0];
    interleave.calls = 0u;
    filter.invoke = close_during_filter;
    filter.user_data = &interleave;
    fixture.openers[0].filter = &filter;
    build_frame(frame, destination, source, 0x0800u, payload, sizeof(payload));

    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(interleave.calls == 1u);
    CHECK(fixture.openers[0].closing == TRUE);
    CHECK(copy_to_calls == 1u);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

struct guarded_rx_buffer {
    UBYTE before[16];
    UBYTE bytes[P4WLAN_MAX_FRAME];
    UBYTE after[16];
};

static void check_guard(const UBYTE *guard, ULONG length, UBYTE expected)
{
    ULONG i;
    for (i = 0; i < length; ++i)
        CHECK(guard[i] == expected);
}

static void test_cooked_rx_exact_mtu_with_guard_bytes(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct guarded_rx_buffer guarded;
    UBYTE frame[P4WLAN_ETH_HEADER + P4WLAN_MTU];
    UBYTE payload[P4WLAN_MTU];
    const UBYTE destination[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
    const UBYTE source[6] = {0x02, 0x06, 0x07, 0x08, 0x09, 0x0a};
    ULONG i;

    for (i = 0; i < sizeof(payload); ++i)
        payload[i] = (UBYTE)(0x61u + (i & 0xffu));
    memset(&guarded, 0xa5, sizeof(guarded));
    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request,
                     guarded.bytes, sizeof(payload), 0x0800u, 0u);
    build_frame(frame, destination, source, 0x0800u, payload,
                sizeof(payload));

    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_DataLength == P4WLAN_MTU);
    CHECK(memcmp(guarded.bytes, payload, sizeof(payload)) == 0);
    /* Storage is larger than the advertised capacity: the unused suffix is
     * also a canary, so an over-copy within that storage cannot pass. */
    check_guard(guarded.bytes + sizeof(payload),
                sizeof(guarded.bytes) - sizeof(payload), 0xa5u);
    check_guard(guarded.before, sizeof(guarded.before), 0xa5u);
    check_guard(guarded.after, sizeof(guarded.after), 0xa5u);
    CHECK(fixture.base.unit.stats.PacketsReceived == 1u);
    CHECK(fixture.base.unit.rx_bytes == sizeof(frame));
    CHECK(copy_to_calls == 1u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

static void test_raw_rx_min_max_with_guard_bytes(void)
{
    struct fixture fixture;
    struct IOSana2Req requests[2];
    struct guarded_rx_buffer guarded[2];
    UBYTE frame[P4WLAN_MAX_FRAME];
    UBYTE payload[P4WLAN_MAX_FRAME - P4WLAN_ETH_HEADER];
    const UBYTE destination[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
    const UBYTE source[6] = {0x02, 0x06, 0x07, 0x08, 0x09, 0x0a};
    const ULONG lengths[2] = {P4WLAN_ETH_HEADER, P4WLAN_MAX_FRAME};
    ULONG i;

    for (i = 0; i < sizeof(payload); ++i)
        payload[i] = (UBYTE)(0xe1u - (i & 0xffu));
    memset(guarded, 0xa5, sizeof(guarded));
    reset_observations();
    fixture_init(&fixture, 1u);

    for (i = 0; i < 2u; ++i) {
        ULONG payload_length = lengths[i] - P4WLAN_ETH_HEADER;
        (void)queue_read(&fixture, &fixture.openers[0], &requests[i],
                         guarded[i].bytes, lengths[i], 0x88b5u,
                         SANA2IOF_RAW);
        build_frame(frame, destination, source, 0x88b5u, payload,
                    payload_length);
        CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, lengths[i]));
        CHECK(reply_calls == i + 1u);
        CHECK(replies[i] == &requests[i]);
        CHECK(requests[i].ios2_Req.io_Error == 0);
        CHECK(requests[i].ios2_DataLength == lengths[i]);
        CHECK(memcmp(guarded[i].bytes, frame, lengths[i]) == 0);
        check_guard(guarded[i].bytes + lengths[i],
                    sizeof(guarded[i].bytes) - lengths[i], 0xa5u);
        check_guard(guarded[i].before, sizeof(guarded[i].before), 0xa5u);
        check_guard(guarded[i].after, sizeof(guarded[i].after), 0xa5u);
        CHECK(fixture.openers[0].refs == 1u);
    }
    CHECK(fixture.base.unit.stats.PacketsReceived == 2u);
    CHECK(fixture.base.unit.rx_bytes ==
          P4WLAN_ETH_HEADER + P4WLAN_MAX_FRAME);
    CHECK(free_calls == 2u);
    CHECK(IsMinListEmpty(&fixture.openers[0].requests));
}

static void test_cooked_rx_rejects_payload_above_mtu(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    UBYTE buffer[P4WLAN_MTU];
    UBYTE frame[P4WLAN_MAX_FRAME];
    UBYTE payload[P4WLAN_MAX_FRAME - P4WLAN_ETH_HEADER];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};

    memset(payload, 0x5a, sizeof(payload));
    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    build_frame(frame, destination, source, 0x0800u, payload,
                sizeof(payload));

    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_MTU_EXCEEDED);
    CHECK(request.ios2_WireError == S2WERR_BUFF_ERROR);
    CHECK(copy_to_calls == 0u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

static void test_malformed_rx_lengths_drop_without_consuming_read(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    UBYTE buffer[16];
    UBYTE short_frame[P4WLAN_ETH_HEADER - 1u];
    UBYTE oversized[P4WLAN_MAX_FRAME + 1u];
    UBYTE valid_frame[P4WLAN_ETH_HEADER + 2u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[2] = {0xab, 0xcd};

    memset(short_frame, 0x11, sizeof(short_frame));
    memset(oversized, 0x22, sizeof(oversized));
    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);

    CHECK(p4wlan_frame_callback(&fixture.base.unit, short_frame,
                                sizeof(short_frame)));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, oversized,
                                sizeof(oversized)));
    CHECK(reply_calls == 0u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 1u);
    CHECK(fixture.base.unit.stats.BadData == 2u);
    CHECK(fixture.base.unit.stats.PacketsReceived == 0u);
    CHECK(fixture.base.unit.rx_dropped_frames == 2u);
    CHECK(fixture.base.unit.rx_sequence == 0u);

    build_frame(valid_frame, destination, source, 0x0800u, payload,
                sizeof(payload));
    CHECK(p4wlan_frame_callback(&fixture.base.unit, valid_frame,
                                sizeof(valid_frame)));
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_DataLength == sizeof(payload));
    CHECK(fixture.base.unit.stats.PacketsReceived == 1u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 0u);
    CHECK(fixture.openers[0].refs == 1u);
}

static void test_offline_rx_drop_does_not_complete_read(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct MinList done;
    UBYTE buffer[16];
    UBYTE frame[P4WLAN_ETH_HEADER + 2u];
    const UBYTE destination[6] = {0x02, 0, 0, 0, 0, 1};
    const UBYTE source[6] = {0x02, 0, 0, 0, 0, 2};
    const UBYTE payload[2] = {0x12, 0x34};

    reset_observations();
    fixture_init(&fixture, 1u);
    (void)queue_read(&fixture, &fixture.openers[0], &request, buffer,
                     sizeof(buffer), 0x0800u, 0u);
    fixture.base.unit.online = FALSE;
    build_frame(frame, destination, source, 0x0800u, payload,
                sizeof(payload));

    CHECK(p4wlan_frame_callback(&fixture.base.unit, frame, sizeof(frame)));
    CHECK(reply_calls == 0u);
    CHECK(minlist_count(&fixture.openers[0].requests) == 1u);
    CHECK(fixture.openers[0].refs == 2u);
    CHECK(fixture.base.unit.stats.PacketsReceived == 0u);
    CHECK(fixture.base.unit.rx_dropped_frames == 1u);

    /* Clean the deliberately retained queued read using actual flush and
     * finish helpers after asserting that the RX callback did not reply. */
    minlist_init(&done);
    ObtainSemaphore(&fixture.base.unit.lock);
    p4wlan_flush_all_locked(&fixture.base.unit, &done,
                            S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE, TRUE);
    ReleaseSemaphore(&fixture.base.unit.lock);
    CHECK(minlist_count(&done) == 1u);
    p4wlan_finish_done(&fixture.base.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(replies[0] == &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_OUTOFSERVICE);
    CHECK(request.ios2_WireError == S2WERR_UNIT_OFFLINE);
    CHECK(fixture.openers[0].refs == 1u);
    CHECK(free_calls == 1u);
}

int main(void)
{
    test_cooked_tx_frame_contents();
    test_cooked_tx_exact_mtu_and_rejects_oversize();
    test_raw_tx_boundaries_and_validation();
    test_tx_copy_failure_completes_without_send();
    test_radio_send_failure_completes_once_and_wipes();
    test_abort_during_copy_prevents_wire_send();
    test_abort_refuses_committed_tx();
    test_rx_broadcast_multicast_and_multiple_openers();
    test_rx_filter_rejects_then_advances_to_next_frame();
    test_rx_short_buffer_completes_once_with_error();
    test_rx_copy_failure_completes_once();
    test_active_read_abort_during_copy_completes_once();
    test_close_state_interleaving_during_filter_aborts_rx();
    test_cooked_rx_exact_mtu_with_guard_bytes();
    test_raw_rx_min_max_with_guard_bytes();
    test_cooked_rx_rejects_payload_above_mtu();
    test_malformed_rx_lengths_drop_without_consuming_read();
    test_offline_rx_drop_does_not_complete_read();
    printf("SANA datapath host checks: %lu\n", checks);
    return 0;
}
