/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host-only tests for source-extracted SANA-II scan functions. The radio,
 * Exec semaphore/list operations, pool, and request finisher are deterministic
 * fakes. They test copying/projection/publication logic, not target scheduling,
 * pool lifetime integration, or hardware behavior. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *APTR;
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef int32_t LONG;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int BOOL;

#define EXEC_TYPES_H 1
#define UTILITY_TAGITEM_H 1
typedef ULONG Tag;
struct TagItem {
    Tag ti_Tag __attribute__((aligned(sizeof(IPTR))));
    IPTR ti_Data __attribute__((aligned(sizeof(IPTR))));
} __attribute__((packed));
#define TAG_DONE 0UL
#define TAG_IGNORE 1UL
#define TAG_MORE 2UL
#define TAG_SKIP 3UL
#define TAG_USER (1UL << 31)
#include <devices/sana2wireless.h>
#include "../network/hosted_scan.h"

#define TRUE 1
#define FALSE 0
#define IOERR_ABORTED (-2)
#define S2ERR_NO_RESOURCES 1
#define S2ERR_OUTOFSERVICE 10
#define S2WERR_GENERIC_ERROR 0u
#define P4WLAN_MAX_FRAME 1518u

struct MinNode {
    struct MinNode *mln_Succ;
    struct MinNode *mln_Pred;
};

struct MinList {
    struct MinNode *mlh_Head;
    struct MinNode *mlh_Tail;
    struct MinNode *mlh_TailPred;
};

struct SignalSemaphore {
    unsigned depth;
};

struct Task { ULONG unused; };
struct Hook { ULONG unused; };
struct Sana2DeviceStats { ULONG unused[8]; };
struct Sana2DeviceQuery { ULONG unused[8]; };
struct p4wlan_opener;
struct p4wlan_request;
struct p4wlan_unit;
struct p4_wifi_scan_result;
struct P4WifiBase;

struct P4WifiBase {
    BOOL (*scan)(struct P4WifiBase *, struct p4_wifi_scan_result *);
};

struct IORequest {
    BYTE io_Error;
};

struct IOSana2Req {
    struct IORequest ios2_Req;
    ULONG ios2_WireError;
    APTR ios2_StatData;
};

typedef BOOL (*p4wlan_copy_fn)(APTR, APTR, ULONG);

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

#define REMOVE(node) minlist_remove((struct MinNode *)(node))

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore);
static void ObtainSemaphore(struct SignalSemaphore *semaphore);
static void ReleaseSemaphore(struct SignalSemaphore *semaphore);
static APTR AllocPooled(APTR pool, IPTR size);
static void FreePooled(APTR pool, APTR memory, IPTR size);
static void p4wlan_finish_request(struct p4wlan_unit *unit,
                                 struct p4wlan_request *item);

#include "../network/scan_projection.h"
#include "sana_scan_extract.inc"

static unsigned long checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

static unsigned pool_allocations;
static unsigned pool_frees;
static BOOL pool_failure;
static APTR last_pool_pointer;
static IPTR last_pool_size;
static unsigned finish_count;
static unsigned radio_calls;
static BOOL radio_result;
static BOOL radio_fill_data;
static BOOL radio_fault;
static BOOL radio_cancel;
static BOOL radio_close;
static BOOL radio_offline;
static int32_t radio_status;
static size_t radio_record_count;
static BOOL lock_violation;
static struct p4wlan_unit *active_unit;
static struct p4wlan_request *active_item;

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    if (semaphore->depth)
        return FALSE;
    semaphore->depth = 1u;
    return TRUE;
}

static void ObtainSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore->depth == 0u);
    semaphore->depth = 1u;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore->depth == 1u);
    semaphore->depth = 0u;
}

static APTR AllocPooled(APTR pool, IPTR size)
{
    APTR memory;
    (void)pool;
    last_pool_size = size;
    if (pool_failure)
        return NULL;
    memory = calloc(1u, (size_t)size);
    CHECK(memory != NULL);
    ++pool_allocations;
    last_pool_pointer = memory;
    return memory;
}

static void FreePooled(APTR pool, APTR memory, IPTR size)
{
    (void)pool;
    CHECK(memory != NULL);
    CHECK(size == last_pool_size);
    CHECK(memory == last_pool_pointer);
    ++pool_frees;
    free(memory);
    last_pool_pointer = NULL;
}

