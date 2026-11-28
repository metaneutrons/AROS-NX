/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Source-extracted association lifecycle tests. The selected SANA BeginIO
 * switch arms are the production text; this harness does not model the full
 * BeginIO prologue or device scheduler. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define P4_WIFI_ASSOC_DIAGNOSTICS 1

/* Exact numeric-only formats; execute opt-in observers in every lifecycle
 * policy fixture without sending any synthetic payload to a console. */
static void bug(const char *format, ...)
{
    if (strcmp(format, "[WiFiReq] t=%lu done=%lu\n") &&
        strcmp(format, "[WiFiReq] reject=1\n") &&
        strcmp(format, "[WiFiReq] reject=2\n") &&
        strcmp(format, "[WiFiIO] c=%lu p=%lu\n") &&
        strcmp(format, "[WiFiInfo] e=%ld v=%lu s=%lu\n") &&
        strcmp(format, "[WiFiRetire] k=%lu s=%lu d=%lu\n") &&
        strcmp(format, "[WiFiReq] t=%lu ch=%lu a=%lu f=%lu\n")) {
        fprintf(stderr, "unexpected association diagnostic format\n");
        exit(1);
    }
}

typedef void *APTR;
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t UWORD;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int16_t BOOL;

#define TRUE ((BOOL)1)
#define FALSE ((BOOL)0)
#define MEMF_PUBLIC 0x01u
#define MEMF_CLEAR 0x02u
#define IOF_QUICK 0x01u
#define NT_MESSAGE 5u
#define P4WLAN_MAX_FRAME 1518u

/* Let the production association-options and bridge headers use these host
 * primitive types while retaining their real tag values and target layouts. */
#define EXEC_TYPES_H 1
#define EXEC_LIBRARIES_H 1
#define EXEC_SEMAPHORES_H 1
#include "../network/fullmac_policy.h"
#include "../network/association_options.h"

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

struct SignalSemaphore { unsigned depth; };
struct Task { ULONG id; };
struct Library { ULONG unused; };
struct Device { ULONG unused; };
struct Unit { ULONG unused; };
struct Hook { ULONG unused; };
struct ExecBase { ULONG unused; };
struct Sana2DeviceStats { ULONG words[16]; };
struct Sana2DeviceQuery { ULONG words[16]; };

#include "../network/runtime_probe.h"

typedef BOOL (*p4wlan_copy_fn)(APTR, APTR, ULONG);

struct Message {
    struct Node mn_Node;
    ULONG mn_Length;
};

struct IORequest {
    struct Message io_Message;
    struct Device *io_Device;
    struct Unit *io_Unit;
    ULONG io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
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

struct p4wlan_opener;
struct p4wlan_unit;

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

#define NEWLIST(list) minlist_init((struct MinList *)(list))
#define ADDTAIL(list, node) \
    minlist_addtail((struct MinList *)(list), (struct MinNode *)(node))
#define REMOVE(node) minlist_remove((struct MinNode *)(node))
#define IsMinListEmpty(list) \
    ((list)->mlh_TailPred == (struct MinNode *)(list))

static void ObtainSemaphore(struct SignalSemaphore *semaphore);
static void ReleaseSemaphore(struct SignalSemaphore *semaphore);
static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore);
static APTR AllocVec(IPTR size, ULONG flags);
static void FreeVec(APTR memory);
static APTR AllocPooled(APTR pool, ULONG size);
static void FreePooled(APTR pool, APTR memory, ULONG size);
static void ReplyMsg(struct Message *message);
static void Signal(struct Task *task, ULONG mask);
static void CopyMem(const void *source, void *destination, ULONG length);

#include "sana_association_queue_extract.inc"

static unsigned long checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

struct allocation_record {
    void *pointer;
    BOOL live;
    BOOL association;
};

static struct allocation_record allocation_records[128];
static unsigned allocation_slots;
static unsigned allocation_calls;
static unsigned free_calls;
static unsigned wipe_checks;
static unsigned reply_calls;
static struct IOSana2Req *replied[256];
static BOOL fail_next_vector_allocation;
static BOOL fail_next_pool_allocation;
static APTR pool_memory;
static struct p4wlan_unit *active_unit;
static struct p4wlan_opener *active_opener;
static struct p4wlan_opener *disassociate_requester;
static struct IOSana2Req *replacement_request;
static struct IOSana2Req *disassociate_request;
static const struct TagItem *replacement_tags;
static const struct TagItem *disassociate_tags;
static BOOL replace_during_associate;
static BOOL disassociate_during_associate;
static BOOL associate_returns_success;
static BOOL callback_fault_during_associate;
static BOOL association_allocation_expected;
static BOOL offline_during_associate;
static BOOL disconnect_returns_success;
static BOOL replace_during_disconnect;
static ULONG mismatch_snapshot_field;
static unsigned associate_calls;
static unsigned unlocked_association_replies;
static unsigned unacknowledged_service_skips;
static unsigned associate_cancel_checks;
static unsigned disassociate_cancel_checks;
static unsigned offline_cancel_checks;
static unsigned disconnect_calls;
static BOOL reuse_request_during_association_reply;
static BOOL fail_teardown_during_association_reply;
static BOOL disassociate_during_failed_teardown_reply;
static BOOL close_model_during_failed_teardown_reply;
static BOOL offline_during_failed_teardown_reply;
static struct IOSana2Req *failure_event_before_ack;
static struct IOSana2Req *failed_teardown_leave_request;
static const struct TagItem *failed_teardown_leave_tags;
static ULONG token_after_failed_teardown_reply;
static struct TagItem *tags_to_reuse_during_reply;
static size_t tags_to_reuse_count;
static struct TagItem *replacement_tags_for_reply;
static BOOL association_reply_callback_active;
static BOOL service_before_association_ack;
static ULONG cookies_seen_at_association_reply[128];
static unsigned association_reply_cookie_count;

static void dispatch_case(struct p4wlan_unit *unit,
                          struct p4wlan_opener *opener,
                          struct IOSana2Req *request)
{
    struct MinList done;
    struct p4wlan_request *item = NULL;
    ULONG command = request->ios2_Req.io_Command;
    ULONG event_mask = request->ios2_WireError;
    NEWLIST(&done);
    request->ios2_Req.io_Error = 0;
    request->ios2_WireError = 0;
    association_allocation_expected = command == S2_SETOPTIONS;
    ObtainSemaphore(&unit->lock);
    switch (command) {
#include "sana_association_queue_cases.inc"
    default:
        p4wlan_immediate_error(request, S2ERR_BAD_ARGUMENT,
                               S2WERR_GENERIC_ERROR);
        break;
    }
    association_allocation_expected = FALSE;
#include "sana_association_queue_epilogue.inc"
}

static LONG dispatch_abort(struct esp32p4wlan_base *LIBBASE,
                           struct IORequest *iorequest)
{
#include "sana_abort_io_body.inc"
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

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore != NULL);
    CHECK(semaphore->depth == 0u);
    semaphore->depth = 1u;
    return TRUE;
}

static APTR AllocVec(IPTR size, ULONG flags)
{
    void *memory;
    BOOL association = association_allocation_expected;
    association_allocation_expected = FALSE;
    if (fail_next_vector_allocation) {
        fail_next_vector_allocation = FALSE;
        return NULL;
    }
    memory = (flags & MEMF_CLEAR) ? calloc(1u, (size_t)size) :
                                    malloc((size_t)size);
    if (!memory)
        return NULL;
    CHECK(allocation_slots <
          sizeof(allocation_records) / sizeof(allocation_records[0]));
    allocation_records[allocation_slots].pointer = memory;
    allocation_records[allocation_slots].live = TRUE;
    allocation_records[allocation_slots].association = association;
    ++allocation_slots;
    ++allocation_calls;
    return memory;
}

static BOOL bytes_are_zero(const UBYTE *bytes, size_t size)
{
    size_t index;
    for (index = 0; index < size; ++index)
        if (bytes[index] != 0u)
            return FALSE;
    return TRUE;
}

static void FreeVec(APTR memory)
{
    unsigned index = allocation_slots;
    while (index != 0u) {
        --index;
        if (allocation_records[index].pointer == memory &&
            allocation_records[index].live)
            break;
    }
    CHECK(index < allocation_slots);
    CHECK(allocation_records[index].pointer == memory);
    CHECK(allocation_records[index].live);
    if (allocation_records[index].association) {
        CHECK(bytes_are_zero((const UBYTE *)memory,
                             sizeof(struct p4wlan_association)));
        ++wipe_checks;
    }
    allocation_records[index].live = FALSE;
    ++free_calls;
    free(memory);
}

static APTR AllocPooled(APTR pool, ULONG size)
{
    (void)pool;
    CHECK(pool_memory == NULL);
    if (fail_next_pool_allocation) {
        fail_next_pool_allocation = FALSE;
        return NULL;
    }
    pool_memory = malloc((size_t)size);
    return pool_memory;
}

static void FreePooled(APTR pool, APTR memory, ULONG size)
{
    (void)pool;
    (void)size;
    CHECK(memory != NULL && memory == pool_memory);
    free(pool_memory);
    pool_memory = NULL;
}

static unsigned live_allocations(void)
{
    unsigned index;
    unsigned count = 0u;
    for (index = 0u; index < allocation_slots; ++index)
        if (allocation_records[index].live)
            ++count;
    return count;
}

static void ReplyMsg(struct Message *message)
{
    struct IOSana2Req *request = (struct IOSana2Req *)message;
    CHECK(active_unit != NULL);
    if (request->ios2_Req.io_Command == S2_SETOPTIONS &&
        active_unit->lock.depth == 0u &&
        request->ios2_Req.io_Error == 0 &&
        active_unit->pending_association &&
        !active_unit->pending_association->acknowledged &&
        active_unit->pending_association->opener ==
            request->ios2_BufferManagement) {
        struct p4wlan_association *pending;
        ULONG token_at_reply;
        CHECK(request->ios2_Req.io_Flags & IOF_QUICK ? FALSE : TRUE);
        CHECK(active_unit->lock.depth == 0u);
        CHECK(active_unit->pending_association != NULL);
        CHECK(active_unit->pending_association->opener ==
              request->ios2_BufferManagement);
        pending = active_unit->pending_association;
        token_at_reply = pending->token;
        CHECK(!pending->acknowledged);
        CHECK(association_reply_cookie_count <
              sizeof(cookies_seen_at_association_reply) /
                  sizeof(cookies_seen_at_association_reply[0]));
        cookies_seen_at_association_reply[association_reply_cookie_count++] =
            pending->cookie;
        ++unlocked_association_replies;

        /* ReplyMsg may call PA_CALL code. Service must not consume a pending
         * selection until this exact request has been acknowledged. */
        if (!association_reply_callback_active &&
            service_before_association_ack) {
            unsigned calls_before = associate_calls;
            association_reply_callback_active = TRUE;
            service_before_association_ack = FALSE;
            CHECK(p4wlan_associate_one(active_unit, active_unit->radio));
            CHECK(active_unit->pending_association == pending);
            CHECK(!pending->acknowledged);
            CHECK(associate_calls == calls_before);
            ++unacknowledged_service_skips;
            association_reply_callback_active = FALSE;
        }

        /* Model a disconnect failure inside the unlocked Reply hook, before
         * this SETOPTIONS frame has acknowledged its accepted typed attempt. */
        if (!association_reply_callback_active &&
            fail_teardown_during_association_reply) {
            struct p4wlan_association *unacknowledged =
                active_unit->pending_association;
            unsigned calls_before = associate_calls;
            unsigned disconnects_before = disconnect_calls;
            CHECK(unacknowledged == pending);
            CHECK(active_unit->teardown_pending);
            CHECK(!disconnect_returns_success);
            fail_teardown_during_association_reply = FALSE;
            association_reply_callback_active = TRUE;
            CHECK(p4wlan_maintain_link(active_unit, active_unit->radio));
            CHECK(disconnect_calls == disconnects_before + 1u);
            CHECK(active_unit->callback_fault);
            CHECK(!active_unit->teardown_pending);
            /* This is the missing boundary: an accepted but not yet ACKed
             * attempt must stay pinned/current until its own Reply returns. */
            CHECK(active_unit->pending_association == unacknowledged);
            CHECK(unacknowledged->token == token_at_reply);
            CHECK(!unacknowledged->acknowledged);
            CHECK(unacknowledged->opener->active_cookie ==
                  unacknowledged->cookie);
            CHECK(failure_event_before_ack != NULL);
            CHECK(failure_event_before_ack->ios2_WireError == 0u);
            CHECK(unacknowledged->opener->failed_cookie == 0u);
            CHECK(p4wlan_associate_one(active_unit, active_unit->radio));
            CHECK(active_unit->pending_association == unacknowledged);
            CHECK(!unacknowledged->acknowledged);
            CHECK(active_unit->active_association == NULL);
            CHECK(associate_calls == calls_before);
            CHECK(failure_event_before_ack->ios2_WireError == 0u);
            token_after_failed_teardown_reply = active_unit->selection_token;
            if (disassociate_during_failed_teardown_reply) {
                struct p4wlan_opener *owner = unacknowledged->opener;
                disassociate_during_failed_teardown_reply = FALSE;
                CHECK(failed_teardown_leave_request != NULL);
                CHECK(failed_teardown_leave_tags != NULL);
                failed_teardown_leave_request->ios2_Data =
                    (APTR)failed_teardown_leave_tags;
                dispatch_case(active_unit, owner,
                              failed_teardown_leave_request);
                /* The faulted real SETOPTIONS leave must still revoke the
                 * owner's unacknowledged cookie, without new teardown RPC. */
                CHECK(failed_teardown_leave_request->ios2_Req.io_Error == 0);
                CHECK(active_unit->pending_association == NULL);
                CHECK(active_unit->active_association == NULL);
                CHECK(active_unit->selection_token >
                      token_after_failed_teardown_reply);
                CHECK(owner->active_cookie == 0u);
                CHECK(owner->failed_cookie == 0u);
                CHECK(failure_event_before_ack->ios2_WireError == 0u);
                CHECK(associate_calls == calls_before);
                CHECK(disconnect_calls == disconnects_before + 1u);
            }
            if (close_model_during_failed_teardown_reply) {
                struct p4wlan_opener *owner = unacknowledged->opener;
                struct MinList done;
                close_model_during_failed_teardown_reply = FALSE;
                NEWLIST(&done);
                /* Model Close's local phase only: closing flag plus the
                 * actual extracted opener-flush/cancel helper. This does not
                 * claim to exercise Close's list unlink or wait/drain loop. */
                ObtainSemaphore(&active_unit->lock);
                owner->closing = TRUE;
                p4wlan_flush_opener_locked(active_unit, owner, &done,
                    IOERR_ABORTED, S2WERR_GENERIC_ERROR, TRUE);
                ReleaseSemaphore(&active_unit->lock);
                p4wlan_finish_done(active_unit, &done);
                CHECK(active_unit->pending_association == NULL);
                CHECK(active_unit->selection_token >
                      token_after_failed_teardown_reply);
                CHECK(owner->active_cookie == 0u);
                CHECK(owner->failed_cookie == 0u);
                CHECK(failure_event_before_ack->ios2_WireError !=
                      S2EVENT_CONNECT_FAILED);
                CHECK(associate_calls == calls_before);
                CHECK(disconnect_calls == disconnects_before + 1u);
            }
            if (offline_during_failed_teardown_reply) {
                struct IOSana2Req offline_request;
                struct p4wlan_opener *owner = unacknowledged->opener;
                offline_during_failed_teardown_reply = FALSE;
                memset(&offline_request, 0, sizeof(offline_request));
                offline_request.ios2_Req.io_Command = S2_OFFLINE;
                offline_request.ios2_BufferManagement = owner;
                dispatch_case(active_unit, owner, &offline_request);
                CHECK(!active_unit->online);
                CHECK(active_unit->pending_association == NULL);
                CHECK(active_unit->selection_token >
                      token_after_failed_teardown_reply);
                CHECK(owner->active_cookie == 0u);
                CHECK(failure_event_before_ack->ios2_WireError !=
                      S2EVENT_CONNECT_FAILED);
            }
            association_reply_callback_active = FALSE;
        }

        /* Reuse both borrowed objects from the PA_CALL reply hook. A nested
         * accepted request supersedes the old attempt while its BeginIO frame
         * is suspended; the old frame may only ACK by its saved token. */
        if (!association_reply_callback_active &&
            reuse_request_during_association_reply) {
            CHECK(tags_to_reuse_during_reply != NULL);
            CHECK(replacement_tags_for_reply != NULL);
            CHECK(request->ios2_Data == tags_to_reuse_during_reply);
            association_reply_callback_active = TRUE;
            reuse_request_during_association_reply = FALSE;
            memcpy(tags_to_reuse_during_reply, replacement_tags_for_reply,
                   tags_to_reuse_count * sizeof(*tags_to_reuse_during_reply));
            request->ios2_Req.io_Flags |= IOF_QUICK;
            dispatch_case(active_unit, active_opener, request);
            CHECK(active_unit->pending_association != NULL);
            CHECK(active_unit->pending_association->token != token_at_reply);
            CHECK(active_unit->pending_association->acknowledged);
            /* Model another accepted BeginIO still between SETOPTIONS and
             * its own ACK while this older ReplyMsg callback is unwinding. */
            active_unit->pending_association->acknowledged = FALSE;
            memset(tags_to_reuse_during_reply, 0,
                   tags_to_reuse_count * sizeof(*tags_to_reuse_during_reply));
            memset(replacement_tags_for_reply, 0,
                   tags_to_reuse_count * sizeof(*replacement_tags_for_reply));
            request->ios2_Data = NULL;
            request->ios2_BufferManagement = NULL;
            request->ios2_Req.io_Command = 0xDEADu;
            association_reply_callback_active = FALSE;
        }
    } else {
        CHECK(active_unit->lock.depth == 0u);
    }
    CHECK(reply_calls < sizeof(replied) / sizeof(replied[0]));
    replied[reply_calls++] = request;
}

