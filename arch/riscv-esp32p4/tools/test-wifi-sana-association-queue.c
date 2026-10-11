/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Source-extracted association lifecycle tests. The selected SANA BeginIO
 * switch arms are the production text; this harness does not model the full
 * BeginIO prologue or device scheduler. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    ULONG ios2_DataLength;
    APTR ios2_BufferManagement;
    APTR ios2_Data;
    APTR ios2_StatData;
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
static struct IOSana2Req *replied[128];
static BOOL fail_next_vector_allocation;
static BOOL fail_next_pool_allocation;
static APTR pool_memory;
static struct p4wlan_unit *active_unit;
static struct p4wlan_opener *active_opener;
static struct IOSana2Req *replacement_request;
static const struct TagItem *replacement_tags;
static BOOL replace_during_associate;
static BOOL associate_returns_success;
static BOOL association_allocation_expected;
static BOOL offline_during_associate;
static BOOL disconnect_returns_success;
static BOOL replace_during_disconnect;
static ULONG mismatch_snapshot_field;
static unsigned associate_calls;
static unsigned unlocked_association_replies;
static unsigned unacknowledged_service_skips;
static unsigned associate_cancel_checks;
static unsigned offline_cancel_checks;
static unsigned disconnect_calls;
static BOOL reuse_request_during_association_reply;
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
    ULONG command = request->ios2_Req.io_Command;
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
    ReleaseSemaphore(&unit->lock);
    association_allocation_expected = FALSE;
    p4wlan_finish_done(unit, &done);
    p4wlan_term_io(request);
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
    replace_during_associate = FALSE;
    associate_returns_success = TRUE;
    mismatch_snapshot_field = 0u;
    replacement_request = NULL;
    replacement_tags = NULL;
    association_allocation_expected = FALSE;
    offline_during_associate = FALSE;
    offline_cancel_checks = 0u;
    disconnect_returns_success = TRUE;
    replace_during_disconnect = FALSE;
    disconnect_calls = 0u;
    reuse_request_during_association_reply = FALSE;
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

static void make_disassociate_tags(struct TagItem tags[2])
{
    tags[0].ti_Tag = S2INFO_Disassociate;
    tags[0].ti_Data = TRUE;
    tags[1].ti_Tag = TAG_DONE;
    tags[1].ti_Data = 0u;
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
    test_beginio_refusals_preserve_selection_and_link();
    test_cookie_copy_and_reply_ack_fencing();
    test_replacement_owns_and_wipes_attempt();
    test_service_commit_and_owned_network_snapshot();
    test_confirmed_snapshot_mismatch_never_connects();
    test_active_attempt_replacement_and_flush_cancellation();
    test_flush_releases_committed_pin_without_disconnect();
    test_success_and_failure_wipe_secrets();
    test_offline_cancels_pending_attempt();
    test_offline_cancels_active_attempt_before_commit();
    test_confirmed_down_revokes_committed_link();
    test_link_observations_preserve_only_exact_accepted_ap();
    test_disassociate_maintains_confirmed_teardown();
    test_teardown_failure_never_publishes_disconnect();
    test_new_selection_during_teardown_is_preserved();
    test_resource_valid_requires_abi10_callbacks();
    test_suite_cleanup();
    printf("SANA association queue: %lu checks, %u replies, %u/%u vector alloc/free, %u association wipes\n",
           checks, reply_calls, allocation_calls, free_calls, wipe_checks);
    return 0;
}