static void p4wlan_finish_request(struct p4wlan_unit *unit,
                                  struct p4wlan_request *item)
{
    CHECK(unit->lock.depth == 0u);
    CHECK(item->request != NULL);
    item->request->ios2_Req.io_Error = item->completion_error;
    item->request->ios2_WireError = item->completion_wire;
    ++finish_count;
}

static void fill_record(struct p4_hosted_scan_record *record,
                        const UBYTE *ssid, size_t ssid_size,
                        ULONG channel, int32_t rssi)
{
    size_t i;
    memset(record, 0, sizeof(*record));
    for (i = 0; i < sizeof(record->bssid); ++i)
        record->bssid[i] = (UBYTE)(0x10u + i);
    record->ssid_size = ssid_size;
    if (ssid_size != 0 && ssid_size <= sizeof(record->ssid))
        memcpy(record->ssid, ssid, ssid_size);
    record->primary = channel;
    record->rssi = rssi;
    record->authmode = 3;
    record->pairwise_cipher = 4;
    record->group_cipher = 3;
}

static BOOL fake_radio_scan(struct P4WifiBase *radio,
                            struct p4_wifi_scan_result *output)
{
    (void)radio;
    ++radio_calls;
    if (active_unit->lock.depth != 0u)
        lock_violation = TRUE;
    if (radio_fill_data) {
        static const UBYTE ssid[] = {'n', 'e', 't'};
        output->aps.status = 0;
        output->aps.number = 1u;
        output->aps.record_count = 1u;
        fill_record(&output->aps.records[0], ssid, sizeof(ssid), 6u, -47);
    }
    if (radio_status)
        output->aps.status = radio_status;
    if (radio_record_count)
        output->aps.record_count = radio_record_count;
    if (radio_fault)
        active_unit->callback_fault = TRUE;
    if (radio_cancel)
        active_item->cancelled = TRUE;
    if (radio_close)
        active_item->opener->closing = TRUE;
    if (radio_offline)
        active_unit->hardware_ready = FALSE;
    return radio_result;
}

static void fixture_init(struct p4wlan_unit *unit,
                         struct p4wlan_opener *opener,
                         struct p4wlan_request *item,
                         struct IOSana2Req *request,
                         struct P4WifiBase *radio,
                         BOOL allocate)
{
    memset(unit, 0, sizeof(*unit));
    memset(opener, 0, sizeof(*opener));
    memset(item, 0, sizeof(*item));
    memset(request, 0, sizeof(*request));
    memset(radio, 0, sizeof(*radio));
    minlist_init(&unit->scan_queue);
    unit->hardware_ready = TRUE;
    unit->radio = radio;
    radio->scan = fake_radio_scan;
    item->opener = opener;
    item->request = request;
    item->kind = P4WLAN_REQUEST_SCAN;
    item->state = P4WLAN_REQUEST_QUEUED;
    item->data = allocate ? (APTR)unit : NULL;
    item->write_node.mln_Succ = NULL;
    item->write_node.mln_Pred = NULL;
    minlist_addtail(&unit->scan_queue, &item->write_node);
    request->ios2_StatData = (APTR)(uintptr_t)0x1234u;
    active_unit = unit;
    active_item = item;
    radio_result = TRUE;
    radio_fill_data = FALSE;
    radio_fault = FALSE;
    radio_cancel = FALSE;
    radio_close = FALSE;
    radio_offline = FALSE;
    radio_status = 0;
    radio_record_count = 0u;
    pool_failure = FALSE;
    lock_violation = FALSE;
    radio_calls = 0u;
    finish_count = 0u;
    pool_allocations = 0u;
    pool_frees = 0u;
    last_pool_pointer = NULL;
    last_pool_size = 0u;
}

static BOOL all_zero(const UBYTE *bytes, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        if (bytes[i] != 0u)
            return FALSE;
    return TRUE;
}

static struct p4wlan_scan_public_record *record_for_list(struct TagItem *list)
{
    return (struct p4wlan_scan_public_record *)
        ((UBYTE *)list - offsetof(struct p4wlan_scan_public_record, tags));
}