static void Signal(struct Task *task, ULONG mask)
{
    CHECK(task != NULL);
    CHECK(mask != 0u);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 1u);
}

static void CopyMem(const void *source, void *destination, ULONG length)
{
    CHECK(source != NULL && destination != NULL);
    memcpy(destination, source, (size_t)length);
}

static BOOL fake_associate(struct P4WifiBase *radio,
                           const struct p4_wifi_association_target *target,
                           const struct p4_wifi_association_cancel *cancel)
{
    ULONG index;
    CHECK(active_unit != NULL && active_opener != NULL);
    CHECK(active_unit->lock.depth == 0u);
    CHECK(radio == active_unit->radio);
    CHECK(radio->association_enabled);
    ++associate_calls;
    if (replace_during_associate) {
        replace_during_associate = FALSE;
        replacement_request->ios2_Data = (APTR)replacement_tags;
        dispatch_case(active_unit, active_opener, replacement_request);
        CHECK(cancel->cancelled(cancel->context));
        ++associate_cancel_checks;
    }
    if (offline_during_associate) {
        struct IOSana2Req offline_request;
        offline_during_associate = FALSE;
        memset(&offline_request, 0, sizeof(offline_request));
        offline_request.ios2_Req.io_Command = S2_OFFLINE;
        offline_request.ios2_BufferManagement = active_opener;
        dispatch_case(active_unit, active_opener, &offline_request);
        CHECK(!active_unit->online);
        CHECK(cancel->cancelled(cancel->context));
        ++offline_cancel_checks;
    }
    if (disassociate_during_associate) {
        disassociate_during_associate = FALSE;
        CHECK(disassociate_requester != NULL);
        CHECK(disassociate_request != NULL);
        CHECK(disassociate_tags != NULL);
        CHECK(active_unit->active_association == cancel->context);
        CHECK(active_unit->pending_association == NULL);
        CHECK(active_unit->active_association->target.fullmac_profile != 0u);
        CHECK(active_unit->active_association->cookie != 0u);
        disassociate_request->ios2_Data = (APTR)disassociate_tags;
        dispatch_case(active_unit, disassociate_requester,
                      disassociate_request);
        CHECK(!cancel->cancelled(cancel->context));
        CHECK(active_unit->active_association == cancel->context);
        CHECK(active_unit->active_association->opener == active_opener);
        CHECK(active_unit->active_association->opener->active_cookie ==
              active_unit->active_association->cookie);
        ++disassociate_cancel_checks;
    }
    if (callback_fault_during_associate) {
        callback_fault_during_associate = FALSE;
        __sync_lock_test_and_set(&active_unit->callback_fault, TRUE);
    }
    if (!associate_returns_success)
        return FALSE;
    memset(&radio->confirmed_ap, 0, sizeof(radio->confirmed_ap));
    radio->confirmed_ap.ssid_length = target->ssid_length;
    for (index = 0; index < target->ssid_length; ++index)
        radio->confirmed_ap.ssid[index] = target->ssid[index];
    radio->confirmed_ap.bssid[0] = 0x02u;
    radio->confirmed_ap.bssid[1] = 0x12u;
    radio->confirmed_ap.bssid[2] = 0x34u;
    radio->confirmed_ap.bssid[3] = 0x56u;
    radio->confirmed_ap.bssid[4] = 0x78u;
    radio->confirmed_ap.bssid[5] = 0x9Au;
    radio->confirmed_ap.channel = target->channel ? target->channel : 6u;
    radio->confirmed_ap.authmode = target->authmode;
    if (mismatch_snapshot_field == 1u)
        radio->confirmed_ap.ssid[0] ^= 0x20u;
    else if (mismatch_snapshot_field == 2u)
        radio->confirmed_ap.bssid[5] ^= 0x01u;
    else if (mismatch_snapshot_field == 3u)
        ++radio->confirmed_ap.channel;
    else if (mismatch_snapshot_field == 4u)
        ++radio->confirmed_ap.authmode;
    radio->observed_ap = radio->confirmed_ap;
    radio->link_state = P4_WIFI_LINK_UP;
    return TRUE;
}

static BOOL fake_disconnect(struct P4WifiBase *radio)
{
    CHECK(active_unit != NULL && active_opener != NULL);
    CHECK(active_unit->lock.depth == 0u);
    CHECK(radio == active_unit->radio);
    ++disconnect_calls;
    if (replace_during_disconnect) {
        replace_during_disconnect = FALSE;
        replacement_request->ios2_Data = (APTR)replacement_tags;
        dispatch_case(active_unit, active_opener, replacement_request);
    }
    if (disconnect_returns_success) {
        radio->link_state = P4_WIFI_LINK_DOWN;
        memset(&radio->observed_ap, 0, sizeof(radio->observed_ap));
    }
    return disconnect_returns_success;
}

static BOOL fake_resource_attach(struct P4WifiBase *radio, APTR client,
    const struct p4_wifi_device_callbacks *callbacks)
{
    /* Presence stubs: p4wlan_resource_valid never invokes bridge callbacks. */
    (void)radio;
    (void)client;
    (void)callbacks;
    return TRUE;
}

static void fake_resource_detach(struct P4WifiBase *radio, APTR client)
{
    (void)radio;
    (void)client;
}

static BOOL fake_resource_send_frame(struct P4WifiBase *radio,
    const UBYTE *frame, ULONG length)
{
    (void)radio;
    (void)frame;
    (void)length;
    return TRUE;
}

static BOOL fake_resource_scan(struct P4WifiBase *radio,
    struct p4_wifi_scan_result *result)
{
    (void)radio;
    (void)result;
    return TRUE;
}

static ULONG fake_resource_maintenance_request(struct P4WifiBase *radio)
{
    (void)radio;
    return 1u;
}

static ULONG fake_resource_maintenance_poll(struct P4WifiBase *radio,
    ULONG token)
{
    (void)radio;
    (void)token;
    return P4_WIFI_MAINT_ACTIVE;
}

static BOOL fake_resource_maintenance_release(struct P4WifiBase *radio,
    ULONG token)
{
    (void)radio;
    (void)token;
    return TRUE;
}

static ULONG fake_resource_discovery_poll(struct P4WifiBase *radio, ULONG token,
    struct p4_wifi_discovery_result *output)
{
    (void)radio;
    (void)token;
    (void)output;
    return P4_WIFI_DISCOVERY_IDLE;
}

static void init_request(struct IOSana2Req *request, ULONG command,
                         struct p4wlan_opener *opener)
{
    memset(request, 0, sizeof(*request));
    request->ios2_Req.io_Command = command;
    request->ios2_Req.io_Flags = 0u; /* BeginIO's normal non-quick request */
    request->ios2_BufferManagement = opener;
}

static BOOL fake_resource_maintenance_fence(struct P4WifiBase *radio,
                                           ULONG token, ULONG mode)
{
    (void)radio; (void)token; (void)mode;
    return FALSE;
}

static void init_fixture(struct p4wlan_unit *unit,
                         struct p4wlan_opener *opener,
                         struct P4WifiBase *radio)
{
    memset(unit, 0, sizeof(*unit));
    memset(opener, 0, sizeof(*opener));
    memset(radio, 0, sizeof(*radio));
    NEWLIST(&unit->openers);
    NEWLIST(&unit->write_queue);
    NEWLIST(&unit->scan_queue);
    NEWLIST(&opener->requests);
    ADDTAIL((struct List *)&unit->openers, (struct Node *)&opener->unit_node);
    opener->refs = 1u; /* Close's base pin */
    unit->online = TRUE;
    unit->hardware_ready = TRUE;
    unit->selection_token = 10u;
    unit->radio = radio;
    radio->bridge_abi = P4_WIFI_DEVICE_BRIDGE_ABI;
    radio->bridge_size = sizeof(*radio);
    radio->attach = fake_resource_attach;
    radio->detach = fake_resource_detach;
    radio->send_frame = fake_resource_send_frame;
    radio->scan = fake_resource_scan;
    radio->association_enabled = TRUE;
    radio->associate = fake_associate;
    radio->disconnect = fake_disconnect;
    radio->maintenance_request = fake_resource_maintenance_request;
    radio->maintenance_poll = fake_resource_maintenance_poll;
    radio->maintenance_release = fake_resource_maintenance_release;
    radio->maintenance_discover = fake_resource_maintenance_release;
    radio->maintenance_discovery_poll = fake_resource_discovery_poll;
    radio->maintenance_fence = fake_resource_maintenance_fence;
    active_unit = unit;
    active_opener = opener;
    disassociate_requester = NULL;
    replace_during_associate = FALSE;
    disassociate_during_associate = FALSE;
    associate_returns_success = TRUE;
    callback_fault_during_associate = FALSE;
    mismatch_snapshot_field = 0u;
    replacement_request = NULL;
    disassociate_request = NULL;
    replacement_tags = NULL;
    disassociate_tags = NULL;
    association_allocation_expected = FALSE;
    offline_during_associate = FALSE;
    offline_cancel_checks = 0u;
    disassociate_cancel_checks = 0u;
    disconnect_returns_success = TRUE;
    replace_during_disconnect = FALSE;
    disconnect_calls = 0u;
    reuse_request_during_association_reply = FALSE;
    fail_teardown_during_association_reply = FALSE;
    disassociate_during_failed_teardown_reply = FALSE;
    close_model_during_failed_teardown_reply = FALSE;
    offline_during_failed_teardown_reply = FALSE;
    failure_event_before_ack = NULL;
    failed_teardown_leave_request = NULL;
    failed_teardown_leave_tags = NULL;
    token_after_failed_teardown_reply = 0u;
    tags_to_reuse_during_reply = NULL;
    tags_to_reuse_count = 0u;
    replacement_tags_for_reply = NULL;
    association_reply_callback_active = FALSE;
    service_before_association_ack = FALSE;
    unlocked_association_replies = 0u;
    unacknowledged_service_skips = 0u;
    association_reply_cookie_count = 0u;
}

static void test_resource_valid_requires_abi10_callbacks(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    ULONG (*request)(struct P4WifiBase *);
    ULONG (*poll)(struct P4WifiBase *, ULONG);
    BOOL (*release)(struct P4WifiBase *, ULONG);

    init_fixture(&unit, &opener, &radio);
    CHECK(P4_WIFI_DEVICE_BRIDGE_ABI == 12u);
    CHECK(radio.bridge_abi == P4_WIFI_DEVICE_BRIDGE_ABI);
    CHECK(radio.bridge_size == sizeof(radio));
    /* ABI10 OTA hooks are optional when privacy/recovery release gates are
     * closed; the ordinary SANA bridge remains valid without write dispatch. */
    CHECK(radio.ota_submit == NULL && radio.ota_poll == NULL);
    CHECK(radio.ota_cancel == NULL && radio.ota_discard == NULL);
    CHECK(p4wlan_resource_valid(&radio));
    CHECK(!p4wlan_resource_valid(NULL));

    radio.bridge_abi = 9u;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.bridge_abi = 8u;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.bridge_abi = P4_WIFI_DEVICE_BRIDGE_ABI;
    radio.bridge_size = sizeof(radio) - 1u;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.bridge_size = sizeof(radio);

    request = radio.maintenance_request;
    radio.maintenance_request = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_request = request;
    poll = radio.maintenance_poll;
    radio.maintenance_poll = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_poll = poll;
    release = radio.maintenance_release;
    radio.maintenance_release = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_release = release;
    radio.maintenance_discover = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_discover = fake_resource_maintenance_release;
    radio.maintenance_discovery_poll = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_discovery_poll = fake_resource_discovery_poll;
    radio.maintenance_fence = NULL;
    CHECK(!p4wlan_resource_valid(&radio));
    radio.maintenance_fence = fake_resource_maintenance_fence;
    CHECK(p4wlan_resource_valid(&radio));
}

static void make_tags(struct TagItem tags[10], const UBYTE *ssid,
                      const UBYTE *passphrase, const UBYTE *bssid,
                      ULONG channel)
{
    static UBYTE rsn[20] = {
        48, 18, 1, 0,
        0, 15, 172, 4,
        1, 0, 0, 15, 172, 4,
        1, 0, 0, 15, 172, 2
    };
    ULONG index = 0;
    tags[index].ti_Tag = S2INFO_SSID;
    tags[index++].ti_Data = (IPTR)ssid;
    if (bssid) {
        tags[index].ti_Tag = S2INFO_BSSID;
        tags[index++].ti_Data = (IPTR)bssid;
    }
    tags[index].ti_Tag = S2INFO_PortType;
    tags[index++].ti_Data = (IPTR)S2PORT_MANAGED;
    if (channel) {
        tags[index].ti_Tag = S2INFO_Channel;
        tags[index++].ti_Data = (IPTR)channel;
    }
    tags[index].ti_Tag = S2INFO_AuthTypes;
    tags[index++].ti_Data = 1u;
    tags[index].ti_Tag = S2INFO_WPAInfo;
    tags[index++].ti_Data = passphrase ? (IPTR)rsn : 0u;
    if (passphrase) {
        tags[index].ti_Tag = S2INFO_Passphrase;
        tags[index++].ti_Data = (IPTR)passphrase;
    }
    tags[index].ti_Tag = S2INFO_WPAInfoLength;
    tags[index++].ti_Data = passphrase ? sizeof(rsn) : 0u;
    tags[index].ti_Tag = TAG_DONE;
    tags[index].ti_Data = 0u;
}

static void make_tags_with_cookie(struct TagItem tags[10],
                                  const UBYTE *ssid,
                                  const UBYTE *passphrase,
                                  ULONG cookie)
{
    ULONG index;
    make_tags(tags, ssid, passphrase, NULL, 0u);
    for (index = 0; index < 9u && tags[index].ti_Tag != TAG_DONE; ++index)
        ;
    CHECK(index < 9u);
    tags[index + 1u] = tags[index];
    tags[index].ti_Tag = S2INFO_AssociationCookie;
    tags[index].ti_Data = cookie;
}

static struct p4wlan_association *queue_profile(
    struct p4wlan_unit *unit, struct p4wlan_opener *opener,
    struct IOSana2Req *request, struct TagItem *tags)
{
    init_request(request, S2_SETOPTIONS, opener);
    request->ios2_Data = tags;
    dispatch_case(unit, opener, request);
    return unit->pending_association;
}

static void init_fullmac_selection(struct Sana2FullMACAssociation *selection,
                                   ULONG profile, ULONG flags)
{
    memset(selection, 0, sizeof(*selection));
    selection->Size = sizeof(*selection);
    selection->Version = S2FULLMAC_VERSION;
    selection->Profile = profile;
    selection->Flags = flags;
    if (profile == S2FULLMAC_PROFILE_SAE_CCMP_PMF) {
        selection->PMFPolicy = S2FULLMAC_PMF_REQUIRED;
        selection->SAEPweMethod = S2FULLMAC_SAE_PWE_H2E;
    } else {
        selection->PMFPolicy = S2FULLMAC_PMF_DISABLED;
    }
}

static void make_fullmac_tags(struct TagItem tags[8], const UBYTE *ssid,
                              const struct Sana2FullMACAssociation *selection,
                              const struct Sana2WirelessCredential *credential,
                              ULONG cookie)
{
    ULONG index = 0u;
    memset(tags, 0, 8u * sizeof(*tags));
    tags[index].ti_Tag = S2INFO_SSID;
    tags[index++].ti_Data = (IPTR)ssid;
    tags[index].ti_Tag = S2INFO_PortType;
    tags[index++].ti_Data = S2PORT_MANAGED;
    tags[index].ti_Tag = S2INFO_FullMACAssociation;
    tags[index++].ti_Data = (IPTR)selection;
    if (credential) {
        tags[index].ti_Tag = S2INFO_WirelessCredential;
        tags[index++].ti_Data = (IPTR)credential;
    }
    tags[index].ti_Tag = S2INFO_AssociationCookie;
    tags[index++].ti_Data = cookie;
    tags[index].ti_Tag = TAG_DONE;
}

#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
static BOOL query_fullmac_capabilities(
    struct p4wlan_unit *unit, struct p4wlan_opener *opener,
    struct Sana2FullMACCapabilities *caps)
{
    struct IOSana2Req request;
    memset(caps, 0, sizeof(*caps));
    caps->SizeAvailable = sizeof(*caps);
    caps->Version = S2FULLMAC_VERSION;
    init_request(&request, S2_GETFULLMACCAPABILITIES, opener);
    request.ios2_StatData = caps;
    dispatch_case(unit, opener, &request);
    return request.ios2_Req.io_Error == 0;
}
#endif

static BOOL supported_command(ULONG command)
{
    ULONG index;
    for (index = 0u; p4wlan_supported_commands[index] != 0u; ++index)
        if (p4wlan_supported_commands[index] == command)
            return TRUE;
    return FALSE;
}