static void test_security_projection(void)
{
    struct p4_hosted_scan_record record;
    struct Sana2WirelessSecuritySummary summary;
    static const struct {
        int32_t auth;
        int32_t pairwise;
        int32_t group;
        ULONG protocol;
        ULONG cipher_pair;
        ULONG cipher_group;
        ULONG flags;
    } cases[] = {
        {0, 0, 0, 0, 0, 0, S2WIRELESS_SUMF_KNOWN},
        {2, 3, 3, S2WIRELESS_PROTO_WPA, S2WIRELESS_CIPHER_TKIP,
         S2WIRELESS_CIPHER_TKIP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY},
        {2, 3, 4, S2WIRELESS_PROTO_WPA, S2WIRELESS_CIPHER_TKIP,
         S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY},
        {3, 4, 3, S2WIRELESS_PROTO_RSN, S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_CIPHER_TKIP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY},
        {3, 4, 4, S2WIRELESS_PROTO_RSN, S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY},
        {6, 4, 4, S2WIRELESS_PROTO_RSN_SAE, S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY},
        {7, 4, 4, S2WIRELESS_PROTO_RSN | S2WIRELESS_PROTO_RSN_SAE,
         S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP,
         S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY}
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&record, 0, sizeof(record));
        record.authmode = cases[i].auth;
        record.pairwise_cipher = cases[i].pairwise;
        record.group_cipher = cases[i].group;
        memset(&summary, 0xa5, sizeof(summary));
        p4wlan_scan_security(&record, &summary);
        CHECK(summary.Size == sizeof(summary));
        CHECK(summary.Version == S2WIRELESS_SUMMARY_VERSION);
        CHECK(summary.Flags == cases[i].flags);
        CHECK(summary.Protocols == cases[i].protocol);
        CHECK(summary.KeyManagement == (cases[i].protocol ==
            S2WIRELESS_PROTO_RSN_SAE ? S2WIRELESS_KEY_SAE :
            (cases[i].protocol == (S2WIRELESS_PROTO_RSN |
                                   S2WIRELESS_PROTO_RSN_SAE) ?
                S2WIRELESS_KEY_PSK | S2WIRELESS_KEY_SAE :
                (cases[i].protocol ? S2WIRELESS_KEY_PSK : 0u))));
        CHECK(summary.PairwiseCiphers == cases[i].cipher_pair);
        CHECK(summary.GroupCiphers == cases[i].cipher_group);
    }

    {
        static const int32_t unsupported[][3] = {
            {1, 3, 4}, {2, 0, 4}, {2, 3, 0}, {2, 5, 4},
            {2, 4, 5}, {3, 6, 4}, {4, 4, 4}, {-1, 3, 3},
            {6, 3, 4}, {6, 4, 3}, {6, 5, 4}, {6, 4, 5},
            {6, 6, 4}, {6, 4, 6}, {6, 0, 4}, {6, 4, 0},
            {7, 3, 4}, {7, 4, 3}, {7, 5, 4}, {7, 4, 5},
            {7, 6, 4}, {7, 4, 6}, {7, 0, 4}, {7, 4, 0},
            {7, -1, 4}, {7, 4, -1},
            {8, 4, 4}, {9, 4, 4}, {INT32_MAX, 4, 4}
        };
        for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
            memset(&record, 0, sizeof(record));
            record.authmode = unsupported[i][0];
            record.pairwise_cipher = unsupported[i][1];
            record.group_cipher = unsupported[i][2];
            memset(&summary, 0xa5, sizeof(summary));
            p4wlan_scan_security(&record, &summary);
            CHECK(summary.Flags == S2WIRELESS_SUMF_PRIVACY);
            CHECK(summary.Protocols == 0u && summary.KeyManagement == 0u);
            CHECK(summary.PairwiseCiphers == 0u &&
                  summary.GroupCiphers == 0u);
        }
    }
    memset(&summary, 0xa5, sizeof(summary));
    p4wlan_scan_security(NULL, &summary);
    CHECK(summary.Flags == S2WIRELESS_SUMF_PRIVACY);
    CHECK(summary.Protocols == 0u && summary.KeyManagement == 0u);
    p4wlan_scan_security(NULL, NULL);
}

static void test_record_filter_and_ssid_bounds(void)
{
    struct p4_hosted_scan_record record;
    struct p4wlan_request item;
    UBYTE ssid32[32];
    UBYTE ssid33[33];
    UBYTE embedded[] = {'a', 0, 'b'};
    size_t i;

    memset(&item, 0, sizeof(item));
    fill_record(&record, NULL, 0u, 1u, 0);
    CHECK(p4wlan_scan_record_matches(&record, &item));
    record.primary = 14u;
    CHECK(p4wlan_scan_record_matches(&record, &item));
    record.primary = 15u;
    CHECK(!p4wlan_scan_record_matches(&record, &item));
    record.primary = 0u;
    CHECK(!p4wlan_scan_record_matches(&record, &item));

    for (i = 0; i < sizeof(ssid32); ++i)
        ssid32[i] = (UBYTE)('A' + (i % 26u));
    memset(&item, 0, sizeof(item));
    fill_record(&record, ssid32, sizeof(ssid32), 11u, -81);
    CHECK(p4wlan_scan_record_matches(&record, &item));
    item.scan_filter_set = TRUE;
    memcpy(item.scan_filter, ssid32, sizeof(ssid32));
    item.scan_filter[32] = 0u;
    CHECK(p4wlan_scan_record_matches(&record, &item));
    item.scan_filter[31] ^= 1u;
    CHECK(!p4wlan_scan_record_matches(&record, &item));
    item.scan_filter[31] ^= 1u;
    item.scan_filter[32] = 'x';
    CHECK(!p4wlan_scan_record_matches(&record, &item));

    memset(&item, 0, sizeof(item));
    fill_record(&record, ssid32, sizeof(ssid32), 1u, 0);
    record.ssid_size = 33u;
    memset(ssid33, 'x', sizeof(ssid33));
    memcpy(record.ssid, ssid33, sizeof(record.ssid));
    CHECK(!p4wlan_scan_record_matches(&record, &item));
    fill_record(&record, embedded, sizeof(embedded), 1u, 0);
    CHECK(!p4wlan_scan_record_matches(&record, &item));
    fill_record(&record, (const UBYTE *)"z", 1u, 1u, 0);
    item.scan_filter_set = TRUE;
    item.scan_filter[0] = 'z';
    item.scan_filter[1] = 0u;
    CHECK(p4wlan_scan_record_matches(&record, &item));
    item.scan_filter[1] = 'q';
    CHECK(!p4wlan_scan_record_matches(&record, &item));
}

static void test_copy_filter_tag_chains(void)
{
    struct p4wlan_request item;
    UBYTE ssid32[33];
    UBYTE ssid33[34];
    struct TagItem second[] = {{S2INFO_SSID, 0}, {TAG_DONE, 0}};
    struct TagItem chain[] = {
        {TAG_IGNORE, 77u}, {TAG_MORE, (IPTR)second}, {TAG_DONE, 0}
    };
    struct TagItem skip[] = {
        {TAG_SKIP, 1u}, {S2INFO_SSID, (IPTR)"ignored"},
        {S2INFO_SSID, (IPTR)"chosen"}, {TAG_DONE, 0}
    };
    struct TagItem null_then_duplicate[] = {
        {S2INFO_SSID, 0}, {S2INFO_SSID, 0}, {TAG_DONE, 0}
    };
    struct TagItem duplicate[] = {
        {S2INFO_SSID, (IPTR)"one"}, {S2INFO_SSID, (IPTR)"two"},
        {TAG_DONE, 0}
    };
    struct TagItem unsupported[] = {{TAG_USER + 99u, 0}, {TAG_DONE, 0}};
    struct TagItem no_term[P4WLAN_TAG_LIMIT];
    struct TagItem self_more[1];
    struct TagItem overflow_skip[] = {{TAG_SKIP, (IPTR)-1}, {TAG_DONE, 0}};
    size_t i;

    memset(&item, 0, sizeof(item));
    CHECK(p4wlan_scan_copy_filter(&item, NULL));
    CHECK(!item.scan_filter_set);

    memset(&item, 0, sizeof(item));
    second[0].ti_Data = (IPTR)"more";
    CHECK(p4wlan_scan_copy_filter(&item, chain));
    CHECK(item.scan_filter_set && strcmp((const char *)item.scan_filter,
                                         "more") == 0);

    memset(&item, 0, sizeof(item));
    CHECK(p4wlan_scan_copy_filter(&item, skip));
    CHECK(strcmp((const char *)item.scan_filter, "chosen") == 0);

    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, null_then_duplicate));
    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, duplicate));
    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, unsupported));
    memset(&item, 0, sizeof(item));
    CHECK(p4wlan_scan_copy_filter(&item, (const struct TagItem *)
                                  (uintptr_t)0));

    for (i = 0; i < 32u; ++i)
        ssid32[i] = (UBYTE)('a' + (i % 26u));
    ssid32[32] = 0u;
    {
        struct TagItem exact[] = {{S2INFO_SSID, (IPTR)ssid32}, {TAG_DONE, 0}};
        memset(&item, 0, sizeof(item));
        CHECK(p4wlan_scan_copy_filter(&item, exact));
        CHECK(item.scan_filter_set && memcmp(item.scan_filter, ssid32, 33u) == 0);
    }
    memset(ssid33, 'b', 33u);
    ssid33[33] = 0u;
    {
        struct TagItem too_long[] = {{S2INFO_SSID, (IPTR)ssid33}, {TAG_DONE, 0}};
        memset(&item, 0, sizeof(item));
        CHECK(!p4wlan_scan_copy_filter(&item, too_long));
    }

    for (i = 0; i < sizeof(no_term) / sizeof(no_term[0]); ++i) {
        no_term[i].ti_Tag = TAG_IGNORE;
        no_term[i].ti_Data = 0u;
    }
    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, no_term));
    self_more[0].ti_Tag = TAG_MORE;
    self_more[0].ti_Data = (IPTR)self_more;
    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, self_more));
    memset(&item, 0, sizeof(item));
    CHECK(!p4wlan_scan_copy_filter(&item, overflow_skip));
}