static void test_fullmac_capability_queries_follow_policy_gates(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
#if !P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct Sana2FullMACCapabilities unchanged;
#endif
    struct Sana2WirelessCapabilities legacy;
    struct IOSana2Req request;

    init_fixture(&unit, &opener, &radio);
    CHECK(supported_command(S2_SETOPTIONS));
    CHECK(supported_command(S2_GETWIRELESSCAPABILITIES));
    CHECK(supported_command(S2_GETFULLMACCAPABILITIES) ==
          (BOOL)P4_WIFI_FULLMAC_ADMISSION_ENABLED);
    memset(&caps, 0x5Au, sizeof(caps));
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    caps.SizeAvailable = sizeof(caps);
    caps.Version = S2FULLMAC_VERSION;
#else
    unchanged = caps;
#endif
    init_request(&request, S2_GETFULLMACCAPABILITIES, &opener);
    request.ios2_StatData = &caps;
    dispatch_case(&unit, &opener, &request);
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0u);
    CHECK(caps.SizeAvailable == sizeof(caps));
    CHECK(caps.SizeSupplied == sizeof(caps));
    CHECK(caps.Version == S2FULLMAC_VERSION);
    CHECK(caps.Profiles == (S2FULLMAC_PROFILE_OPEN |
                            S2FULLMAC_PROFILE_WPA2_CCMP |
                            S2FULLMAC_PROFILE_SAE_CCMP_PMF));
    CHECK(caps.SAEPweMethods == (S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
                                 S2FULLMAC_SAE_PWE_H2E |
                                 S2FULLMAC_SAE_PWE_BOTH));
    CHECK(caps.Reserved[0] == 0u && caps.Reserved[1] == 0u);
#if P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED && \
    P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED
    CHECK(caps.Flags == 0u);
#else
    CHECK(caps.Flags == S2FULLMAC_CAPF_DIAGNOSTIC_RISK);
#endif
    CHECK(Sana2FullMACCapabilitiesValid(&caps));

    /* Enabled offers validate exact caller size and version before filling. */
    caps.SizeAvailable = sizeof(caps) - 1u;
    caps.Version = S2FULLMAC_VERSION;
    init_request(&request, S2_GETFULLMACCAPABILITIES, &opener);
    request.ios2_StatData = &caps;
    dispatch_case(&unit, &opener, &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
    CHECK(request.ios2_WireError == S2WERR_BAD_STATDATA);
    caps.SizeAvailable = sizeof(caps);
    caps.Version = S2FULLMAC_VERSION + 1u;
    init_request(&request, S2_GETFULLMACCAPABILITIES, &opener);
    request.ios2_StatData = &caps;
    dispatch_case(&unit, &opener, &request);
    CHECK(request.ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
    CHECK(request.ios2_WireError == S2WERR_BAD_STATDATA);
#else
    CHECK(request.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(memcmp(&caps, &unchanged, sizeof(caps)) == 0);
#endif

    memset(&legacy, 0xA5, sizeof(legacy));
    legacy.SizeAvailable = sizeof(legacy);
    legacy.Version = S2WIRELESS_CAPABILITIES_VERSION;
    init_request(&request, S2_GETWIRELESSCAPABILITIES, &opener);
    request.ios2_StatData = &legacy;
    dispatch_case(&unit, &opener, &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(legacy.SizeAvailable == sizeof(legacy));
    CHECK(legacy.SizeSupplied == sizeof(legacy));
    CHECK(legacy.Version == S2WIRELESS_CAPABILITIES_VERSION);
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    CHECK(legacy.Flags == (S2WIRELESS_CAPF_SECURITY_SUMMARY |
                           S2WIRELESS_CAPF_CONNECT_FAILED));
#else
    CHECK(legacy.Flags == S2WIRELESS_CAPF_SECURITY_SUMMARY);
#endif
    CHECK((legacy.Flags & S2WIRELESS_CAPF_FOURWAY) == 0u);
    CHECK(legacy.Protocols == 0u);
    CHECK(legacy.KeyManagement == 0u);
    CHECK(legacy.PairwiseCiphers == 0u);
    CHECK(legacy.GroupCiphers == 0u);
}

static void test_cookie_copy_and_reply_ack_fencing(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request, flush_request;
    struct TagItem borrowed_tags[10], replacement_tags[10], legacy_tags[10];
    struct p4_wifi_association_target parsed;
    struct p4wlan_association *attempt;
    UBYTE old_ssid[] = "old-max-cookie";
    UBYTE next_ssid[] = "lower-new-cookie";
    const ULONG old_cookie = ~(ULONG)0;
    const ULONG next_cookie = 3u; /* valid, deliberately non-monotonic */
    unsigned calls_before;
    unsigned wipes_before;

    init_fixture(&unit, &opener, &radio);

    /* Preserve the no-cookie ABI wrapper and require an owned clean output. */
    make_tags(legacy_tags, old_ssid, NULL, NULL, 0u);
    CHECK(p4wlan_copy_association_options(&parsed, legacy_tags));
    CHECK(parsed.ssid_length == strlen("old-max-cookie"));
    make_tags_with_cookie(legacy_tags, old_ssid, NULL, old_cookie);
    CHECK(!p4wlan_copy_association_options(&parsed, legacy_tags));
    CHECK(bytes_are_zero((const UBYTE *)&parsed, sizeof(parsed)));

    make_tags_with_cookie(borrowed_tags, old_ssid, NULL, old_cookie);
    make_tags_with_cookie(replacement_tags, next_ssid, NULL, next_cookie);
    init_request(&request, S2_SETOPTIONS, &opener);
    request.ios2_Req.io_Flags = IOF_QUICK;
    request.ios2_Data = borrowed_tags;
    service_before_association_ack = TRUE;
    reuse_request_during_association_reply = TRUE;
    tags_to_reuse_during_reply = borrowed_tags;
    tags_to_reuse_count = 10u;
    replacement_tags_for_reply = replacement_tags;
    calls_before = associate_calls;
    wipes_before = wipe_checks;

    /* The first Reply hook services too early, then reuses this request and
     * tag buffer to submit a new selection before the old frame resumes. */
    dispatch_case(&unit, &opener, &request);
    CHECK(unlocked_association_replies == 2u);
    CHECK(association_reply_cookie_count == 2u);
    CHECK(cookies_seen_at_association_reply[0] == old_cookie);
    CHECK(cookies_seen_at_association_reply[1] == next_cookie);
    CHECK(unacknowledged_service_skips == 1u);
    CHECK(associate_calls == calls_before);
    CHECK(unit.pending_association != NULL);
    attempt = unit.pending_association;
    CHECK(attempt->cookie == next_cookie);
    CHECK(attempt->target.ssid_length == strlen("lower-new-cookie"));
    CHECK(memcmp(attempt->target.ssid, next_ssid,
                 strlen("lower-new-cookie")) == 0);
    CHECK(opener.active_cookie == next_cookie);
    CHECK(opener.failed_cookie == 0u);
    CHECK(!attempt->acknowledged); /* older Reply token cannot ACK replacement */
    CHECK(request.ios2_Req.io_Command == 0xDEADu);
    CHECK(request.ios2_Data == NULL && request.ios2_BufferManagement == NULL);
    CHECK(bytes_are_zero((const UBYTE *)borrowed_tags,
                         sizeof(borrowed_tags)));
    CHECK(bytes_are_zero((const UBYTE *)replacement_tags,
                         sizeof(replacement_tags)));
    CHECK(wipe_checks == wipes_before + 1u); /* superseded attempt retired */

    /* The unacknowledged replacement remains invisible to radio service. Once
     * its own ACK is released, its failure is bound to cookie 3, never max. */
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(associate_calls == calls_before);
    attempt->acknowledged = TRUE; /* finish the modeled replacement ACK */
    associate_returns_success = FALSE;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(unit.pending_association == NULL);
    CHECK(opener.active_cookie == 0u);
    CHECK(opener.failed_cookie == next_cookie);
    CHECK(opener.failed_cookie != old_cookie);
    CHECK(unit.pending_events == 0u);

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.active_cookie == 0u && opener.failed_cookie == 0u);
    CHECK(unit.pending_association == NULL);
}

static void seed_committed_link(struct p4wlan_unit *unit,
                                struct p4wlan_opener *opener,
                                struct P4WifiBase *radio)
{
    static const UBYTE ssid[] = "existing";
    unit->link_committed = TRUE;
    unit->link_opener = opener;
    ++opener->refs;
    unit->link_info.ssid_length = sizeof(ssid) - 1u;
    memcpy(unit->link_info.ssid, ssid, sizeof(ssid));
    unit->link_info.bssid[0] = 0x02u;
    unit->link_info.bssid[5] = 1u;
    unit->link_info.channel = 1u;
    unit->link_info.authmode = 0u;
    radio->associated = TRUE;
}

static void add_opener(struct p4wlan_unit *unit,
                       struct p4wlan_opener *opener)
{
    memset(opener, 0, sizeof(*opener));
    NEWLIST(&opener->requests);
    ADDTAIL((struct List *)&unit->openers, (struct Node *)&opener->unit_node);
    opener->refs = 1u; /* Close's base pin */
}

static struct p4wlan_request *queue_event_waiter(
    struct p4wlan_unit *unit, struct p4wlan_opener *opener,
    struct IOSana2Req *request, ULONG event_mask)
{
    struct p4wlan_request *item =
        AllocVec(sizeof(*item), MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(item != NULL);
    init_request(request, S2_ONEVENT, opener);
    item->opener = opener;
    item->request = request;
    item->kind = P4WLAN_REQUEST_EVENT;
    item->state = P4WLAN_REQUEST_QUEUED;
    item->event_mask = event_mask;
    ADDTAIL((struct List *)&opener->requests,
            (struct Node *)&item->owner_node);
    ++opener->refs;
    ++opener->request_count;
    ++opener->event_count;
    ++unit->request_count;
    ++unit->event_count;
    return item;
}

static void test_onevent_dispatch_event_rearm_and_abort(void)
{
    struct esp32p4wlan_base base;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct MinList done;
    unsigned replies_before;

    init_fixture(&base.unit, &opener, &radio);
    base.unit.initialized = TRUE;

    init_request(&request, S2_ONEVENT, &opener);
    request.ios2_Req.io_Flags = IOF_QUICK;
    request.ios2_WireError = S2EVENT_CONNECT;
    replies_before = reply_calls;
    dispatch_case(&base.unit, &opener, &request);

    /* BeginIO must leave an accepted waiter owned and parked. */
    CHECK(reply_calls == replies_before);
    CHECK(!(request.ios2_Req.io_Flags & IOF_QUICK));
    CHECK(request.ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0u);
    CHECK(base.unit.request_count == 1u && opener.request_count == 1u);
    CHECK(base.unit.event_count == 1u && opener.event_count == 1u);
    CHECK(opener.refs == 2u);

    /* A production event consumes this waiter and replies exactly once. */
    NEWLIST(&done);
    ObtainSemaphore(&base.unit.lock);
    p4wlan_event_locked(&base.unit, S2EVENT_CONNECT, &done);
    ReleaseSemaphore(&base.unit.lock);
    p4wlan_finish_done(&base.unit, &done);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == S2EVENT_CONNECT);
    CHECK(base.unit.pending_events == 0u);
    CHECK(base.unit.request_count == 0u && opener.request_count == 0u);
    CHECK(base.unit.event_count == 0u && opener.event_count == 0u);
    CHECK(opener.refs == 1u);

    /* Reusing the completed IORequest arms a fresh wait and must park again. */
    request.ios2_Req.io_Flags = IOF_QUICK;
    replies_before = reply_calls;
    dispatch_case(&base.unit, &opener, &request);
    CHECK(reply_calls == replies_before);
    CHECK(!(request.ios2_Req.io_Flags & IOF_QUICK));
    CHECK(base.unit.request_count == 1u && opener.request_count == 1u);
    CHECK(base.unit.event_count == 1u && opener.event_count == 1u);
    CHECK(opener.refs == 2u);

    /* Exercise the extracted production AbortIO body on the rearmed wait. */
    request.ios2_Req.io_Message.mn_Length = sizeof(request);
    request.ios2_Req.io_Device = (struct Device *)&base;
    request.ios2_Req.io_Unit = (struct Unit *)&base.unit;
    CHECK(dispatch_abort(&base, (struct IORequest *)&request) == 0);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(request.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(base.unit.request_count == 0u && opener.request_count == 0u);
    CHECK(base.unit.event_count == 0u && opener.event_count == 0u);
    CHECK(opener.refs == 1u);
    CHECK(dispatch_abort(&base, (struct IORequest *)&request) == -1);
    CHECK(reply_calls == replies_before + 1u);
}

static void test_onevent_rejects_invalid_mask_and_full_capacity(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req pending[P4WLAN_MAX_EVENTS];
    struct IOSana2Req request;
    struct IOSana2Req flush_request;
    unsigned replies_before;
    unsigned allocations_before;
    unsigned frees_before;
    ULONG index;

    init_fixture(&unit, &opener, &radio);

    init_request(&request, S2_ONEVENT, &opener);
    request.ios2_WireError = S2EVENT_CONNECT | 0x80000000u;
    replies_before = reply_calls;
    allocations_before = allocation_calls;
    dispatch_case(&unit, &opener, &request);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(request.ios2_Req.io_Error == S2ERR_NOT_SUPPORTED);
    CHECK(request.ios2_WireError == S2WERR_BAD_EVENT);
    CHECK(allocation_calls == allocations_before);
    CHECK(unit.request_count == 0u && opener.request_count == 0u);
    CHECK(unit.event_count == 0u && opener.event_count == 0u);
    CHECK(opener.refs == 1u);

    for (index = 0; index < P4WLAN_MAX_EVENTS; ++index) {
        init_request(&pending[index], S2_ONEVENT, &opener);
        pending[index].ios2_Req.io_Flags = IOF_QUICK;
        pending[index].ios2_WireError = S2EVENT_CONNECT;
        replies_before = reply_calls;
        dispatch_case(&unit, &opener, &pending[index]);
        CHECK(reply_calls == replies_before);
        CHECK(!(pending[index].ios2_Req.io_Flags & IOF_QUICK));
    }
    CHECK(unit.event_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.event_count == P4WLAN_MAX_EVENTS);
    CHECK(unit.request_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.request_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.refs == 1u + P4WLAN_MAX_EVENTS);

    init_request(&request, S2_ONEVENT, &opener);
    request.ios2_WireError = S2EVENT_CONNECT;
    replies_before = reply_calls;
    allocations_before = allocation_calls;
    frees_before = free_calls;
    dispatch_case(&unit, &opener, &request);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(allocation_calls == allocations_before + 1u);
    CHECK(free_calls == frees_before + 1u);
    CHECK(unit.event_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.event_count == P4WLAN_MAX_EVENTS);
    CHECK(unit.request_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.request_count == P4WLAN_MAX_EVENTS);
    CHECK(opener.refs == 1u + P4WLAN_MAX_EVENTS);

    init_request(&flush_request, CMD_FLUSH, &opener);
    replies_before = reply_calls;
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(reply_calls == replies_before + P4WLAN_MAX_EVENTS + 1u);
    for (index = 0; index < P4WLAN_MAX_EVENTS; ++index) {
        CHECK(pending[index].ios2_Req.io_Error == IOERR_ABORTED);
        CHECK(pending[index].ios2_WireError == S2WERR_GENERIC_ERROR);
    }
    CHECK(unit.request_count == 0u && opener.request_count == 0u);
    CHECK(unit.event_count == 0u && opener.event_count == 0u);
    CHECK(opener.refs == 1u);
}

static void seed_typed_committed_link(struct p4wlan_unit *unit,
                                      struct p4wlan_opener *opener,
                                      struct P4WifiBase *radio,
                                      ULONG cookie)
{
    seed_committed_link(unit, opener, radio);
    unit->link_cookie = cookie;
    unit->link_fullmac = TRUE;
    opener->fullmac_events = TRUE;
    radio->observed_ap = unit->link_info;
    radio->link_state = P4_WIFI_LINK_UP;
}

static void make_disassociate_tags(struct TagItem tags[2])
{
    tags[0].ti_Tag = S2INFO_Disassociate;
    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0u;
}

static void exercise_legacy_cookie_failure_leave_latch(BOOL faulted,
                                                        ULONG cookie)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req associate_request, failure_event, leave_request;
    struct IOSana2Req flush_request;
    struct TagItem tags[10], leave_tags[2];
    struct p4wlan_association *attempt;
    UBYTE ssid[] = "legacy-cookie-failure";
    ULONG latch_cookie = cookie + 1u;
    unsigned prior_disconnect_calls;

    init_fixture(&unit, &opener, &radio);
    (void)queue_event_waiter(&unit, &opener, &failure_event,
                             S2EVENT_CONNECT_FAILED);
    make_tags_with_cookie(tags, ssid, NULL, cookie);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);
    CHECK(attempt != NULL && attempt->acknowledged);
    CHECK(attempt->target.fullmac_profile == 0u);
    CHECK(attempt->cookie == cookie && !opener.fullmac_events);

    /* A cause-free legacy-cookie failure is associated only with this
     * acknowledged attempt's copied cookie; it says nothing about AP cause. */
    associate_returns_success = FALSE;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(failure_event.ios2_Req.io_Error == 0);
    CHECK(failure_event.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(failure_event.ios2_DataLength == cookie);
    CHECK(opener.active_cookie == 0u && opener.failed_cookie == 0u);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.pending_events == 0u);

    /* Leave also retires a failure that arrived before the caller rearmed its
     * event request. Exercise both healthy teardown admission and faulted
     * local cancellation through the actual S2_SETOPTIONS tag path. */
    make_tags_with_cookie(tags, ssid, NULL, latch_cookie);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);
    CHECK(attempt != NULL && attempt->cookie == latch_cookie);
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(opener.failed_cookie == latch_cookie);
    CHECK(opener.active_cookie == 0u);

    if (faulted) unit.callback_fault = TRUE;
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &opener);
    leave_request.ios2_Data = leave_tags;
    prior_disconnect_calls = disconnect_calls;
    dispatch_case(&unit, &opener, &leave_request);
    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(opener.failed_cookie == 0u && opener.active_cookie == 0u);
    CHECK(!opener.fullmac_events);
    if (faulted) {
        CHECK(unit.callback_fault && !unit.teardown_pending);
        CHECK(disconnect_calls == prior_disconnect_calls);
    } else {
        CHECK(!unit.callback_fault && unit.teardown_pending);
        CHECK(disconnect_calls == prior_disconnect_calls);
        CHECK(p4wlan_maintain_link(&unit, &radio));
        CHECK(!unit.teardown_pending);
        CHECK(disconnect_calls == prior_disconnect_calls + 1u);
    }
    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u);
}