static void test_scan_success_owned_publication(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct p4wlan_request item;
    struct IOSana2Req request;
    struct P4WifiBase radio;
    struct p4wlan_scan_publication *publication;
    struct p4wlan_scan_public_record *record;
    struct TagItem *list;
    APTR sentinel = (APTR)(uintptr_t)0x1234u;

    fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
    radio_fill_data = TRUE;
    memset(&unit.scan_result, 0xa5, sizeof(unit.scan_result));
    CHECK(p4wlan_scan_one(&unit, &radio));
    CHECK(radio_calls == 1u && finish_count == 1u);
    CHECK(!lock_violation && unit.lock.depth == 0u);
    CHECK(item.state == P4WLAN_REQUEST_COMPLETING);
    CHECK(item.completion_error == 0 && item.completion_wire == 0u);
    CHECK(item.completion_sets_length && item.completion_length == 1u);
    CHECK(request.ios2_StatData != sentinel);
    CHECK(all_zero((const UBYTE *)&unit.scan_result, sizeof(unit.scan_result)));
    CHECK(pool_allocations == 1u && pool_frees == 0u);

    publication = (struct p4wlan_scan_publication *)last_pool_pointer;
    CHECK(publication != NULL);
    CHECK(request.ios2_StatData == publication->lists);
    list = publication->lists[0];
    CHECK(list == publication->records[0].tags);
    record = record_for_list(list);
    CHECK(record == &publication->records[0]);
    CHECK(strcmp((const char *)record->ssid, "net") == 0);
    CHECK(record->bssid[0] == 0x10u && record->bssid[5] == 0x15u);
    CHECK(record->security.Flags ==
          (S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY));
    CHECK(record->tags[0].ti_Tag == S2INFO_SSID &&
          record->tags[0].ti_Data == (IPTR)record->ssid);
    CHECK(record->tags[1].ti_Tag == S2INFO_BSSID &&
          record->tags[1].ti_Data == (IPTR)record->bssid);
    CHECK(record->tags[2].ti_Tag == S2INFO_Channel &&
          record->tags[2].ti_Data == 6u);
    CHECK(record->tags[3].ti_Tag == S2INFO_Signal &&
          (LONG)record->tags[3].ti_Data == -47);
    CHECK(record->tags[4].ti_Tag == S2INFO_Capabilities &&
          record->tags[4].ti_Data == 0x10u);
    CHECK(record->tags[5].ti_Tag == S2INFO_SecuritySummary &&
          record->tags[5].ti_Data == (IPTR)&record->security);
    CHECK(record->tags[6].ti_Tag == TAG_DONE);

    FreePooled(&unit, publication, sizeof(*publication));
    CHECK(pool_frees == 1u && last_pool_pointer == NULL);
}

static void test_zero_result_and_filtered_records(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct p4wlan_request item;
    struct IOSana2Req request;
    struct P4WifiBase radio;
    struct p4wlan_scan_publication *publication;
    APTR sentinel = (APTR)(uintptr_t)0x1234u;

    fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
    CHECK(p4wlan_scan_one(&unit, &radio));
    CHECK(item.completion_error == 0 && item.completion_sets_length);
    CHECK(item.completion_length == 0u && request.ios2_StatData != sentinel);
    publication = (struct p4wlan_scan_publication *)last_pool_pointer;
    CHECK(publication != NULL && publication->lists[0] == NULL);
    CHECK(pool_allocations == 1u && pool_frees == 0u);
    FreePooled(&unit, publication, sizeof(*publication));

    fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
    radio_fill_data = TRUE;
    item.scan_filter_set = TRUE;
    item.scan_filter[0] = 'x';
    item.scan_filter[1] = 0u;
    CHECK(p4wlan_scan_one(&unit, &radio));
    CHECK(item.completion_error == 0 && item.completion_length == 0u);
    publication = (struct p4wlan_scan_publication *)last_pool_pointer;
    CHECK(publication != NULL && publication->lists[0] == NULL);
    CHECK(all_zero((const UBYTE *)&unit.scan_result, sizeof(unit.scan_result)));
    FreePooled(&unit, publication, sizeof(*publication));
}