static void test_legacy_cookie_failure_and_leave_clear_latch(void)
{
    exercise_legacy_cookie_failure_leave_latch(FALSE, 0xFA110050u);
    exercise_legacy_cookie_failure_leave_latch(TRUE, 0xFA110060u);
}

static void test_beginio_refusals_preserve_selection_and_link(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req old_request, refused_request;
    struct TagItem old_tags[10], refused_tags[10];
    UBYTE old_ssid[] = "prior-net";
    UBYTE new_ssid[] = "next-net";
    UBYTE old_password[] = "prior-password";
    UBYTE new_password[] = "next-password";
    struct p4wlan_association *old_attempt;
    ULONG old_token;
    ULONG old_refs;
    struct p4_wifi_link_info old_link;
    unsigned before_reply;

    init_fixture(&unit, &opener, &radio);
    make_tags(old_tags, old_ssid, old_password, NULL, 0u);
    old_attempt = queue_profile(&unit, &opener, &old_request, old_tags);
    CHECK(old_attempt != NULL);
    old_token = unit.selection_token;
    old_refs = opener.refs;
    before_reply = reply_calls;

    make_tags(refused_tags, new_ssid, new_password, NULL, 0u);
    fail_next_vector_allocation = TRUE;
    init_request(&refused_request, S2_SETOPTIONS, &opener);
    refused_request.ios2_Data = refused_tags;
    dispatch_case(&unit, &opener, &refused_request);
    CHECK(refused_request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(unit.pending_association == old_attempt);
    CHECK(old_attempt->target.ssid_length == strlen("prior-net"));
    CHECK(memcmp(old_attempt->target.ssid, old_ssid,
                 strlen("prior-net")) == 0);
    CHECK(memcmp(old_attempt->target.password, old_password,
                 strlen("prior-password")) == 0);
    CHECK(unit.selection_token == old_token && opener.refs == old_refs);
    CHECK(reply_calls == before_reply + 1u);

    /* Duplicate SSID tag is refused after allocating a new candidate. */
    make_tags(refused_tags, new_ssid, new_password, NULL, 0u);
    refused_tags[7] = refused_tags[6];
    refused_tags[6].ti_Tag = S2INFO_SSID;
    refused_tags[6].ti_Data = (IPTR)new_ssid;
    init_request(&refused_request, S2_SETOPTIONS, &opener);
    refused_request.ios2_Data = refused_tags;
    dispatch_case(&unit, &opener, &refused_request);
    CHECK(refused_request.ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
    CHECK(unit.pending_association == old_attempt);
    CHECK(memcmp(old_attempt->target.ssid, old_ssid,
                 strlen("prior-net")) == 0);
    CHECK(memcmp(old_attempt->target.password, old_password,
                 strlen("prior-password")) == 0);
    CHECK(unit.selection_token == old_token && opener.refs == old_refs);

    /* Once a link is committed, the privacy gate and saturated token both
     * refuse before changing the link snapshot or its owner pin. */
    ObtainSemaphore(&unit.lock);
    p4wlan_invalidate_selection_locked(&unit);
    ReleaseSemaphore(&unit.lock);
    CHECK(opener.refs == 1u);
    seed_committed_link(&unit, &opener, &radio);
    old_link = unit.link_info;
    old_refs = opener.refs;
    radio.association_enabled = FALSE;
    init_request(&refused_request, S2_SETOPTIONS, &opener);
    refused_request.ios2_Data = refused_tags;
    dispatch_case(&unit, &opener, &refused_request);
    CHECK(refused_request.ios2_Req.io_Error == S2ERR_OUTOFSERVICE);
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(opener.refs == old_refs);
    CHECK(memcmp(&unit.link_info, &old_link, sizeof(old_link)) == 0);

    radio.association_enabled = TRUE;
    unit.selection_token = ~(ULONG)0;
    init_request(&refused_request, S2_SETOPTIONS, &opener);
    refused_request.ios2_Data = refused_tags;
    dispatch_case(&unit, &opener, &refused_request);
    CHECK(refused_request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(unit.selection_token == ~(ULONG)0);
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(opener.refs == old_refs);
    CHECK(memcmp(&unit.link_info, &old_link, sizeof(old_link)) == 0);
    ObtainSemaphore(&unit.lock);
    p4wlan_invalidate_selection_locked(&unit);
    ReleaseSemaphore(&unit.lock);
    CHECK(opener.refs == 1u);
}

static void test_replacement_owns_and_wipes_attempt(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req first_request, second_request;
    struct TagItem first_tags[10], second_tags[10];
    UBYTE first_ssid[] = "first-network";
    UBYTE second_ssid[] = "second-network";
    UBYTE first_password[] = "first-secret";
    UBYTE second_password[] = "second-secret";
    UBYTE bssid[] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au };
    struct p4wlan_association *first_attempt, *second_attempt;
    ULONG token_before;
    uintptr_t first_address;
    unsigned old_wipes;

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    make_tags(first_tags, first_ssid, first_password, bssid, 6u);
    first_attempt = queue_profile(&unit, &opener, &first_request, first_tags);
    CHECK(first_attempt != NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(opener.refs == 2u); /* owner plus queued attempt */
    CHECK(first_attempt->target.password_length == strlen("first-secret"));
    CHECK(memcmp(first_attempt->target.password, first_password,
                 strlen("first-secret")) == 0);
    token_before = unit.selection_token;
    first_address = (uintptr_t)first_attempt;
    old_wipes = wipe_checks;

    /* The request has already replied; destroy all borrowed input and request
     * bytes before submitting a replacement. */
    memset(first_tags, 0xA5, sizeof(first_tags));
    memset(first_ssid, 'X', sizeof(first_ssid) - 1u);
    memset(first_password, 'Y', sizeof(first_password) - 1u);
    memset(&first_request, 0xA5, sizeof(first_request));
    make_tags(second_tags, second_ssid, second_password, bssid, 6u);
    second_attempt = queue_profile(&unit, &opener, &second_request,
                                   second_tags);
    CHECK(second_attempt != NULL && (uintptr_t)second_attempt != first_address);
    CHECK(unit.pending_association == second_attempt);
    CHECK(second_attempt->token == token_before + 1u);
    CHECK(unit.selection_token == token_before + 1u);
    CHECK(second_attempt->unit == &unit && second_attempt->opener == &opener);
    CHECK(second_attempt->target.ssid_length == strlen("second-network"));
    CHECK(memcmp(second_attempt->target.ssid, second_ssid,
                 strlen("second-network")) == 0);
    CHECK(memcmp(second_attempt->target.password, second_password,
                 strlen("second-secret")) == 0);
    CHECK(opener.refs == 2u);
    CHECK(wipe_checks == old_wipes + 1u); /* replacement wiped before free */
    {
        struct IOSana2Req flush_request;
        init_request(&flush_request, CMD_FLUSH, &opener);
        dispatch_case(&unit, &opener, &flush_request);
    }
    CHECK(unit.pending_association == NULL && opener.refs == 1u);
}

static void test_service_commit_and_owned_network_snapshot(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request, info_request;
    struct TagItem tags[10];
    UBYTE ssid[] = "confirmed-net";
    UBYTE password[] = "correct horse";
    UBYTE bssid[] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au };
    struct p4wlan_network_publication *publication;
    struct p4wlan_association *attempt;
    ULONG committed_token;
    unsigned prior_associate_calls;

    init_fixture(&unit, &opener, &radio);
    make_tags(tags, ssid, password, bssid, 6u);
    attempt = queue_profile(&unit, &opener, &request, tags);
    CHECK(attempt != NULL && opener.refs == 2u);
    committed_token = attempt->token;
    prior_associate_calls = associate_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(associate_calls == prior_associate_calls + 1u);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(unit.selection_token == committed_token);
    CHECK(radio.associated);
    CHECK(unit.link_info.ssid_length == strlen("confirmed-net"));
    CHECK(memcmp(unit.link_info.ssid, ssid, strlen("confirmed-net")) == 0);
    CHECK(memcmp(unit.link_info.bssid, bssid, sizeof(bssid)) == 0);
    CHECK(unit.link_info.channel == 6u && unit.link_info.authmode == 3u);
    CHECK(bytes_are_zero((const UBYTE *)&radio.confirmed_ap,
                         sizeof(radio.confirmed_ap)));
    CHECK(unit.pending_events == S2EVENT_CONNECT);
    CHECK(opener.refs == 2u); /* committed link pin replaces attempt pin */

    init_request(&info_request, S2_GETNETWORKINFO, &opener);
    info_request.ios2_Data = &info_request; /* pool identity, not dereferenced */
    dispatch_case(&unit, &opener, &info_request);
    CHECK(info_request.ios2_Req.io_Error == 0);
    CHECK(info_request.ios2_StatData != NULL);
    publication = (struct p4wlan_network_publication *)pool_memory;
    CHECK(publication != NULL);
    CHECK(info_request.ios2_StatData == publication->tags);
    CHECK(publication->tags[0].ti_Tag == S2INFO_SSID);
    CHECK(publication->tags[0].ti_Data == (IPTR)publication->info.ssid);
    CHECK(publication->tags[1].ti_Tag == S2INFO_BSSID);
    CHECK(publication->tags[1].ti_Data == (IPTR)publication->info.bssid);
    CHECK(publication->tags[2].ti_Tag == S2INFO_Channel);
    CHECK(publication->tags[2].ti_Data == 6u);
    CHECK(publication->tags[3].ti_Tag == S2INFO_PortType);
    CHECK(publication->tags[3].ti_Data == S2PORT_MANAGED);
    CHECK(publication->tags[4].ti_Tag == TAG_DONE);
    CHECK(publication->info.authmode == 3u);
    CHECK(memcmp(publication->info.ssid, ssid, strlen("confirmed-net")) == 0);

    memset(unit.link_info.ssid, 'Z', sizeof(unit.link_info.ssid));
    CHECK(memcmp(publication->info.ssid, ssid, strlen("confirmed-net")) == 0);
    FreePooled(info_request.ios2_Data, publication, sizeof(*publication));

    unit.link_committed = FALSE;
    init_request(&info_request, S2_GETNETWORKINFO, &opener);
    info_request.ios2_Data = &info_request;
    dispatch_case(&unit, &opener, &info_request);
    CHECK(info_request.ios2_Req.io_Error == S2ERR_OUTOFSERVICE);
    CHECK(info_request.ios2_StatData == NULL);

    unit.link_committed = TRUE;
    fail_next_pool_allocation = TRUE;
    init_request(&info_request, S2_GETNETWORKINFO, &opener);
    info_request.ios2_Data = &info_request;
    dispatch_case(&unit, &opener, &info_request);
    CHECK(info_request.ios2_Req.io_Error == S2ERR_NO_RESOURCES);
    CHECK(info_request.ios2_StatData == NULL);
    {
        struct IOSana2Req flush_request;
        init_request(&flush_request, CMD_FLUSH, &opener);
        dispatch_case(&unit, &opener, &flush_request);
    }
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(opener.refs == 1u);
}

static void test_typed_association_connect_is_cookie_local(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req associate_request, origin_event, foreign_event;
    struct IOSana2Req flush_request;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *foreign_waiter;
    UBYTE ssid[] = "typed-connect";
    const ULONG cookie = 0x12345678u;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &opener, &caps));
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, cookie);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);
    CHECK(attempt != NULL && attempt->cookie == cookie);
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_OPEN);
    (void)queue_event_waiter(&unit, &opener, &origin_event,
                             S2EVENT_CONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_CONNECT);
    prior_replies = reply_calls;

    CHECK(p4wlan_associate_one(&unit, &radio));

    CHECK(reply_calls == prior_replies + 1u);
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(opener.fullmac_events);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_cookie == cookie && unit.link_opener == &opener);
    CHECK(unit.pending_events == 0u); /* no unit-wide typed CONNECT latch */
    CHECK(opener.refs == 2u); /* base plus committed-link pin */
    CHECK(foreign.refs == 2u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(foreign.refs == 1u && foreign.event_count == 0u);
    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u && !unit.link_committed);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac);
#endif
}

static void test_fullmac_admission_policy_and_owned_sae_credential(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    struct IOSana2Req associate_request, connect_event, foreign_event;
    struct IOSana2Req flush_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *foreign_waiter;
    UBYTE ssid[] = "typed-sae-owned";
    UBYTE secret[] = "typed-sae-password";
    UBYTE expected_secret[sizeof(secret)];
    const ULONG cookie = 0x53414501u;
    unsigned prior_wipes;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    memcpy(expected_secret, secret, sizeof(secret));
    memset(&credential, 0, sizeof(credential));
    credential.Size = sizeof(credential);
    credential.Version = S2WIRELESS_CREDENTIAL_VERSION;
    credential.Kind = S2WIRELESS_CRED_SAE_PASSWORD;
    credential.Length = sizeof(secret) - 1u;
    credential.Data = secret;
    prior_wipes = wipe_checks;

#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    CHECK(query_fullmac_capabilities(&unit, &opener, &caps));
    CHECK(caps.Profiles & S2FULLMAC_PROFILE_SAE_CCMP_PMF);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF, 0u);
    make_fullmac_tags(tags, ssid, &selection, &credential, cookie);

    if (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) {
        /* A negotiated diagnostic offer still refuses an attempt without
         * explicit, per-selection consent. The refused allocation is wiped. */
        attempt = queue_profile(&unit, &opener, &associate_request, tags);
        CHECK(attempt == NULL);
        CHECK(associate_request.ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
        CHECK(unit.selection_token == 10u);
        CHECK(unit.pending_association == NULL);
        CHECK(!opener.fullmac_events && opener.active_cookie == 0u);
        CHECK(wipe_checks == prior_wipes + 1u);
        selection.Flags = S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK;
        make_fullmac_tags(tags, ssid, &selection, &credential, cookie);
        prior_wipes = wipe_checks;
    }

    (void)queue_event_waiter(&unit, &opener, &connect_event,
                             S2EVENT_CONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_CONNECT);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);
    CHECK(attempt != NULL);
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(attempt->cookie == cookie);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_SAE_CCMP_PMF);
    CHECK(attempt->target.authmode == 6u);
    CHECK(attempt->target.pmf_required);
    CHECK(attempt->target.sae_pwe_h2e == 2u);
    CHECK(attempt->target.password_length == sizeof(secret) - 1u);
    CHECK(memcmp(attempt->target.password, expected_secret,
                 sizeof(secret) - 1u) == 0);
    CHECK(memcmp(secret, expected_secret, sizeof(secret)) == 0);
    CHECK(opener.fullmac_events);
    CHECK(unit.pending_association == attempt);

    /* BeginIO has replied; mutating borrowed credential storage cannot alter
     * the device-owned target used by the asynchronous radio operation. */
    secret[0] = 'X';
    CHECK(attempt->target.password[0] == expected_secret[0]);
    CHECK(memcmp(attempt->target.password, expected_secret,
                 sizeof(secret) - 1u) == 0);
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(connect_event.ios2_Req.io_Error == 0);
    CHECK(connect_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(connect_event.ios2_DataLength == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_cookie == cookie && unit.link_opener == &opener);
    CHECK(unit.pending_events == 0u);
    CHECK(opener.refs == 2u && foreign.refs == 2u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u && foreign.refs == 1u);
#else
    /* Even test-risk opt-in cannot bypass the unqualified peer contract gate. */
    (void)caps;
    (void)connect_event;
    (void)foreign_event;
    (void)flush_request;
    (void)foreign_waiter;
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                           S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK);
    make_fullmac_tags(tags, ssid, &selection, &credential, cookie);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);
    CHECK(attempt == NULL);
    CHECK(associate_request.ios2_Req.io_Error == S2ERR_BAD_ARGUMENT);
    CHECK(unit.selection_token == 10u);
    CHECK(unit.pending_association == NULL);
    CHECK(!opener.fullmac_events && opener.active_cookie == 0u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(secret[0] == 't');
#endif
}

static void test_legacy_selection_restores_prior_opener_broadcast(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener typed_opener, legacy_opener;
    struct P4WifiBase radio;
    struct IOSana2Req origin_disconnect, origin_connect;
    struct IOSana2Req foreign_disconnect, associate_request, flush_request;
    struct TagItem tags[10];
    UBYTE ssid[] = "legacy-replacement";
    struct p4wlan_request *foreign_waiter;
    struct p4wlan_association *attempt;
    const ULONG cookie = 0xACCE5510u;
    const ULONG sentinel_length = 0xD00DFEEDu;
    unsigned prior_replies;

    init_fixture(&unit, &typed_opener, &radio);
    add_opener(&unit, &legacy_opener);
    seed_typed_committed_link(&unit, &typed_opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &typed_opener, &origin_disconnect,
                             S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &typed_opener, &origin_connect,
                             S2EVENT_CONNECT);
    origin_connect.ios2_DataLength = sentinel_length;
    foreign_waiter = queue_event_waiter(&unit, &legacy_opener,
                                        &foreign_disconnect,
                                        S2EVENT_DISCONNECT);
    make_tags(tags, ssid, NULL, NULL, 0u);
    attempt = queue_profile(&unit, &legacy_opener, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &legacy_opener);
    CHECK(typed_opener.fullmac_events);
    CHECK(!legacy_opener.fullmac_events);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(unit.teardown_opener == &typed_opener);
    CHECK(unit.teardown_cookie == cookie);
    CHECK(!unit.link_committed && unit.link_cookie == 0u);
    CHECK(typed_opener.refs == 4u); /* base, two waiters, teardown pin */

    /* The old owner receives its typed terminal before the replacement may
     * start; the new legacy owner's waiter is not part of that event. */
    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(!unit.teardown_pending);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(origin_disconnect.ios2_Req.io_Error == 0);
    CHECK(origin_disconnect.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_disconnect.ios2_DataLength == cookie);
    CHECK(!typed_opener.fullmac_events);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(typed_opener.refs == 2u); /* base plus pending legacy CONNECT */
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_disconnect.ios2_WireError == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));

    CHECK(reply_calls == prior_replies + 1u);
    CHECK(origin_connect.ios2_Req.io_Error == 0);
    CHECK(origin_connect.ios2_WireError == S2EVENT_CONNECT);
    CHECK(origin_connect.ios2_DataLength == sentinel_length);
    CHECK(!typed_opener.fullmac_events);
    CHECK(unit.link_committed && !unit.link_fullmac);
    CHECK(unit.link_opener == &legacy_opener && unit.link_cookie == 0u);
    CHECK(unit.pending_events == 0u);
    CHECK(typed_opener.refs == 1u && legacy_opener.refs == 3u);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);

    init_request(&flush_request, CMD_FLUSH, &legacy_opener);
    dispatch_case(&unit, &legacy_opener, &flush_request);
    CHECK(legacy_opener.refs == 1u && !unit.link_committed);
}

static void test_same_opener_legacy_replacement_waits_for_disconnect(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req disconnect_event, connect_event, foreign_event;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[10];
    UBYTE ssid[] = "same-opener-legacy";
    struct p4wlan_request *foreign_waiter;
    struct p4wlan_association *attempt;
    const ULONG cookie = 0x5A6E0007u;
    const ULONG sentinel_length = 0xB16B00B5u;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &opener, &disconnect_event,
                             S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &opener, &connect_event,
                             S2EVENT_CONNECT);
    connect_event.ios2_DataLength = sentinel_length;
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    make_tags(tags, ssid, NULL, NULL, 0u);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);

    CHECK(attempt != NULL && attempt->opener == &opener);
    CHECK(attempt->target.fullmac_profile == 0u);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(unit.teardown_opener == &opener && unit.teardown_cookie == cookie);
    CHECK(opener.fullmac_events); /* retained only while its tombstone lives */
    CHECK(unit.pending_association == attempt);
    CHECK(opener.refs == 5u); /* base, two waiters, attempt and teardown pins */
    CHECK(!unit.link_committed && unit.link_cookie == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(reply_calls == prior_replies + 1u);
    CHECK(disconnect_event.ios2_Req.io_Error == 0);
    CHECK(disconnect_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(disconnect_event.ios2_DataLength == cookie);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(!opener.fullmac_events);
    CHECK(unit.pending_association == attempt);
    CHECK(opener.refs == 3u); /* base, next request, pending connect waiter */
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(unit.pending_events == 0u);

    /* The next service pass now starts the pending legacy attempt. */
    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));

    CHECK(reply_calls == prior_replies + 1u);
    CHECK(connect_event.ios2_Req.io_Error == 0);
    CHECK(connect_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(connect_event.ios2_DataLength == sentinel_length);
    CHECK(!opener.fullmac_events);
    CHECK(unit.link_committed && !unit.link_fullmac);
    CHECK(unit.link_opener == &opener && unit.link_cookie == 0u);
    CHECK(opener.refs == 2u); /* base plus new committed-link pin */
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(unit.pending_events == 0u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u && foreign.refs == 1u);
}

static void test_typed_pending_association_survives_old_teardown(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req disconnect_event, connect_event, foreign_event;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *foreign_waiter;
    UBYTE ssid[] = "typed-during-teardown";
    const ULONG old_cookie = 0x70D00001u;
    const ULONG next_cookie = 0x70D00002u;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &opener, &caps));
    seed_typed_committed_link(&unit, &opener, &radio, old_cookie);
    (void)queue_event_waiter(&unit, &opener, &disconnect_event,
                             S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &opener, &connect_event,
                             S2EVENT_CONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, next_cookie);
    attempt = queue_profile(&unit, &opener, &associate_request, tags);

    CHECK(attempt != NULL && attempt->opener == &opener);
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(attempt->cookie == next_cookie);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_OPEN);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(unit.teardown_opener == &opener);
    CHECK(unit.teardown_cookie == old_cookie);
    CHECK(opener.fullmac_events);
    CHECK(unit.pending_association == attempt);
    CHECK(opener.refs == 5u); /* base, two waiters, attempt and tombstone pins */

    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(disconnect_event.ios2_Req.io_Error == 0);
    CHECK(disconnect_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(disconnect_event.ios2_DataLength == old_cookie);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(unit.teardown_cookie == 0u && unit.teardown_token == 0u);
    CHECK(unit.pending_association == attempt);
    CHECK(opener.fullmac_events); /* the new typed attempt owns the mode now */
    CHECK(opener.refs == 3u); /* base, connect waiter and pending attempt */
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(connect_event.ios2_Req.io_Error == 0);
    CHECK(connect_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(connect_event.ios2_DataLength == next_cookie);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_opener == &opener && unit.link_cookie == next_cookie);
    CHECK(opener.fullmac_events);
    CHECK(opener.refs == 2u); /* base and new committed-link pin */
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u && foreign.refs == 1u);
#endif
}

static void test_foreign_disassociate_preserves_typed_pending_owner(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req associate_request, connect_event, foreign_event;
    struct IOSana2Req leave_request, flush_request;
    struct TagItem tags[8], leave_tags[2];
    struct p4wlan_association *attempt;
    struct p4wlan_request *foreign_waiter;
    UBYTE ssid[] = "pending-owned-selection";
    const ULONG cookie = 0xD1550001u;
    ULONG token;
    unsigned prior_replies;

    init_fixture(&unit, &owner, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &owner, &caps));
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, cookie);
    (void)queue_event_waiter(&unit, &owner, &connect_event, S2EVENT_CONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    attempt = queue_profile(&unit, &owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &owner);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_OPEN);
    CHECK(attempt->cookie == cookie && attempt->acknowledged);
    token = unit.selection_token;
    CHECK(token == attempt->token);
    CHECK(owner.fullmac_events && owner.active_cookie == cookie);

    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &foreign);
    leave_request.ios2_Data = leave_tags;
    prior_replies = reply_calls;
    dispatch_case(&unit, &foreign, &leave_request);

    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(unit.selection_token == token);
    CHECK(unit.pending_association == attempt);
    CHECK(attempt->opener == &owner && attempt->acknowledged);
    CHECK(owner.active_cookie == cookie && owner.fullmac_events);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(foreign.active_cookie == 0u && !foreign.fullmac_events);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(connect_event.ios2_Req.io_Error == 0);
    CHECK(connect_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(connect_event.ios2_DataLength == cookie);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_opener == &owner && unit.link_cookie == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);

    init_request(&flush_request, CMD_FLUSH, &owner);
    dispatch_case(&unit, &owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(owner.refs == 1u && foreign.refs == 1u);
#endif
}

static void test_foreign_disassociate_preserves_typed_active_rpc(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req associate_request, connect_event, foreign_event;
    struct IOSana2Req leave_request, flush_request;
    struct TagItem tags[8], leave_tags[2];
    struct p4wlan_request *foreign_waiter;
    struct p4wlan_association *attempt;
    UBYTE ssid[] = "active-owned-selection";
    const ULONG cookie = 0xD1550010u;
    ULONG token;
    unsigned prior_replies, prior_associate_calls;

    init_fixture(&unit, &owner, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &owner, &caps));
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, cookie);
    (void)queue_event_waiter(&unit, &owner, &connect_event, S2EVENT_CONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    attempt = queue_profile(&unit, &owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &owner);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_OPEN);
    CHECK(attempt->cookie == cookie && attempt->acknowledged);
    token = attempt->token;

    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &foreign);
    disassociate_requester = &foreign;
    disassociate_request = &leave_request;
    disassociate_tags = leave_tags;
    disassociate_cancel_checks = 0u;
    disassociate_during_associate = TRUE;
    prior_replies = reply_calls;
    prior_associate_calls = associate_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));

    CHECK(!disassociate_during_associate);
    CHECK(disassociate_cancel_checks == 1u);
    CHECK(associate_calls == prior_associate_calls + 1u);
    CHECK(reply_calls == prior_replies + 2u);
    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(unit.selection_token == token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_opener == &owner && unit.link_cookie == cookie);
    CHECK(owner.fullmac_events && owner.active_cookie == 0u);
    CHECK(connect_event.ios2_Req.io_Error == 0);
    CHECK(connect_event.ios2_WireError == S2EVENT_CONNECT);
    CHECK(connect_event.ios2_DataLength == cookie);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);

    disassociate_requester = NULL;
    disassociate_request = NULL;
    disassociate_tags = NULL;
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &owner);
    dispatch_case(&unit, &owner, &flush_request);
    CHECK(owner.refs == 1u && foreign.refs == 1u);
#endif
}

static void test_typed_attempt_fault_reports_exact_failure_cookie(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req associate_request, connect_event, failure_event;
    struct IOSana2Req foreign_failure, flush_request;
    struct TagItem tags[8];
    struct p4wlan_request *connect_waiter, *foreign_waiter;
    struct p4wlan_association *attempt;
    UBYTE ssid[] = "faulted-typed-attempt";
    const ULONG cookie = 0xD1550011u;
    ULONG token;
    unsigned prior_replies, prior_associate_calls;

    init_fixture(&unit, &owner, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &owner, &caps));
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, cookie);
    connect_waiter = queue_event_waiter(&unit, &owner, &connect_event,
                                        S2EVENT_CONNECT);
    (void)queue_event_waiter(&unit, &owner, &failure_event,
                             S2EVENT_CONNECT_FAILED);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_failure,
                                        S2EVENT_CONNECT_FAILED);
    attempt = queue_profile(&unit, &owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->acknowledged);
    CHECK(attempt->opener == &owner && attempt->cookie == cookie);
    token = attempt->token;
    callback_fault_during_associate = TRUE;
    prior_replies = reply_calls;
    prior_associate_calls = associate_calls;

    CHECK(p4wlan_associate_one(&unit, &radio));

    CHECK(!callback_fault_during_associate && unit.callback_fault);
    CHECK(associate_calls == prior_associate_calls + 1u);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(unit.selection_token == token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac);
    CHECK(!radio.associated);
    CHECK(owner.active_cookie == 0u && owner.failed_cookie == 0u);
    CHECK(!owner.fullmac_events);
    CHECK(failure_event.ios2_Req.io_Error == 0);
    CHECK(failure_event.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(failure_event.ios2_DataLength == cookie);
    CHECK(connect_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(connect_event.ios2_WireError == 0u);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_failure.ios2_WireError == 0u);
    CHECK(unit.pending_events == 0u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &owner);
    dispatch_case(&unit, &owner, &flush_request);
    CHECK(owner.refs == 1u && foreign.refs == 1u);
#endif
}

static void test_foreign_disassociate_preserves_typed_committed_owner(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct IOSana2Req leave_request, owner_event, foreign_event, flush_request;
    struct TagItem leave_tags[2];
    struct p4wlan_request *foreign_waiter;
    const ULONG cookie = 0xD1550002u;
    ULONG token;
    unsigned prior_replies;

    init_fixture(&unit, &owner, &radio);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &owner, &caps));
    seed_typed_committed_link(&unit, &owner, &radio, cookie);
    (void)queue_event_waiter(&unit, &owner, &owner_event,
                             S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    token = unit.selection_token;
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &foreign);
    leave_request.ios2_Data = leave_tags;
    prior_replies = reply_calls;
    dispatch_case(&unit, &foreign, &leave_request);

    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(unit.selection_token == token);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_opener == &owner && unit.link_cookie == cookie);
    CHECK(owner.fullmac_events);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(radio.associated);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(owner_event.ios2_WireError == 0u && foreign_event.ios2_WireError == 0u);
    CHECK(owner.refs == 3u); /* base, committed link and owner waiter */

    init_request(&flush_request, CMD_FLUSH, &owner);
    dispatch_case(&unit, &owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(owner.refs == 1u && foreign.refs == 1u);
#endif
}

static void test_old_typed_teardown_owner_leave_preserves_new_typed_pending(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, new_owner;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, new_connect, leave_request;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[8], leave_tags[2];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter;
    UBYTE ssid[] = "replacement-typed-owner";
    const ULONG old_cookie = 0xD1550003u;
    const ULONG next_cookie = 0xD1550004u;
    ULONG token;
    unsigned prior_replies;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &new_owner);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &new_owner, &new_connect, S2EVENT_CONNECT);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, next_cookie);
    attempt = queue_profile(&unit, &new_owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &new_owner);
    CHECK(attempt->target.fullmac_profile == P4_WIFI_FULLMAC_OPEN);
    CHECK(attempt->cookie == next_cookie && attempt->acknowledged);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(unit.teardown_opener == &old_owner);
    CHECK(unit.teardown_cookie == old_cookie);
    token = unit.selection_token;
    CHECK(token == attempt->token);
    CHECK(old_owner.fullmac_events && new_owner.fullmac_events);
    CHECK(old_owner.refs == 3u); /* base, waiter and tombstone pin */
    CHECK(new_owner.refs == 3u); /* base, connect waiter and attempt pin */

    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &old_owner);
    leave_request.ios2_Data = leave_tags;
    prior_replies = reply_calls;
    dispatch_case(&unit, &old_owner, &leave_request);

    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(unit.selection_token == token);
    CHECK(unit.pending_association == attempt);
    CHECK(attempt->opener == &new_owner && attempt->acknowledged);
    CHECK(attempt->token == token && attempt->cookie == next_cookie);
    CHECK(unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(unit.teardown_token == token && !unit.teardown_notify);
    CHECK(unit.teardown_cookie == 0u);
    CHECK(!old_owner.fullmac_events && new_owner.fullmac_events);
    CHECK(old_owner.refs == 2u); /* tombstone pin dropped, event still queued */
    CHECK(new_owner.refs == 3u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(reply_calls == prior_replies);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(unit.pending_association == attempt);
    CHECK(unit.selection_token == token);
    CHECK(new_owner.fullmac_events && new_owner.refs == 3u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);

    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(new_connect.ios2_Req.io_Error == 0);
    CHECK(new_connect.ios2_WireError == S2EVENT_CONNECT);
    CHECK(new_connect.ios2_DataLength == next_cookie);
    CHECK(unit.link_committed && unit.link_fullmac);
    CHECK(unit.link_opener == &new_owner && unit.link_cookie == next_cookie);
    CHECK(new_owner.refs == 2u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);

    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &new_owner);
    dispatch_case(&unit, &new_owner, &flush_request);
    CHECK(old_owner.refs == 1u && new_owner.refs == 1u);
#endif
}

static void test_typed_disconnect_latch_survives_mode_retirement(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    const ULONG cookie = 0x44D15C00u;

    init_fixture(&unit, &opener, &radio);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    radio.link_state = P4_WIFI_LINK_DOWN;

    CHECK(!p4wlan_maintain_link(&unit, &radio));

    CHECK(!opener.fullmac_events);
    CHECK(opener.terminal_event == S2EVENT_DISCONNECT);
    CHECK(opener.terminal_cookie == cookie);
    CHECK(unit.pending_events == 0u);
    CHECK(opener.refs == 1u);

    init_request(&request, S2_ONEVENT, &opener);
    request.ios2_DataLength = 0xBADCAFEu;
    CHECK(p4wlan_onevent_immediate_locked(&unit, &opener, &request,
                                          S2EVENT_DISCONNECT));
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(request.ios2_DataLength == cookie);
    CHECK(opener.terminal_event == 0u && opener.terminal_cookie == 0u);
}

static void test_confirmed_snapshot_mismatch_never_connects(void)
{
    ULONG field;
    for (field = 1u; field <= 4u; ++field) {
        struct p4wlan_unit unit;
        struct p4wlan_opener opener;
        struct P4WifiBase radio;
        struct IOSana2Req request;
        struct TagItem tags[10];
        UBYTE ssid[] = "target-network";
        UBYTE password[] = "secure-pass";
        UBYTE bssid[] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au };
        ULONG prior_associate_calls;

        init_fixture(&unit, &opener, &radio);
        make_tags(tags, ssid, password, bssid, 6u);
        CHECK(queue_profile(&unit, &opener, &request, tags) != NULL);
        mismatch_snapshot_field = field;
        prior_associate_calls = associate_calls;
        CHECK(p4wlan_associate_one(&unit, &radio));
        CHECK(associate_calls == prior_associate_calls + 1u);
        CHECK(!unit.link_committed && !radio.associated);
        CHECK(unit.link_opener == NULL && unit.pending_association == NULL);
        CHECK(bytes_are_zero((const UBYTE *)&unit.link_info,
                             sizeof(unit.link_info)));
        CHECK(unit.pending_events == 0u); /* no false CONNECT or DISCONNECT */
        CHECK(opener.refs == 1u);
    }
    mismatch_snapshot_field = 0u;
}