static void test_scan_failures_and_late_abort(void)
{
    unsigned mode;
    for (mode = 0; mode < 7u; ++mode) {
        struct p4wlan_unit unit;
        struct p4wlan_opener opener;
        struct p4wlan_request item;
        struct IOSana2Req request;
        struct P4WifiBase radio;
        APTR sentinel = (APTR)(uintptr_t)0x1234u;

        fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
        radio_fill_data = TRUE;
        if (mode == 0u)
            radio_result = FALSE;
        else if (mode == 1u)
            radio_status = -3;
        else if (mode == 2u)
            radio_fault = TRUE;
        else if (mode == 3u)
            radio_cancel = TRUE;
        else if (mode == 4u)
            radio_close = TRUE;
        else if (mode == 5u)
            pool_failure = TRUE;
        else
            radio_offline = TRUE;
        CHECK(p4wlan_scan_one(&unit, &radio));
        CHECK(finish_count == 1u && radio_calls == 1u);
        CHECK(unit.lock.depth == 0u && !lock_violation);
        CHECK(request.ios2_StatData == sentinel);
        CHECK(!item.completion_sets_length);
        CHECK(item.completion_length == 0u);
        CHECK(all_zero((const UBYTE *)&unit.scan_result, sizeof(unit.scan_result)));

        if (mode == 5u) {
            CHECK(item.completion_error == S2ERR_NO_RESOURCES);
            CHECK(pool_allocations == 0u && pool_frees == 0u);
        } else if (mode >= 2u && mode <= 4u) {
            CHECK(item.completion_error == IOERR_ABORTED);
            CHECK(pool_allocations == 1u && pool_frees == 1u);
            CHECK(last_pool_pointer == NULL);
        } else if (mode == 6u) {
            CHECK(item.completion_error == IOERR_ABORTED);
            CHECK(pool_allocations == 1u && pool_frees == 1u);
        } else {
            CHECK(item.completion_error == S2ERR_OUTOFSERVICE);
            CHECK(pool_allocations == 0u && pool_frees == 0u);
        }
        CHECK(request.ios2_Req.io_Error == item.completion_error);
        CHECK(request.ios2_WireError == S2WERR_GENERIC_ERROR);
    }

    {
        struct p4wlan_unit unit;
        struct p4wlan_opener opener;
        struct p4wlan_request item;
        struct IOSana2Req request;
        struct P4WifiBase radio;
        fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
        radio_fill_data = FALSE;
        radio_record_count = P4_HOSTED_SCAN_MAX_RECORDS + 1u;
        CHECK(p4wlan_scan_one(&unit, &radio));
        CHECK(item.completion_error == S2ERR_OUTOFSERVICE);
        CHECK(pool_allocations == 0u && pool_frees == 0u);
        CHECK(request.ios2_StatData == (APTR)(uintptr_t)0x1234u);
        CHECK(all_zero((const UBYTE *)&unit.scan_result, sizeof(unit.scan_result)));
    }
}

static void test_scan_preconditions_and_no_work(void)
{
    struct p4wlan_unit unit;
    struct p4wlan_opener opener;
    struct p4wlan_request item;
    struct IOSana2Req request;
    struct P4WifiBase radio;

    fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
    minlist_init(&unit.scan_queue);
    CHECK(!p4wlan_scan_one(&unit, &radio));
    CHECK(finish_count == 0u && radio_calls == 0u);

    fixture_init(&unit, &opener, &item, &request, &radio, TRUE);
    unit.lock.depth = 1u;
    CHECK(!p4wlan_scan_one(&unit, &radio));
    CHECK(finish_count == 0u && radio_calls == 0u);
    unit.lock.depth = 0u;
}

int main(void)
{
    test_security_projection();
    test_record_filter_and_ssid_bounds();
    test_copy_filter_tag_chains();
    test_scan_success_owned_publication();
    test_zero_result_and_filtered_records();
    test_scan_failures_and_late_abort();
    test_scan_preconditions_and_no_work();
    printf("wifi SANA scan host tests: %lu checks\n", checks);
    return 0;
}