static void test_active_attempt_replacement_and_flush_cancellation(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req first_request, replacement, flush_request;
    struct TagItem first_tags[10], replacement_list[10];
    UBYTE first_ssid[] = "active-target";
    UBYTE replacement_ssid[] = "replacement";
    struct p4wlan_association *replacement_attempt;
    ULONG token_before;

    init_fixture(&unit, &opener, &radio);
    make_tags(first_tags, first_ssid, NULL, NULL, 0u);
    CHECK(queue_profile(&unit, &opener, &first_request, first_tags) != NULL);
    make_tags(replacement_list, replacement_ssid, NULL, NULL, 0u);
    init_request(&replacement, S2_SETOPTIONS, &opener);
    replacement.ios2_Data = replacement_list;
    replacement_request = &replacement;
    replacement_tags = replacement_list;
    replace_during_associate = TRUE;
    token_before = unit.selection_token;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(associate_cancel_checks == 1u);
    CHECK(unit.selection_token == token_before + 1u);
    CHECK(unit.pending_association != NULL);
    replacement_attempt = unit.pending_association;
    CHECK(replacement_attempt->target.ssid_length ==
          strlen("replacement"));
    CHECK(memcmp(replacement_attempt->target.ssid, replacement_ssid,
                 strlen("replacement")) == 0);
    CHECK(unit.active_association == NULL && !unit.link_committed);
    CHECK(unit.pending_events == 0u);
    CHECK(opener.refs == 2u);

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(flush_request.ios2_Req.io_Error == 0);
    CHECK(unit.pending_association == NULL);
    CHECK(unit.selection_token == token_before + 2u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(opener.refs == 1u);
    CHECK(unit.pending_events == 0u);
}

static void test_flush_releases_committed_pin_without_disconnect(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request, flush_request;
    struct TagItem tags[10];
    UBYTE ssid[] = "flush-link";
    ULONG previous_token;

    init_fixture(&unit, &opener, &radio);
    make_tags(tags, ssid, NULL, NULL, 0u);
    CHECK(queue_profile(&unit, &opener, &request, tags) != NULL);
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(unit.link_committed && opener.refs == 2u);
    previous_token = unit.selection_token;
    unit.pending_events |= S2EVENT_CONNECT;

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(unit.selection_token == previous_token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(bytes_are_zero((const UBYTE *)&unit.link_info,
                         sizeof(unit.link_info)));
    CHECK(opener.refs == 1u);
    CHECK(unit.pending_events == 0u); /* flush cancels stale CONNECT only */
    CHECK(radio.associated); /* Flush invalidates native gates, not peer facts. */
}

static void test_success_and_failure_wipe_secrets(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct TagItem tags[10];
    UBYTE ssid[] = "secret-target";
    UBYTE password[] = "top-secret";
    unsigned prior_wipe_checks;

    init_fixture(&unit, &opener, &radio);
    make_tags(tags, ssid, password, NULL, 0u);
    CHECK(queue_profile(&unit, &opener, &request, tags) != NULL);
    prior_wipe_checks = wipe_checks;
    associate_returns_success = FALSE;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(wipe_checks == prior_wipe_checks + 1u);
    CHECK(!unit.link_committed && !radio.associated);
    CHECK(unit.pending_events == 0u);
    CHECK(bytes_are_zero((const UBYTE *)&radio.confirmed_ap,
                         sizeof(radio.confirmed_ap)));
    CHECK(opener.refs == 1u);
}

static void test_offline_cancels_pending_attempt(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request, offline_request;
    struct TagItem tags[10];
    UBYTE ssid[] = "pending-offline";
    UBYTE password[] = "wipe-this-key";
    struct p4wlan_association *attempt;
    ULONG token;
    unsigned prior_wipes;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    make_tags(tags, ssid, password, NULL, 0u);
    attempt = queue_profile(&unit, &opener, &request, tags);
    CHECK(attempt != NULL && opener.refs == 2u);
    token = unit.selection_token;
    prior_wipes = wipe_checks;
    prior_replies = reply_calls;

    init_request(&offline_request, S2_OFFLINE, &opener);
    dispatch_case(&unit, &opener, &offline_request);
    CHECK(offline_request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(!unit.online);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(unit.selection_token == token + 1u);
    CHECK(opener.refs == 1u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK((unit.pending_events &
           (S2EVENT_CONNECT | S2EVENT_DISCONNECT)) == 0u);
    CHECK(allocation_calls == free_calls);
    CHECK(live_allocations() == 0u);
}

static void test_offline_cancels_active_attempt_before_commit(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct TagItem tags[10];
    UBYTE ssid[] = "active-offline";
    UBYTE password[] = "late-secret";
    unsigned prior_calls;
    unsigned prior_wipes;
    unsigned prior_replies;
    ULONG token;

    init_fixture(&unit, &opener, &radio);
    make_tags(tags, ssid, password, NULL, 0u);
    CHECK(queue_profile(&unit, &opener, &request, tags) != NULL);
    token = unit.selection_token;
    prior_calls = associate_calls;
    prior_wipes = wipe_checks;
    prior_replies = reply_calls;
    offline_during_associate = TRUE;

    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(!offline_during_associate);
    CHECK(offline_cancel_checks == 1u);
    CHECK(associate_calls == prior_calls + 1u);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(!unit.online);
    CHECK(unit.selection_token == token + 1u);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(!radio.associated);
    CHECK(unit.teardown_pending);
    CHECK(bytes_are_zero((const UBYTE *)&radio.confirmed_ap,
                         sizeof(radio.confirmed_ap)));
    CHECK((unit.pending_events &
           (S2EVENT_CONNECT | S2EVENT_DISCONNECT)) == 0u);
    CHECK(opener.refs == 1u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(allocation_calls == free_calls);
    CHECK(live_allocations() == 0u);
}

static void test_confirmed_down_revokes_committed_link(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    ULONG token;

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    radio.link_state = P4_WIFI_LINK_DOWN;
    unit.pending_events = S2EVENT_CONNECT;
    token = unit.selection_token;

    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(bytes_are_zero((const UBYTE *)&unit.link_info,
                         sizeof(unit.link_info)));
    CHECK(opener.refs == 1u);
    CHECK(!radio.associated);
    CHECK(!unit.teardown_pending);
    CHECK(unit.pending_events == S2EVENT_DISCONNECT);
}

static void test_typed_confirmed_down_is_cookie_local(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req origin_event, foreign_event, flush_request;
    struct p4wlan_request *foreign_waiter;
    const ULONG cookie = 0xA13C0042u;
    ULONG token;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &opener, &origin_event,
                             S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    radio.link_state = P4_WIFI_LINK_DOWN;
    unit.pending_events = S2EVENT_CONNECT;
    token = unit.selection_token;
    prior_replies = reply_calls;

    CHECK(!p4wlan_maintain_link(&unit, &radio));

    CHECK(reply_calls == prior_replies + 1u);
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac);
    CHECK(!unit.teardown_pending);
    CHECK(unit.pending_events == 0u); /* typed loss never broadcasts */
    CHECK(opener.refs == 1u && foreign.refs == 2u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(foreign.refs == 1u && foreign.event_count == 0u);
}

static void test_link_observations_preserve_only_exact_accepted_ap(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct p4_wifi_link_info accepted;
    ULONG token;

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    accepted = unit.link_info;
    token = unit.selection_token;
    radio.link_state = P4_WIFI_LINK_UNKNOWN;
    radio.associated = FALSE;

    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(unit.selection_token == token);
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(memcmp(&unit.link_info, &accepted, sizeof(accepted)) == 0);
    CHECK(opener.refs == 2u && !radio.associated);
    CHECK(unit.pending_events == 0u);

    radio.link_state = P4_WIFI_LINK_UP;
    radio.observed_ap = accepted;
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(unit.selection_token == token);
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(radio.associated);
    CHECK(opener.refs == 2u);
    CHECK(unit.pending_events == 0u); /* restoring same AP is not CONNECT */

    radio.observed_ap.ssid[0] ^= 0x20u;
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(opener.refs == 1u && !radio.associated);
    CHECK(unit.teardown_pending);
    CHECK(unit.pending_events == S2EVENT_DISCONNECT);
}

static void test_disassociate_maintains_confirmed_teardown(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct TagItem leave_tags[2];

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    unit.pending_events = S2EVENT_CONNECT;
    make_disassociate_tags(leave_tags);
    init_request(&request, S2_SETOPTIONS, &opener);
    request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &request);

    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(opener.refs == 1u);
    CHECK((unit.pending_events & S2EVENT_CONNECT) == 0u);
    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 1u && radio.disconnect == fake_disconnect);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(!unit.callback_fault && !radio.associated);
    CHECK(unit.pending_events == S2EVENT_DISCONNECT);
}

static void test_typed_teardown_retains_cookie_pin_until_success(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req request, origin_event, foreign_event, flush_request;
    struct TagItem leave_tags[2];
    struct p4wlan_request *foreign_waiter;
    const ULONG cookie = 0x87654321u;
    ULONG token;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &opener, &origin_event,
                             S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    unit.pending_events = S2EVENT_CONNECT;
    token = unit.selection_token;
    make_disassociate_tags(leave_tags);
    init_request(&request, S2_SETOPTIONS, &opener);
    request.ios2_Data = leave_tags;

    dispatch_case(&unit, &opener, &request);

    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(unit.teardown_opener == &opener);
    CHECK(unit.teardown_cookie == cookie);
    CHECK(opener.refs == 3u); /* base, queued event, deferred teardown pin */
    CHECK((unit.pending_events & S2EVENT_CONNECT) == 0u);
    CHECK(origin_event.ios2_WireError == 0u);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);

    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(disconnect_calls == 1u);
    CHECK(reply_calls == prior_replies + 1u); /* origin only */
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(opener.refs == 1u && foreign.refs == 2u);
    CHECK(unit.pending_events == 0u);

    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(foreign.refs == 1u && foreign.event_count == 0u);
}

static void test_typed_teardown_failure_drops_pin_without_disconnect(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req request, origin_event, foreign_event, flush_request;
    struct TagItem leave_tags[2];
    struct p4wlan_request *origin_waiter, *foreign_waiter;
    const ULONG cookie = 0xF00D002Au;
    unsigned prior_replies;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    origin_waiter = queue_event_waiter(&unit, &opener, &origin_event,
                                       S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    make_disassociate_tags(leave_tags);
    init_request(&request, S2_SETOPTIONS, &opener);
    request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &request);
    CHECK(unit.teardown_opener == &opener && unit.teardown_cookie == cookie);
    disconnect_returns_success = FALSE;
    prior_replies = reply_calls;

    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(reply_calls == prior_replies);
    CHECK(unit.callback_fault);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(!unit.link_committed && !unit.link_opener);
    CHECK(opener.refs == 2u); /* base plus still-parked event request */
    CHECK(origin_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(origin_event.ios2_WireError == 0u);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(opener.refs == 1u && foreign.refs == 1u);
}

static void test_unacknowledged_typed_replacement_survives_failed_teardown(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, current_connect, current_failure;
    struct IOSana2Req foreign_failure, associate_request, flush_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter, *connect_waiter, *foreign_waiter;
    UBYTE ssid[] = "unacknowledged-teardown-failure";
    const ULONG old_cookie = 0xFA110010u;
    const ULONG current_cookie = 0xFA110011u;
    ULONG token;
    unsigned prior_replies, prior_associate_calls, prior_wipes;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    connect_waiter = queue_event_waiter(&unit, &current_owner,
                                        &current_connect, S2EVENT_CONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &current_failure,
                             S2EVENT_CONNECT_FAILED);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_failure,
                                        S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);

    disconnect_returns_success = FALSE;
    failure_event_before_ack = &current_failure;
    fail_teardown_during_association_reply = TRUE;
    prior_replies = reply_calls;
    prior_associate_calls = associate_calls;
    prior_wipes = wipe_checks;
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);

    CHECK(!fail_teardown_during_association_reply);
    CHECK(attempt != NULL && attempt->opener == &current_owner);
    CHECK(attempt->cookie == current_cookie && attempt->acknowledged);
    CHECK(unit.pending_association == attempt);
    CHECK(unit.selection_token == attempt->token);
    token = attempt->token;
    CHECK(disconnect_calls == 1u);
    CHECK(unit.callback_fault && !unit.teardown_pending);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(current_owner.active_cookie == current_cookie);
    CHECK(current_owner.failed_cookie == 0u);
    CHECK(current_failure.ios2_WireError == 0u);
    CHECK(associate_calls == prior_associate_calls);
    CHECK(reply_calls == prior_replies + 1u); /* accepted SETOPTIONS ACK only */
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);

    /* The post-Reply service pass publishes one exact failure without making
     * a radio call; it neither fabricates CONNECT/DISCONNECT nor broadcasts. */
    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(associate_calls == prior_associate_calls);
    CHECK(reply_calls == prior_replies + 1u);
    CHECK(current_failure.ios2_Req.io_Error == 0);
    CHECK(current_failure.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(current_failure.ios2_DataLength == current_cookie);
    CHECK(current_connect.ios2_WireError == 0u);
    CHECK(connect_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_failure.ios2_WireError == 0u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK(unit.selection_token == token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac && !radio.associated);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(!current_owner.fullmac_events);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(old_owner.refs == 2u); /* base plus parked old DISCONNECT */
    CHECK(current_owner.refs == 2u); /* base plus parked CONNECT */
    CHECK(foreign.refs == 2u); /* base plus parked foreign failure waiter */

    failure_event_before_ack = NULL;
    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &current_owner);
    dispatch_case(&unit, &current_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u &&
          foreign.refs == 1u);
#endif
}

static void test_faulted_disassociate_cancels_unacknowledged_replacement(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, current_connect, current_failure;
    struct IOSana2Req leave_request, associate_request, flush_request;
    struct TagItem tags[8], leave_tags[2];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter, *connect_waiter, *failure_waiter;
    UBYTE ssid[] = "faulted-leave-before-ack";
    const ULONG old_cookie = 0xFA110030u;
    const ULONG current_cookie = 0xFA110031u;
    ULONG original_token;
    unsigned prior_associate_calls, prior_wipes;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    connect_waiter = queue_event_waiter(&unit, &current_owner,
                                        &current_connect, S2EVENT_CONNECT);
    failure_waiter = queue_event_waiter(&unit, &current_owner,
                                        &current_failure,
                                        S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &current_owner);

    original_token = unit.selection_token;
    disconnect_returns_success = FALSE;
    failure_event_before_ack = &current_failure;
    failed_teardown_leave_request = &leave_request;
    failed_teardown_leave_tags = leave_tags;
    fail_teardown_during_association_reply = TRUE;
    disassociate_during_failed_teardown_reply = TRUE;
    prior_associate_calls = associate_calls;
    prior_wipes = wipe_checks;
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);

    CHECK(!fail_teardown_during_association_reply);
    CHECK(!disassociate_during_failed_teardown_reply);
    CHECK(attempt == NULL); /* leave revoked it before the original ACK */
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(disconnect_calls == 1u);
    CHECK(associate_calls == prior_associate_calls);
    CHECK(unit.callback_fault && unit.online);
    CHECK(unit.selection_token > token_after_failed_teardown_reply);
    CHECK(unit.selection_token > original_token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(!current_owner.fullmac_events);
    CHECK(current_failure.ios2_WireError == 0u);
    CHECK(current_connect.ios2_WireError == 0u);
    CHECK(failure_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(connect_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(old_owner.refs == 2u); /* base plus parked old DISCONNECT */
    CHECK(current_owner.refs == 3u); /* base plus two parked event requests */
    CHECK(!p4wlan_associate_one(&unit, &radio));
    CHECK(associate_calls == prior_associate_calls);

    failure_event_before_ack = NULL;
    failed_teardown_leave_request = NULL;
    failed_teardown_leave_tags = NULL;
    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &current_owner);
    dispatch_case(&unit, &current_owner, &flush_request);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u);
#endif
}

static void test_faulted_committed_leave_does_not_schedule_teardown(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req leave_request, disconnect_event, flush_request;
    struct TagItem leave_tags[2];
    struct p4wlan_request *disconnect_waiter;
    const ULONG cookie = 0xFA110035u;
    ULONG token;
    unsigned prior_associate_calls, prior_disconnect_calls;

    init_fixture(&unit, &opener, &radio);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    disconnect_waiter = queue_event_waiter(&unit, &opener, &disconnect_event,
                                           S2EVENT_DISCONNECT);
    unit.callback_fault = TRUE;
    token = unit.selection_token;
    prior_associate_calls = associate_calls;
    prior_disconnect_calls = disconnect_calls;
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &opener);
    leave_request.ios2_Data = leave_tags;

    dispatch_case(&unit, &opener, &leave_request);

    CHECK(leave_request.ios2_Req.io_Error == 0);
    CHECK(unit.callback_fault && unit.online);
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.link_cookie == 0u && !unit.link_fullmac);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(opener.active_cookie == 0u && opener.failed_cookie == 0u);
    CHECK(!opener.fullmac_events);
    CHECK(associate_calls == prior_associate_calls);
    CHECK(disconnect_calls == prior_disconnect_calls);
    CHECK(disconnect_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(disconnect_event.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(opener.refs == 2u); /* base plus parked event */

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(opener.refs == 1u);
}

static void test_close_phase_model_cancels_unacknowledged_replacement(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, current_connect, current_failure;
    struct IOSana2Req associate_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter;
    UBYTE ssid[] = "close-phase-unacknowledged";
    const ULONG old_cookie = 0xFA110040u;
    const ULONG current_cookie = 0xFA110041u;
    ULONG original_token;
    unsigned prior_associate_calls, prior_wipes;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &current_connect,
                             S2EVENT_CONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &current_failure,
                             S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);

    original_token = unit.selection_token;
    disconnect_returns_success = FALSE;
    failure_event_before_ack = &current_failure;
    fail_teardown_during_association_reply = TRUE;
    close_model_during_failed_teardown_reply = TRUE;
    prior_associate_calls = associate_calls;
    prior_wipes = wipe_checks;
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);

    CHECK(!fail_teardown_during_association_reply);
    CHECK(!close_model_during_failed_teardown_reply);
    CHECK(attempt == NULL); /* Close-phase model revoked it before ACK */
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(disconnect_calls == 1u);
    CHECK(associate_calls == prior_associate_calls);
    CHECK(unit.online && unit.callback_fault);
    CHECK(unit.selection_token > token_after_failed_teardown_reply);
    CHECK(unit.selection_token > original_token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(current_owner.closing && current_owner.refs == 1u);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(current_owner.event_count == 0u && current_owner.request_count == 0u);
    CHECK(current_failure.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(current_failure.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(current_failure.ios2_WireError != S2EVENT_CONNECT_FAILED);
    CHECK(current_connect.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(current_connect.ios2_WireError != S2EVENT_CONNECT);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(wipe_checks == prior_wipes + 1u);

    /* This models only Close's local closing/flush/cancel phase through the
     * extracted helper; it does not execute the native Close wait/drain loop. */
    failure_event_before_ack = NULL;
    init_request(&associate_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &associate_request);
    CHECK(old_owner.refs == 1u);
#endif
}

static void test_offline_cancels_unacknowledged_failed_replacement(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, current_connect, current_failure;
    struct IOSana2Req associate_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    UBYTE ssid[] = "offline-unacknowledged-failure";
    const ULONG old_cookie = 0xFA110020u;
    const ULONG current_cookie = 0xFA110021u;
    ULONG original_token;
    unsigned prior_associate_calls, prior_wipes;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    (void)queue_event_waiter(&unit, &old_owner, &old_disconnect,
                             S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &current_connect,
                             S2EVENT_CONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &current_failure,
                             S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);

    original_token = unit.selection_token;
    disconnect_returns_success = FALSE;
    failure_event_before_ack = &current_failure;
    fail_teardown_during_association_reply = TRUE;
    offline_during_failed_teardown_reply = TRUE;
    prior_associate_calls = associate_calls;
    prior_wipes = wipe_checks;
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);

    CHECK(!fail_teardown_during_association_reply);
    CHECK(!offline_during_failed_teardown_reply);
    CHECK(attempt == NULL); /* accepted Reply was revoked before its ACK */
    CHECK(associate_request.ios2_Req.io_Error == 0);
    CHECK(disconnect_calls == 1u);
    CHECK(associate_calls == prior_associate_calls);
    CHECK(!unit.online && unit.callback_fault);
    CHECK(unit.selection_token > token_after_failed_teardown_reply);
    CHECK(unit.selection_token > original_token);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(!current_owner.fullmac_events);
    CHECK(current_failure.ios2_Req.io_Error == S2ERR_OUTOFSERVICE);
    CHECK(current_failure.ios2_WireError == S2WERR_UNIT_OFFLINE);
    CHECK(current_failure.ios2_WireError != S2EVENT_CONNECT_FAILED);
    CHECK(current_connect.ios2_WireError != S2EVENT_CONNECT);
    CHECK(old_disconnect.ios2_WireError != S2EVENT_DISCONNECT);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u);

    failure_event_before_ack = NULL;
#endif
}

static void test_failed_old_teardown_reports_only_current_typed_cookie(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, own_failure, foreign_failure;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter, *foreign_waiter;
    UBYTE ssid[] = "failed-disconnect-owned-cookie";
    const ULONG old_cookie = 0xFA110001u;
    const ULONG current_cookie = 0xFA110002u;
    unsigned prior_replies, prior_wipes;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &current_owner, &own_failure,
                             S2EVENT_CONNECT_FAILED);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_failure,
                                        S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &current_owner);
    CHECK(attempt->cookie == current_cookie && attempt->acknowledged);
    CHECK(unit.teardown_opener == &old_owner);
    CHECK(unit.teardown_cookie == old_cookie && unit.teardown_notify);
    CHECK(current_owner.active_cookie == current_cookie);

    disconnect_returns_success = FALSE;
    prior_replies = reply_calls;
    prior_wipes = wipe_checks;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(disconnect_calls == 1u);
    CHECK(reply_calls == prior_replies + 1u); /* current cookie failure only */
    CHECK(own_failure.ios2_Req.io_Error == 0);
    CHECK(own_failure.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(own_failure.ios2_DataLength == current_cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_failure.ios2_WireError == 0u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(!unit.teardown_pending && unit.teardown_opener == NULL);
    CHECK(unit.callback_fault);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(!current_owner.fullmac_events);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(old_owner.refs == 2u); /* base plus still-parked old disconnect */
    CHECK(current_owner.refs == 1u);
    CHECK(foreign.refs == 2u);

    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u &&
          foreign.refs == 1u);
#endif
}

static void test_failed_old_teardown_latches_cookie_for_late_waiter(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner, foreign;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, foreign_failure, failure_request;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[8];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter, *foreign_waiter;
    UBYTE ssid[] = "failed-disconnect-late-cookie";
    const ULONG old_cookie = 0xFA110003u;
    const ULONG current_cookie = 0xFA110004u;
    unsigned prior_replies;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    add_opener(&unit, &foreign);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_failure,
                                        S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &current_owner);
    CHECK(attempt->cookie == current_cookie && attempt->acknowledged);

    disconnect_returns_success = FALSE;
    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(reply_calls == prior_replies);
    CHECK(current_owner.failed_cookie == current_cookie);
    CHECK(current_owner.active_cookie == 0u);
    CHECK(!unit.pending_association && !unit.active_association);
    CHECK(unit.callback_fault && !unit.teardown_pending);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_failure.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);

    init_request(&failure_request, S2_ONEVENT, &current_owner);
    CHECK(p4wlan_onevent_immediate_locked(&unit, &current_owner,
        &failure_request, S2EVENT_CONNECT_FAILED));
    CHECK(failure_request.ios2_Req.io_Error == 0);
    CHECK(failure_request.ios2_WireError == S2EVENT_CONNECT_FAILED);
    CHECK(failure_request.ios2_DataLength == current_cookie);
    CHECK(current_owner.failed_cookie == 0u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);

    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u &&
          foreign.refs == 1u);
#endif
}

static void test_cancelled_replacement_gets_no_failed_teardown_cookie(void)
{
#if P4_WIFI_FULLMAC_ADMISSION_ENABLED
    struct p4wlan_unit unit;
    struct p4wlan_opener old_owner, current_owner;
    struct P4WifiBase radio;
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct IOSana2Req old_disconnect, own_failure, cancel_request;
    struct IOSana2Req associate_request, flush_request;
    struct TagItem tags[8], leave_tags[2];
    struct p4wlan_association *attempt;
    struct p4wlan_request *old_waiter, *own_waiter;
    UBYTE ssid[] = "cancel-before-failed-teardown";
    const ULONG old_cookie = 0xFA110005u;
    const ULONG current_cookie = 0xFA110006u;
    unsigned prior_replies;

    init_fixture(&unit, &old_owner, &radio);
    add_opener(&unit, &current_owner);
    CHECK(query_fullmac_capabilities(&unit, &old_owner, &caps));
    seed_typed_committed_link(&unit, &old_owner, &radio, old_cookie);
    old_waiter = queue_event_waiter(&unit, &old_owner, &old_disconnect,
                                    S2EVENT_DISCONNECT);
    own_waiter = queue_event_waiter(&unit, &current_owner, &own_failure,
                                    S2EVENT_CONNECT_FAILED);
    init_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
        (caps.Flags & S2FULLMAC_CAPF_DIAGNOSTIC_RISK) ?
            S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK : 0u);
    make_fullmac_tags(tags, ssid, &selection, NULL, current_cookie);
    attempt = queue_profile(&unit, &current_owner, &associate_request, tags);
    CHECK(attempt != NULL && attempt->opener == &current_owner);
    CHECK(attempt->cookie == current_cookie && attempt->acknowledged);
    CHECK(unit.teardown_opener == &old_owner);
    CHECK(unit.pending_association == attempt);

    make_disassociate_tags(leave_tags);
    init_request(&cancel_request, S2_SETOPTIONS, &current_owner);
    replacement_request = &cancel_request;
    replacement_tags = leave_tags;
    active_opener = &current_owner;
    replace_during_disconnect = TRUE;
    disconnect_returns_success = FALSE;
    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(!replace_during_disconnect);
    CHECK(cancel_request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == prior_replies + 1u); /* only the cancel ACK */
    CHECK(disconnect_calls == 1u);
    CHECK(unit.callback_fault && !unit.teardown_pending);
    CHECK(unit.pending_association == NULL && unit.active_association == NULL);
    CHECK(current_owner.active_cookie == 0u && current_owner.failed_cookie == 0u);
    CHECK(own_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(own_failure.ios2_WireError == 0u);
    CHECK(old_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(old_disconnect.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
    CHECK(unit.link_opener == NULL && !unit.link_committed);

    init_request(&flush_request, CMD_FLUSH, &old_owner);
    dispatch_case(&unit, &old_owner, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &current_owner);
    dispatch_case(&unit, &current_owner, &flush_request);
    CHECK(old_owner.refs == 1u && current_owner.refs == 1u);
#endif
}

static void test_teardown_failure_never_publishes_disconnect(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct TagItem leave_tags[2];

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    unit.pending_events = S2EVENT_CONNECT;
    make_disassociate_tags(leave_tags);
    init_request(&request, S2_SETOPTIONS, &opener);
    request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &request);
    disconnect_returns_success = FALSE;

    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 1u);
    CHECK(unit.callback_fault);
    CHECK(!unit.teardown_pending && !unit.link_committed);
    CHECK(!radio.associated);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);
}

static void test_new_selection_during_teardown_is_preserved(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req leave_request, replacement_request_local, flush_request;
    struct TagItem leave_tags[2], replacement_tags_local[10];
    UBYTE replacement_ssid[] = "new-selection";
    struct p4wlan_association *attempt;
    ULONG token;
    unsigned prior_wipes;

    init_fixture(&unit, &opener, &radio);
    seed_committed_link(&unit, &opener, &radio);
    unit.pending_events = S2EVENT_CONNECT;
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &opener);
    leave_request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &leave_request);
    token = unit.selection_token;

    make_tags(replacement_tags_local, replacement_ssid, NULL, NULL, 0u);
    init_request(&replacement_request_local, S2_SETOPTIONS, &opener);
    replacement_request = &replacement_request_local;
    replacement_tags = replacement_tags_local;
    replace_during_disconnect = TRUE;
    prior_wipes = wipe_checks;

    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(!replace_during_disconnect);
    CHECK(disconnect_calls == 1u);
    CHECK(unit.selection_token == token + 1u);
    CHECK(unit.teardown_pending && !unit.teardown_notify);
    CHECK(!unit.link_committed && unit.link_opener == NULL);
    CHECK(unit.pending_events == 0u); /* no stale CONNECT/DISCONNECT */
    attempt = unit.pending_association;
    CHECK(attempt != NULL && attempt->target.ssid_length ==
          strlen("new-selection"));
    CHECK(memcmp(attempt->target.ssid, replacement_ssid,
                 strlen("new-selection")) == 0);
    CHECK(opener.refs == 2u);
    CHECK(wipe_checks == prior_wipes);

    token = unit.selection_token;
    CHECK(p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 2u);
    CHECK(unit.selection_token == token);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.pending_association == attempt);
    CHECK(unit.pending_events == 0u);

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(unit.pending_association == NULL && opener.refs == 1u);
    CHECK(wipe_checks == prior_wipes + 1u);
    CHECK(allocation_calls == free_calls);
    CHECK(live_allocations() == 0u);
}

static void test_cancel_during_disconnect_suppresses_typed_result(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req leave_request, repeat_request, cancel_request;
    struct IOSana2Req origin_event;
    struct IOSana2Req foreign_event;
    struct TagItem leave_tags[2];
    struct p4wlan_request *foreign_waiter;
    const ULONG cookie = 0xCACE0021u;
    unsigned prior_replies;
    ULONG token, teardown_token;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &opener, &origin_event,
                             S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &opener);
    leave_request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &leave_request);
    token = unit.selection_token;
    CHECK(unit.teardown_opener == &opener && unit.teardown_cookie == cookie);
    teardown_token = unit.teardown_token;
    CHECK(teardown_token != 0u);

    /* A repeated disassociate invalidates selections but must not replace or
     * duplicate the in-flight physical teardown identity and pin. */
    init_request(&repeat_request, S2_SETOPTIONS, &opener);
    repeat_request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &repeat_request);
    CHECK(unit.selection_token == token + 1u);
    CHECK(unit.teardown_token == teardown_token);
    CHECK(unit.teardown_opener == &opener && unit.teardown_cookie == cookie);
    CHECK(unit.teardown_pending && unit.teardown_notify);
    CHECK(opener.refs == 3u); /* base, origin waiter, teardown pin */
    token = unit.selection_token;

    /* Flush from inside disconnect revokes only the tombstone identity. The
     * physical teardown token remains valid so this successful RPC settles. */
    init_request(&cancel_request, CMD_FLUSH, &opener);
    replacement_request = &cancel_request;
    replacement_tags = NULL;
    replace_during_disconnect = TRUE;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(!replace_during_disconnect);
    CHECK(disconnect_calls == 1u);
    CHECK(unit.selection_token == token);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(unit.teardown_token == 0u);
    CHECK(opener.refs == 1u);
    CHECK(origin_event.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(origin_event.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(origin_event.ios2_WireError != S2EVENT_DISCONNECT);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(foreign_event.ios2_WireError == 0u);
    CHECK(unit.pending_events == 0u);

    /* Even if a later radio operation would fail, there must be no retry:
     * confirmed success already settled the physical teardown. */
    prior_replies = reply_calls;
    disconnect_returns_success = FALSE;
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 1u);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(reply_calls == prior_replies);
    CHECK(origin_event.ios2_WireError != S2EVENT_DISCONNECT);
    CHECK(unit.pending_events == 0u);

    init_request(&cancel_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &cancel_request);
    CHECK(foreign.refs == 1u && foreign.event_count == 0u);
}

static void test_typed_teardown_supersession_resets_legacy_mode(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener, foreign;
    struct P4WifiBase radio;
    struct IOSana2Req leave_request, replacement_io, origin_event;
    struct IOSana2Req foreign_event, flush_request;
    struct TagItem leave_tags[2], replacement_tag_list[10];
    UBYTE replacement_ssid[] = "legacy-after-typed";
    struct p4wlan_request *foreign_waiter;
    struct p4wlan_association *attempt;
    const ULONG cookie = 0xB16B00B5u;
    ULONG token;

    init_fixture(&unit, &opener, &radio);
    add_opener(&unit, &foreign);
    seed_typed_committed_link(&unit, &opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &opener, &origin_event,
                             S2EVENT_DISCONNECT);
    foreign_waiter = queue_event_waiter(&unit, &foreign, &foreign_event,
                                        S2EVENT_DISCONNECT);
    make_disassociate_tags(leave_tags);
    init_request(&leave_request, S2_SETOPTIONS, &opener);
    leave_request.ios2_Data = leave_tags;
    dispatch_case(&unit, &opener, &leave_request);
    token = unit.selection_token;

    make_tags(replacement_tag_list, replacement_ssid, NULL, NULL, 0u);
    init_request(&replacement_io, S2_SETOPTIONS, &opener);
    replacement_request = &replacement_io;
    replacement_tags = replacement_tag_list;
    replace_during_disconnect = TRUE;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(!replace_during_disconnect);
    CHECK(unit.selection_token == token + 1u);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(!opener.fullmac_events);
    CHECK(unit.pending_association != NULL);
    attempt = unit.pending_association;
    CHECK(attempt->target.fullmac_profile == 0u);
    CHECK(attempt->target.ssid_length == strlen("legacy-after-typed"));
    CHECK(memcmp(attempt->target.ssid, replacement_ssid,
                 strlen("legacy-after-typed")) == 0);
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);
    CHECK(unit.pending_events == 0u);

    /* The preserved teardown token accepted the result despite the newer
     * selection token. The replacement is now eligible for radio service. */
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 1u);
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(unit.link_committed && unit.link_opener == &opener);
    CHECK(!unit.link_fullmac && !opener.fullmac_events);
    CHECK(unit.link_cookie == 0u);
    CHECK(foreign_waiter->state == P4WLAN_REQUEST_QUEUED);

    init_request(&flush_request, CMD_FLUSH, &opener);
    dispatch_case(&unit, &opener, &flush_request);
    CHECK(unit.pending_association == NULL && opener.refs == 1u);
    init_request(&flush_request, CMD_FLUSH, &foreign);
    dispatch_case(&unit, &foreign, &flush_request);
    CHECK(foreign.refs == 1u);
}

static void test_replacement_opener_cancel_preserves_old_teardown(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener typed_opener, replacement_opener;
    struct P4WifiBase radio;
    struct IOSana2Req origin_event, replacement_event;
    struct IOSana2Req select_request, cancel_request;
    struct TagItem selection_tags[10];
    UBYTE ssid[] = "replacement-b";
    struct p4wlan_association *attempt;
    const ULONG cookie = 0xA10B0001u;
    ULONG selection_token, teardown_token;
    unsigned prior_replies;

    init_fixture(&unit, &typed_opener, &radio);
    add_opener(&unit, &replacement_opener);
    seed_typed_committed_link(&unit, &typed_opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &typed_opener, &origin_event,
                             S2EVENT_DISCONNECT);
    (void)queue_event_waiter(&unit, &replacement_opener, &replacement_event,
                             S2EVENT_DISCONNECT);
    make_tags(selection_tags, ssid, NULL, NULL, 0u);
    attempt = queue_profile(&unit, &replacement_opener, &select_request,
                            selection_tags);
    CHECK(attempt != NULL && attempt->opener == &replacement_opener);
    CHECK(unit.teardown_opener == &typed_opener);
    CHECK(unit.teardown_cookie == cookie);
    teardown_token = unit.teardown_token;
    selection_token = unit.selection_token;
    CHECK(teardown_token != 0u && teardown_token == selection_token);

    /* B cancels its own pending selection while A's old disconnect RPC runs. */
    init_request(&cancel_request, CMD_FLUSH, &replacement_opener);
    replacement_request = &cancel_request;
    replacement_tags = NULL;
    active_opener = &replacement_opener;
    replace_during_disconnect = TRUE;
    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(!replace_during_disconnect);
    CHECK(unit.selection_token == selection_token + 1u);
    CHECK(unit.teardown_token == 0u);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(unit.pending_association == NULL);
    CHECK(unit.pending_events == 0u);
    CHECK(reply_calls == prior_replies + 3u);
    CHECK(replied[prior_replies] == &replacement_event);
    CHECK(replied[prior_replies + 1u] == &cancel_request);
    CHECK(replied[prior_replies + 2u] == &origin_event);
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(replacement_event.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(replacement_event.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(replacement_event.ios2_WireError != S2EVENT_DISCONNECT);
    CHECK(typed_opener.refs == 1u && replacement_opener.refs == 1u);

    prior_replies = reply_calls;
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    CHECK(disconnect_calls == 1u && reply_calls == prior_replies);
}

static void test_third_selection_preserves_old_teardown_and_new_attempt(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener typed_opener, opener_b, opener_c;
    struct P4WifiBase radio;
    struct IOSana2Req origin_event, event_b, event_c;
    struct IOSana2Req select_b, select_c, flush_request;
    struct TagItem tags_b[10], tags_c[10];
    UBYTE ssid_b[] = "replacement-b";
    UBYTE ssid_c[] = "replacement-c";
    struct p4wlan_request *waiter_b, *waiter_c;
    struct p4wlan_association *attempt_c;
    const ULONG cookie = 0xA10C0002u;
    ULONG token_b, teardown_token;
    unsigned prior_replies;

    init_fixture(&unit, &typed_opener, &radio);
    add_opener(&unit, &opener_b);
    add_opener(&unit, &opener_c);
    seed_typed_committed_link(&unit, &typed_opener, &radio, cookie);
    (void)queue_event_waiter(&unit, &typed_opener, &origin_event,
                             S2EVENT_DISCONNECT);
    waiter_b = queue_event_waiter(&unit, &opener_b, &event_b,
                                  S2EVENT_DISCONNECT);
    waiter_c = queue_event_waiter(&unit, &opener_c, &event_c,
                                  S2EVENT_DISCONNECT);
    make_tags(tags_b, ssid_b, NULL, NULL, 0u);
    CHECK(queue_profile(&unit, &opener_b, &select_b, tags_b) != NULL);
    CHECK(unit.teardown_opener == &typed_opener);
    CHECK(unit.teardown_cookie == cookie);
    teardown_token = unit.teardown_token;
    token_b = unit.selection_token;

    make_tags(tags_c, ssid_c, NULL, NULL, 0u);
    init_request(&select_c, S2_SETOPTIONS, &opener_c);
    replacement_request = &select_c;
    replacement_tags = tags_c;
    active_opener = &opener_c;
    replace_during_disconnect = TRUE;
    prior_replies = reply_calls;
    CHECK(p4wlan_maintain_link(&unit, &radio));

    CHECK(!replace_during_disconnect);
    CHECK(unit.selection_token == token_b + 1u);
    CHECK(unit.teardown_token == 0u);
    CHECK(!unit.teardown_pending && !unit.teardown_notify);
    CHECK(unit.teardown_opener == NULL && unit.teardown_cookie == 0u);
    CHECK(unit.pending_association != NULL);
    attempt_c = unit.pending_association;
    CHECK(attempt_c->opener == &opener_c);
    CHECK(attempt_c->token == token_b + 1u);
    CHECK(attempt_c->target.ssid_length == strlen("replacement-c"));
    CHECK(memcmp(attempt_c->target.ssid, ssid_c,
                 strlen("replacement-c")) == 0);
    CHECK(reply_calls == prior_replies + 2u); /* C ACK and A terminal */
    CHECK(replied[prior_replies] == &select_c);
    CHECK(replied[prior_replies + 1u] == &origin_event);
    CHECK(origin_event.ios2_Req.io_Error == 0);
    CHECK(origin_event.ios2_WireError == S2EVENT_DISCONNECT);
    CHECK(origin_event.ios2_DataLength == cookie);
    CHECK(!typed_opener.fullmac_events);
    CHECK(waiter_b->state == P4WLAN_REQUEST_QUEUED);
    CHECK(waiter_c->state == P4WLAN_REQUEST_QUEUED);
    CHECK(event_b.ios2_WireError == 0u && event_c.ios2_WireError == 0u);
    CHECK(typed_opener.refs == 1u);
    CHECK(opener_b.refs == 2u); /* B attempt retired, waiter retained */
    CHECK(opener_c.refs == 3u); /* C attempt plus waiter */
    CHECK(unit.pending_events == 0u);
    CHECK(teardown_token != 0u);

    /* C's accepted selection remains serviceable after A's old terminal. */
    CHECK(!p4wlan_maintain_link(&unit, &radio));
    prior_replies = reply_calls;
    CHECK(p4wlan_associate_one(&unit, &radio));
    CHECK(reply_calls == prior_replies);
    CHECK(unit.link_committed && unit.link_opener == &opener_c);
    CHECK(!unit.link_fullmac && unit.link_cookie == 0u);
    CHECK(waiter_b->state == P4WLAN_REQUEST_QUEUED);
    CHECK(waiter_c->state == P4WLAN_REQUEST_QUEUED);
    CHECK(event_b.ios2_WireError == 0u && event_c.ios2_WireError == 0u);
    CHECK((unit.pending_events & S2EVENT_DISCONNECT) == 0u);

    init_request(&flush_request, CMD_FLUSH, &opener_b);
    dispatch_case(&unit, &opener_b, &flush_request);
    init_request(&flush_request, CMD_FLUSH, &opener_c);
    dispatch_case(&unit, &opener_c, &flush_request);
    CHECK(opener_b.refs == 1u && opener_c.refs == 1u);
}

static unsigned replies_since_for_request(unsigned first,
                                           const struct IOSana2Req *request)
{
    unsigned index;
    unsigned count = 0u;
    for (index = first; index < reply_calls; ++index)
        if (replied[index] == request)
            ++count;
    return count;
}

static void set_configinterface_mac(struct IOSana2Req *request,
                                    const UBYTE mac[6], BOOL mismatch)
{
    memcpy(request->ios2_SrcAddr, mac, 6u);
    if (mismatch)
        request->ios2_SrcAddr[5] ^= 1u;
}

static void check_configinterface_refusal(BOOL hardware_ready,
    BOOL callback_fault, BOOL configured, BOOL online, BOOL mismatch_mac,
    BYTE expected_error, ULONG expected_wire_error)
{
    static const UBYTE mac[6] = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req request;
    struct p4_wifi_link_info unit_link_before, confirmed_before, observed_before;
    ULONG pending_before, selection_before, event_count_before;
    ULONG request_count_before, link_cookie_before, link_state_before;
    struct p4wlan_opener *link_opener_before;
    BOOL committed_before, fullmac_before, associated_before;
    unsigned replies_before;

    init_fixture(&unit, &opener, &radio);
    memcpy(unit.mac, mac, sizeof(mac));
    unit.hardware_ready = hardware_ready;
    unit.callback_fault = callback_fault;
    unit.configured = configured;
    unit.online = online;
    unit.pending_events = S2EVENT_DISCONNECT;
    unit.selection_token = 0x120034u;
    radio.link_state = P4_WIFI_LINK_DOWN;
    unit_link_before = unit.link_info;
    confirmed_before = radio.confirmed_ap;
    observed_before = radio.observed_ap;
    pending_before = unit.pending_events;
    selection_before = unit.selection_token;
    event_count_before = unit.event_count;
    request_count_before = unit.request_count;
    link_cookie_before = unit.link_cookie;
    link_opener_before = unit.link_opener;
    committed_before = unit.link_committed;
    fullmac_before = unit.link_fullmac;
    link_state_before = radio.link_state;
    associated_before = radio.associated;

    init_request(&request, S2_CONFIGINTERFACE, &opener);
    set_configinterface_mac(&request, mac, mismatch_mac);
    replies_before = reply_calls;
    dispatch_case(&unit, &opener, &request);

    CHECK(request.ios2_Req.io_Error == expected_error);
    CHECK(request.ios2_WireError == expected_wire_error);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(replies_since_for_request(replies_before, &request) == 1u);
    CHECK(unit.hardware_ready == hardware_ready);
    CHECK(unit.callback_fault == callback_fault);
    CHECK(unit.configured == configured && unit.online == online);
    CHECK(unit.pending_events == pending_before);
    CHECK(unit.selection_token == selection_before);
    CHECK(unit.event_count == event_count_before);
    CHECK(unit.request_count == request_count_before);
    CHECK(unit.link_cookie == link_cookie_before);
    CHECK(unit.link_opener == link_opener_before);
    CHECK(unit.link_committed == committed_before);
    CHECK(unit.link_fullmac == fullmac_before);
    CHECK(memcmp(&unit.link_info, &unit_link_before,
                 sizeof(unit.link_info)) == 0);
    CHECK(radio.link_state == link_state_before);
    CHECK(radio.associated == associated_before);
    CHECK(memcmp(&radio.confirmed_ap, &confirmed_before,
                 sizeof(radio.confirmed_ap)) == 0);
    CHECK(memcmp(&radio.observed_ap, &observed_before,
                 sizeof(radio.observed_ap)) == 0);
    CHECK(unit.lock.depth == 0u);
}

static void test_configinterface_existing_configuration_semantics(void)
{
    static const UBYTE mac[6] = {0x02u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct P4WifiBase radio;
    struct IOSana2Req online_waiter, first_config, repeat_config;
    struct p4_wifi_link_info unit_link_before, confirmed_before, observed_before;
    ULONG pending_before, selection_before, link_state_before;
    unsigned replies_before;

    init_fixture(&unit, &opener, &radio);
    memcpy(unit.mac, mac, sizeof(mac));
    unit.online = FALSE;
    unit.configured = FALSE;
    radio.link_state = P4_WIFI_LINK_DOWN;
    init_request(&online_waiter, S2_ONEVENT, &opener);
    online_waiter.ios2_WireError = S2EVENT_ONLINE;
    replies_before = reply_calls;
    dispatch_case(&unit, &opener, &online_waiter);
    CHECK(reply_calls == replies_before);
    CHECK(unit.event_count == 1u && opener.event_count == 1u);

    init_request(&first_config, S2_CONFIGINTERFACE, &opener);
    set_configinterface_mac(&first_config, mac, FALSE);
    dispatch_case(&unit, &opener, &first_config);
    CHECK(first_config.ios2_Req.io_Error == 0);
    CHECK(first_config.ios2_WireError == 0u);
    CHECK(unit.configured && unit.online);
    CHECK(radio.link_state == P4_WIFI_LINK_DOWN && !radio.associated);
    CHECK(online_waiter.ios2_Req.io_Error == 0);
    CHECK(online_waiter.ios2_WireError == S2EVENT_ONLINE);
    CHECK(reply_calls == replies_before + 2u);
    CHECK(replies_since_for_request(replies_before, &online_waiter) == 1u);
    CHECK(replies_since_for_request(replies_before, &first_config) == 1u);
    CHECK(unit.event_count == 0u && opener.event_count == 0u);
    CHECK(unit.request_count == 0u && opener.request_count == 0u);

    /* Keep an edge latch visible so a repeated configuration cannot consume
     * or replace event state while reporting IS_CONFIGURED. */
    unit.pending_events = S2EVENT_DISCONNECT;
    unit_link_before = unit.link_info;
    confirmed_before = radio.confirmed_ap;
    observed_before = radio.observed_ap;
    pending_before = unit.pending_events;
    selection_before = unit.selection_token;
    link_state_before = radio.link_state;
    init_request(&repeat_config, S2_CONFIGINTERFACE, &opener);
    set_configinterface_mac(&repeat_config, mac, FALSE);
    replies_before = reply_calls;
    dispatch_case(&unit, &opener, &repeat_config);
    CHECK(repeat_config.ios2_Req.io_Error == S2ERR_BAD_STATE);
    CHECK(repeat_config.ios2_WireError == S2WERR_IS_CONFIGURED);
    CHECK(unit.configured && unit.online);
    CHECK(unit.pending_events == pending_before);
    CHECK(unit.selection_token == selection_before);
    CHECK(unit.event_count == 0u && unit.request_count == 0u);
    CHECK(unit.link_committed == FALSE && unit.link_cookie == 0u);
    CHECK(memcmp(&unit.link_info, &unit_link_before,
                 sizeof(unit.link_info)) == 0);
    CHECK(radio.link_state == link_state_before && !radio.associated);
    CHECK(memcmp(&radio.confirmed_ap, &confirmed_before,
                 sizeof(radio.confirmed_ap)) == 0);
    CHECK(memcmp(&radio.observed_ap, &observed_before,
                 sizeof(radio.observed_ap)) == 0);
    CHECK(reply_calls == replies_before + 1u);
    CHECK(replies_since_for_request(replies_before, &repeat_config) == 1u);

    check_configinterface_refusal(TRUE, FALSE, TRUE, FALSE, FALSE,
                                  S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
    check_configinterface_refusal(TRUE, FALSE, FALSE, TRUE, FALSE,
                                  S2ERR_BAD_STATE, S2WERR_UNIT_ONLINE);
    check_configinterface_refusal(TRUE, FALSE, TRUE, TRUE, TRUE,
                                  S2ERR_BAD_ADDRESS, S2WERR_SRC_ADDRESS);
    check_configinterface_refusal(FALSE, FALSE, TRUE, TRUE, FALSE,
                                  S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    check_configinterface_refusal(TRUE, TRUE, TRUE, TRUE, FALSE,
        S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
}

static void test_suite_cleanup(void)
{
    unsigned index;
    CHECK(pool_memory == NULL);
    for (index = 0; index < allocation_slots; ++index)
        CHECK(!allocation_records[index].live);
    CHECK(allocation_calls == free_calls);
    CHECK(reply_calls < sizeof(replied) / sizeof(replied[0]));
}

int main(void)
{
    test_fullmac_capability_queries_follow_policy_gates();
    test_fullmac_admission_policy_and_owned_sae_credential();
    test_beginio_refusals_preserve_selection_and_link();
    test_cookie_copy_and_reply_ack_fencing();
    test_legacy_cookie_failure_and_leave_clear_latch();
    test_replacement_owns_and_wipes_attempt();
    test_service_commit_and_owned_network_snapshot();
    test_typed_association_connect_is_cookie_local();
    test_legacy_selection_restores_prior_opener_broadcast();
    test_same_opener_legacy_replacement_waits_for_disconnect();
    test_typed_pending_association_survives_old_teardown();
    test_foreign_disassociate_preserves_typed_pending_owner();
    test_foreign_disassociate_preserves_typed_active_rpc();
    test_typed_attempt_fault_reports_exact_failure_cookie();
    test_foreign_disassociate_preserves_typed_committed_owner();
    test_old_typed_teardown_owner_leave_preserves_new_typed_pending();
    test_typed_disconnect_latch_survives_mode_retirement();
    test_confirmed_snapshot_mismatch_never_connects();
    test_active_attempt_replacement_and_flush_cancellation();
    test_flush_releases_committed_pin_without_disconnect();
    test_success_and_failure_wipe_secrets();
    test_offline_cancels_pending_attempt();
    test_offline_cancels_active_attempt_before_commit();
    test_confirmed_down_revokes_committed_link();
    test_typed_confirmed_down_is_cookie_local();
    test_link_observations_preserve_only_exact_accepted_ap();
    test_disassociate_maintains_confirmed_teardown();
    test_typed_teardown_retains_cookie_pin_until_success();
    test_typed_teardown_failure_drops_pin_without_disconnect();
    test_unacknowledged_typed_replacement_survives_failed_teardown();
    test_faulted_disassociate_cancels_unacknowledged_replacement();
    test_faulted_committed_leave_does_not_schedule_teardown();
    test_close_phase_model_cancels_unacknowledged_replacement();
    test_offline_cancels_unacknowledged_failed_replacement();
    test_failed_old_teardown_reports_only_current_typed_cookie();
    test_failed_old_teardown_latches_cookie_for_late_waiter();
    test_cancelled_replacement_gets_no_failed_teardown_cookie();
    test_teardown_failure_never_publishes_disconnect();
    test_new_selection_during_teardown_is_preserved();
    test_cancel_during_disconnect_suppresses_typed_result();
    test_typed_teardown_supersession_resets_legacy_mode();
    test_replacement_opener_cancel_preserves_old_teardown();
    test_third_selection_preserves_old_teardown_and_new_attempt();
    test_configinterface_existing_configuration_semantics();
    test_resource_valid_requires_abi10_callbacks();
    test_onevent_dispatch_event_rearm_and_abort();
    test_onevent_rejects_invalid_mask_and_full_capacity();
    test_suite_cleanup();
    printf("SANA association queue: %lu checks, %u replies, %u/%u vector alloc/free, %u association wipes\n",
           checks, reply_calls, allocation_calls, free_calls, wipe_checks);
    return 0;
}
