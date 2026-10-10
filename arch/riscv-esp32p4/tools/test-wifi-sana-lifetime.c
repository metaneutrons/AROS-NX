/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host lifetime model for the actual SANA-II request helper bodies extracted
 * by run-wifi-sana-lifetime-tests.sh. Exec messages, allocation and semaphore
 * operations are deliberately small fakes; this is not scheduler proof. */
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
#define IOERR_ABORTED (-2)
#define IOERR_BADLENGTH (-3)
#define S2WERR_GENERIC_ERROR 0x00000001u
#define IOF_QUICK 0x01u
#define NT_MESSAGE 5u
#define MEMF_PUBLIC 0x01u
#define MEMF_CLEAR 0x02u
#define P4WLAN_MAX_FRAME 1518u
#define SIGF_SINGLE 0x10u

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

struct Task {
    ULONG id;
};

struct Device { ULONG unused; };
struct Unit;
struct P4WifiBase;
struct Hook { ULONG unused; };
struct ExecBase { ULONG unused; };
struct Sana2DeviceStats { ULONG words[16]; };
struct Sana2DeviceQuery { ULONG words[16]; };

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
    APTR ios2_BufferManagement;
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

#define NEWLIST(list) minlist_init((struct MinList *)(list))
#define ADDTAIL(list, node) \
    minlist_addtail((struct MinList *)(list), (struct MinNode *)(node))
#define REMOVE(node) minlist_remove((struct MinNode *)(node))
#define IsMinListEmpty(list) \
    ((list)->mlh_TailPred == (struct MinNode *)(list))

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
};

static struct allocation_record allocations[128];
static unsigned allocation_slots;
static unsigned allocation_calls;
static unsigned free_calls;
static unsigned association_wipe_checks;
static APTR association_to_wipe_check;
static unsigned reply_calls;
static unsigned signal_calls;
static unsigned signal_alloc_calls;
static unsigned signal_free_calls;
static unsigned set_signal_calls;
static unsigned wait_calls;
static unsigned order_counter;
static unsigned reply_order;
static unsigned signal_order;
static ULONG wait_mask_seen;
static BYTE alloc_signal_result = -1;
static struct IOSana2Req *replied[128];
static struct p4wlan_unit *active_unit;
static void (*reply_hook)(struct IOSana2Req *request);
static void (*wait_hook)(ULONG signal_mask);
static struct Task fake_task = {9u};

static APTR AllocVec(IPTR size, ULONG flags);
static void FreeVec(APTR memory);
static void ReplyMsg(struct Message *message);
static void Signal(struct Task *task, ULONG mask);
static void ObtainSemaphore(struct SignalSemaphore *semaphore);
static void ReleaseSemaphore(struct SignalSemaphore *semaphore);
static BYTE AllocSignal(LONG signal_number);
static void FreeSignal(ULONG signal_bit);
static struct Task *FindTask(APTR name);
static ULONG SetSignal(ULONG new_signals, ULONG signal_mask);
static ULONG Wait(ULONG signal_mask);

#define AROS_LH1(result, name, argument, base_type, base_name, vector, lib) \
    result name(argument, base_type base_name)
#define AROS_LHA(type, name, reg) type name
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
#define A1 0
#define LIBBASETYPEPTR struct esp32p4wlan_base *
#define GM_UNIQUENAME(name) name

#include "sana_lifetime_extract.inc"

static APTR AllocVec(IPTR size, ULONG flags)
{
    void *memory;
    if (flags & MEMF_CLEAR)
        memory = calloc(1u, (size_t)size);
    else
        memory = malloc((size_t)size);
    CHECK(memory != NULL);
    CHECK(allocation_slots < sizeof(allocations) / sizeof(allocations[0]));
    allocations[allocation_slots].pointer = memory;
    allocations[allocation_slots].live = TRUE;
    ++allocation_slots;
    ++allocation_calls;
    return memory;
}

static void FreeVec(APTR memory)
{
    unsigned index = allocation_slots;
    while (index != 0u) {
        --index;
        if (allocations[index].pointer == memory && allocations[index].live)
            break;
    }
    CHECK(index < allocation_slots);
    CHECK(allocations[index].pointer == memory);
    CHECK(allocations[index].live);
    if (memory == association_to_wipe_check) {
        const UBYTE *bytes = memory;
        size_t offset;
        for (offset = 0; offset < sizeof(struct p4wlan_association); ++offset)
            CHECK(bytes[offset] == 0u);
        ++association_wipe_checks;
        association_to_wipe_check = NULL;
    }
    allocations[index].live = FALSE;
    ++free_calls;
    free(memory);
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

static void ReplyMsg(struct Message *message)
{
    struct IOSana2Req *request = (struct IOSana2Req *)message;
    CHECK(message != NULL);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 0u);
    CHECK(reply_calls < sizeof(replied) / sizeof(replied[0]));
    replied[reply_calls++] = request;
    reply_order = ++order_counter;
    if (reply_hook)
        reply_hook(request);
}

static void Signal(struct Task *task, ULONG mask)
{
    CHECK(task != NULL);
    CHECK(mask != 0u);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 1u);
    ++signal_calls;
    signal_order = ++order_counter;
}

static BYTE AllocSignal(LONG signal_number)
{
    CHECK(signal_number == -1);
    ++signal_alloc_calls;
    return alloc_signal_result;
}

static void FreeSignal(ULONG signal_bit)
{
    CHECK(signal_bit < 32u);
    ++signal_free_calls;
}

static struct Task *FindTask(APTR name)
{
    CHECK(name == NULL);
    return &fake_task;
}

static ULONG SetSignal(ULONG new_signals, ULONG signal_mask)
{
    CHECK(new_signals == 0u);
    CHECK(signal_mask != 0u);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 1u);
    ++set_signal_calls;
    return 0u;
}

static ULONG Wait(ULONG signal_mask)
{
    CHECK(signal_mask != 0u);
    CHECK(active_unit != NULL);
    CHECK(active_unit->lock.depth == 0u);
    ++wait_calls;
    wait_mask_seen = signal_mask;
    if (wait_hook)
        wait_hook(signal_mask);
    return signal_mask;
}

static unsigned live_allocations(void)
{
    unsigned index;
    unsigned count = 0u;
    for (index = 0u; index < allocation_slots; ++index)
        if (allocations[index].live)
            ++count;
    return count;
}

static int minlist_count(const struct MinList *list)
{
    const struct MinNode *node = list->mlh_Head;
    int count = 0;
    while (node && node->mln_Succ) {
        CHECK(count < 128);
        ++count;
        node = node->mln_Succ;
    }
    return count;
}

static int minlist_contains(const struct MinList *list,
                            const struct MinNode *wanted)
{
    const struct MinNode *node = list->mlh_Head;
    int count = 0;
    while (node && node->mln_Succ) {
        CHECK(count < 128);
        if (node == wanted)
            return 1;
        ++count;
        node = node->mln_Succ;
    }
    return 0;
}

static struct p4wlan_request *find_request(
    struct p4wlan_opener *opener, struct IOSana2Req *request)
{
    struct MinNode *node;
    for (node = opener->requests.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        struct p4wlan_request *item =
            (struct p4wlan_request *)((UBYTE *)node -
                offsetof(struct p4wlan_request, owner_node));
        if (item->request == request)
            return item;
    }
    return NULL;
}

struct fixture {
    struct esp32p4wlan_base base;
    struct p4wlan_opener opener;
};

/* Keep the fixture call sites compact while using the header's real base and
 * unit layout for the library-base argument passed to the extracted AbortIO. */
#define unit base.unit

static void reset_observations(void)
{
    allocation_slots = 0u;
    allocation_calls = 0u;
    free_calls = 0u;
    association_wipe_checks = 0u;
    association_to_wipe_check = NULL;
    reply_calls = 0u;
    signal_calls = 0u;
    signal_alloc_calls = 0u;
    signal_free_calls = 0u;
    set_signal_calls = 0u;
    wait_calls = 0u;
    order_counter = 0u;
    reply_order = 0u;
    signal_order = 0u;
    wait_mask_seen = 0u;
    alloc_signal_result = -1;
    memset(allocations, 0, sizeof(allocations));
    memset(replied, 0, sizeof(replied));
    reply_hook = NULL;
    wait_hook = NULL;
}

static void fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    minlist_init(&fixture->unit.openers);
    minlist_init(&fixture->unit.write_queue);
    minlist_init(&fixture->opener.requests);
    fixture->unit.initialized = TRUE;
    fixture->opener.refs = 1u; /* Close's owning reference. */
    minlist_addtail(&fixture->unit.openers, &fixture->opener.unit_node);
    active_unit = &fixture->unit;
}

static void request_init(struct IOSana2Req *request,
                         struct fixture *fixture)
{
    memset(request, 0, sizeof(*request));
    request->ios2_Req.io_Message.mn_Length = sizeof(*request);
    request->ios2_Req.io_Flags = IOF_QUICK;
    request->ios2_Req.io_Device = (struct Device *)&fixture->base;
    request->ios2_Req.io_Unit = (struct Unit *)&fixture->unit;
    request->ios2_BufferManagement = &fixture->opener;
}

static struct p4wlan_request *enqueue_request(
    struct fixture *fixture, struct IOSana2Req *request, ULONG kind)
{
    struct p4wlan_request *item;
    item = p4wlan_alloc_request(&fixture->opener, request, kind);
    CHECK(item != NULL);
    ObtainSemaphore(&fixture->unit.lock);
    p4wlan_enqueue_locked(&fixture->unit, &fixture->opener, item);
    ReleaseSemaphore(&fixture->unit.lock);
    CHECK(!(request->ios2_Req.io_Flags & IOF_QUICK));
    CHECK(request->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE);
    return item;
}

static void check_fixture_empty(const struct fixture *fixture)
{
    CHECK(IsMinListEmpty(&fixture->opener.requests));
    CHECK(IsMinListEmpty(&fixture->unit.write_queue));
    CHECK(fixture->opener.refs == 1u);
    CHECK(fixture->opener.request_count == 0u);
    CHECK(fixture->opener.read_count == 0u);
    CHECK(fixture->opener.write_count == 0u);
    CHECK(fixture->opener.event_count == 0u);
    CHECK(fixture->unit.request_count == 0u);
    CHECK(fixture->unit.read_count == 0u);
    CHECK(fixture->unit.write_count == 0u);
    CHECK(fixture->unit.event_count == 0u);
    CHECK(live_allocations() == 0u);
}

static void test_queued_abort_unlinks_one_write(void)
{
    struct fixture fixture;
    struct IOSana2Req target, survivor;
    struct p4wlan_request *target_item;
    struct p4wlan_request *survivor_item;

    reset_observations();
    fixture_init(&fixture);
    request_init(&target, &fixture);
    request_init(&survivor, &fixture);
    target_item = enqueue_request(&fixture, &target, P4WLAN_REQUEST_WRITE);
    survivor_item = enqueue_request(&fixture, &survivor, P4WLAN_REQUEST_WRITE);
    CHECK(minlist_count(&fixture.opener.requests) == 2);
    CHECK(minlist_count(&fixture.unit.write_queue) == 2);
    CHECK(fixture.opener.refs == 3u);

    CHECK(abort_io((struct IORequest *)&target.ios2_Req, &fixture.base) == 0);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &target);
    CHECK(target.ios2_Req.io_Error == IOERR_ABORTED);
    CHECK(target.ios2_WireError == S2WERR_GENERIC_ERROR);
    CHECK(find_request(&fixture.opener, &target) == NULL);
    CHECK(!minlist_contains(&fixture.unit.write_queue,
                            &target_item->write_node));
    CHECK(minlist_contains(&fixture.unit.write_queue,
                           &survivor_item->write_node));
    CHECK(minlist_count(&fixture.unit.write_queue) == 1);
    CHECK(fixture.opener.request_count == 1u);
    CHECK(fixture.opener.write_count == 1u);
    CHECK(fixture.unit.request_count == 1u);
    CHECK(fixture.unit.write_count == 1u);
    CHECK(fixture.opener.refs == 2u);
    CHECK(free_calls == 1u);

    CHECK(abort_io((struct IORequest *)&target.ios2_Req, &fixture.base) == -1);
    CHECK(reply_calls == 1u);
    CHECK(free_calls == 1u);
    CHECK(abort_io((struct IORequest *)&survivor.ios2_Req, &fixture.base) == 0);
    CHECK(reply_calls == 2u);
    CHECK(replied[1] == &survivor);
    CHECK(free_calls == 2u);
    check_fixture_empty(&fixture);
}

static void test_active_abort_marks_without_free(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct p4wlan_request *item;
    struct MinList done;

    reset_observations();
    fixture_init(&fixture);
    request_init(&request, &fixture);
    item = enqueue_request(&fixture, &request, P4WLAN_REQUEST_READ);
    item->state = P4WLAN_REQUEST_ACTIVE;

    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) == 0);
    CHECK(item->cancelled == TRUE);
    CHECK(item->state == P4WLAN_REQUEST_ACTIVE);
    CHECK(find_request(&fixture.opener, &request) == item);
    CHECK(reply_calls == 0u);
    CHECK(free_calls == 0u);
    CHECK(fixture.opener.refs == 2u);
    CHECK(fixture.opener.request_count == 1u);

    minlist_init(&done);
    p4wlan_add_done_locked(&done, item, IOERR_ABORTED,
                           S2WERR_GENERIC_ERROR);
    CHECK(item->state == P4WLAN_REQUEST_COMPLETING);
    CHECK(minlist_count(&done) == 1);
    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &request);
    CHECK(free_calls == 1u);
    check_fixture_empty(&fixture);
}

static void test_committed_active_write_refuses_abort(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct p4wlan_request *item;
    struct MinList done;

    reset_observations();
    fixture_init(&fixture);
    request_init(&request, &fixture);
    item = enqueue_request(&fixture, &request, P4WLAN_REQUEST_WRITE);

    /* send_one is not source-extracted by this focused harness. Model the
     * descriptor state it commits at its irreversible-TX boundary: the
     * queued node is consumed, the request is active, and tx is committed. */
    REMOVE((struct Node *)&item->write_node);
    item->state = P4WLAN_REQUEST_ACTIVE;
    item->tx_committed = TRUE;
    CHECK(IsMinListEmpty(&fixture.unit.write_queue));

    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) ==
          -1);
    CHECK(item->state == P4WLAN_REQUEST_ACTIVE);
    CHECK(item->tx_committed == TRUE);
    CHECK(item->cancelled == FALSE);
    CHECK(find_request(&fixture.opener, &request) == item);
    CHECK(fixture.opener.refs == 2u);
    CHECK(fixture.opener.request_count == 1u);
    CHECK(fixture.opener.write_count == 1u);
    CHECK(fixture.unit.request_count == 1u);
    CHECK(fixture.unit.write_count == 1u);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(reply_calls == 0u);
    CHECK(free_calls == 0u);

    /* Abort refusal does not steal ownership: the ordinary transmitter
     * completion still releases and replies to the request exactly once. */
    minlist_init(&done);
    p4wlan_add_done_locked(&done, item, 0, 0);
    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &request);
    CHECK(request.ios2_Req.io_Error == 0);
    CHECK(request.ios2_WireError == 0u);
    CHECK(free_calls == 1u);
    check_fixture_empty(&fixture);
}

static void test_completing_abort_cannot_claim_descriptor(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct p4wlan_request *item;
    struct MinList done;

    reset_observations();
    fixture_init(&fixture);
    request_init(&request, &fixture);
    item = enqueue_request(&fixture, &request, P4WLAN_REQUEST_EVENT);
    minlist_init(&done);
    p4wlan_add_done_locked(&done, item, 0, 0x20u);
    CHECK(item->state == P4WLAN_REQUEST_COMPLETING);
    CHECK(minlist_contains(&done, &item->done_node));
    CHECK(find_request(&fixture.opener, &request) == item);
    CHECK(free_calls == 0u);

    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) == -1);
    CHECK(item->state == P4WLAN_REQUEST_COMPLETING);
    CHECK(minlist_contains(&done, &item->done_node));
    CHECK(reply_calls == 0u);
    CHECK(free_calls == 0u);
    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &request);
    CHECK(request.ios2_WireError == 0x20u);
    CHECK(free_calls == 1u);
    check_fixture_empty(&fixture);
}

static struct fixture *reuse_fixture;
static struct p4wlan_request *reused_item;
static struct p4wlan_request *old_item;
static unsigned frees_seen_during_reply;

static void enqueue_reused_io_during_reply(struct IOSana2Req *request)
{
    CHECK(request != NULL);
    CHECK(find_request(&reuse_fixture->opener, request) == NULL);
    CHECK(reuse_fixture->opener.refs == 2u);
    CHECK(free_calls == frees_seen_during_reply);
    CHECK(old_item != NULL);
    reused_item = p4wlan_alloc_request(&reuse_fixture->opener, request,
                                       P4WLAN_REQUEST_READ);
    CHECK(reused_item != NULL);
    ObtainSemaphore(&reuse_fixture->unit.lock);
    p4wlan_enqueue_locked(&reuse_fixture->unit, &reuse_fixture->opener,
                          reused_item);
    ReleaseSemaphore(&reuse_fixture->unit.lock);
}

static void test_immediate_io_reuse_has_no_stale_membership(void)
{
    struct fixture fixture;
    struct IOSana2Req request;

    reset_observations();
    fixture_init(&fixture);
    request_init(&request, &fixture);
    old_item = enqueue_request(&fixture, &request, P4WLAN_REQUEST_WRITE);
    reuse_fixture = &fixture;
    reused_item = NULL;
    frees_seen_during_reply = free_calls;
    reply_hook = enqueue_reused_io_during_reply;

    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) == 0);
    reply_hook = NULL;
    CHECK(reply_calls == 1u);
    CHECK(find_request(&fixture.opener, &request) == reused_item);
    CHECK(reused_item != NULL && reused_item != old_item);
    CHECK(minlist_contains(&fixture.opener.requests,
                           &reused_item->owner_node));
    CHECK(minlist_count(&fixture.opener.requests) == 1);
    CHECK(fixture.opener.request_count == 1u);
    CHECK(fixture.opener.read_count == 1u);
    CHECK(fixture.opener.write_count == 0u);
    CHECK(fixture.unit.request_count == 1u);
    CHECK(fixture.unit.read_count == 1u);
    CHECK(fixture.unit.write_count == 0u);
    CHECK(fixture.opener.refs == 2u);
    CHECK(free_calls == 1u);

    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) == 0);
    CHECK(reply_calls == 2u);
    CHECK(replied[0] == &request && replied[1] == &request);
    CHECK(free_calls == 2u);
    CHECK(abort_io((struct IORequest *)&request.ios2_Req, &fixture.base) == -1);
    CHECK(reply_calls == 2u);
    check_fixture_empty(&fixture);
}

static void test_close_waiter_signalled_after_reply(void)
{
    struct fixture fixture;
    struct IOSana2Req request;
    struct p4wlan_request *item;
    struct MinList done;
    struct Task close_task = {7u};

    reset_observations();
    fixture_init(&fixture);
    request_init(&request, &fixture);
    item = enqueue_request(&fixture, &request, P4WLAN_REQUEST_READ);
    fixture.opener.closing = TRUE;
    fixture.opener.close_waiter = TRUE;
    fixture.opener.close_task = &close_task;
    fixture.opener.close_signal = 0x40u;
    minlist_init(&done);
    p4wlan_add_done_locked(&done, item, 0, 0);

    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(signal_calls == 1u);
    CHECK(reply_order != 0u && signal_order > reply_order);
    CHECK(fixture.opener.refs == 1u);
    CHECK(free_calls == 1u);
    CHECK(IsMinListEmpty(&fixture.opener.requests));
    CHECK(fixture.opener.request_count == 0u);
    CHECK(live_allocations() == 0u);
}

static struct p4wlan_opener *drain_close_opener;
static struct p4wlan_request *drain_close_item;

static void check_close_reply_lifetime(struct IOSana2Req *request)
{
    CHECK(request == drain_close_item->request);
    CHECK(drain_close_opener->closing == TRUE);
    CHECK(drain_close_opener->close_waiter == TRUE);
    CHECK(drain_close_opener->refs == 2u);
    CHECK(find_request(drain_close_opener, request) == NULL);
    CHECK(free_calls == 0u);
}

static void complete_active_during_close_wait(ULONG signal_mask)
{
    struct MinList done;
    CHECK(signal_mask == SIGF_SINGLE);
    CHECK(active_unit->lock.depth == 0u);
    CHECK(drain_close_opener->refs == 2u);
    CHECK(drain_close_item->state == P4WLAN_REQUEST_ACTIVE);
    CHECK(drain_close_item->cancelled == TRUE);
    minlist_init(&done);
    ObtainSemaphore(&active_unit->lock);
    p4wlan_add_done_locked(&done, drain_close_item, IOERR_ABORTED,
                           S2WERR_GENERIC_ERROR);
    ReleaseSemaphore(&active_unit->lock);
    reply_hook = check_close_reply_lifetime;
    p4wlan_finish_done(active_unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(signal_calls == 1u);
    CHECK(signal_order > reply_order);
}

static void test_close_drains_when_signal_allocation_fails(void)
{
    struct fixture fixture;
    struct p4wlan_unit *close_unit;
    struct p4wlan_opener *opener;
    struct IOSana2Req active_request, close_request;
    struct p4wlan_request *item;

    reset_observations();
    fixture_init(&fixture);
    close_unit = &fixture.unit;
    opener = AllocVec(sizeof(*opener), MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(opener != NULL);
    minlist_init(&opener->requests);
    opener->refs = 1u;
    minlist_addtail(&close_unit->openers, &opener->unit_node);
    close_unit->open_count = 2u;

    memset(&active_request, 0, sizeof(active_request));
    active_request.ios2_Req.io_Message.mn_Length = sizeof(active_request);
    active_request.ios2_Req.io_Device = (struct Device *)&fixture.base;
    active_request.ios2_Req.io_Unit = (struct Unit *)close_unit;
    active_request.ios2_BufferManagement = opener;
    item = p4wlan_alloc_request(opener, &active_request, P4WLAN_REQUEST_READ);
    CHECK(item != NULL);
    ObtainSemaphore(&close_unit->lock);
    p4wlan_enqueue_locked(close_unit, opener, item);
    ReleaseSemaphore(&close_unit->lock);
    item->state = P4WLAN_REQUEST_ACTIVE;

    memset(&close_request, 0, sizeof(close_request));
    close_request.ios2_Req.io_Message.mn_Length = sizeof(close_request);
    close_request.ios2_Req.io_Unit = (struct Unit *)close_unit;
    close_request.ios2_BufferManagement = opener;
    active_unit = close_unit;
    drain_close_opener = opener;
    drain_close_item = item;
    wait_hook = complete_active_during_close_wait;
    alloc_signal_result = -1;

    CHECK(Close(&fixture.base,
                (struct IORequest *)&close_request.ios2_Req) == TRUE);
    CHECK(signal_alloc_calls == 1u);
    CHECK(signal_free_calls == 0u);
    CHECK(wait_calls == 1u);
    CHECK(wait_mask_seen == SIGF_SINGLE);
    CHECK(set_signal_calls == 2u);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &active_request);
    CHECK(reply_order != 0u && signal_order > reply_order);
    CHECK(free_calls == 2u);
    CHECK(allocation_calls == 2u);
    CHECK(live_allocations() == 0u);
    CHECK(minlist_count(&close_unit->openers) == 1);
    CHECK(minlist_contains(&close_unit->openers,
                           &fixture.opener.unit_node));
    CHECK(close_unit->open_count == 1u);
    CHECK(close_unit->request_count == 0u);
    CHECK(close_request.ios2_Req.io_Unit == NULL);
    CHECK(close_request.ios2_BufferManagement == NULL);
}

static struct p4wlan_opener *heap_opener_for_close(struct fixture *fixture)
{
    struct p4wlan_opener *opener;
    opener = AllocVec(sizeof(*opener), MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(opener != NULL);
    minlist_init(&opener->requests);
    opener->refs = 1u;
    ADDTAIL((struct List *)&fixture->unit.openers,
            (struct Node *)&opener->unit_node);
    fixture->unit.open_count = 2u;
    active_unit = &fixture->unit;
    return opener;
}

static void test_close_retires_pending_association_and_credential(void)
{
    struct fixture fixture;
    struct p4wlan_unit *attempt_unit;
    struct p4wlan_opener *opener;
    struct p4wlan_association *attempt;
    struct IOSana2Req close_request;

    reset_observations();
    fixture_init(&fixture);
    opener = heap_opener_for_close(&fixture);
    attempt_unit = &fixture.unit;
    attempt = AllocVec(sizeof(*attempt), MEMF_PUBLIC | MEMF_CLEAR);
    CHECK(attempt != NULL);
    association_to_wipe_check = attempt;
#undef unit
    attempt->unit = attempt_unit;
#define unit base.unit
    attempt->opener = opener;
    attempt->token = 3u;
    attempt->target.password_length = 8u;
    memcpy(attempt->target.password, "password", 8u);
    fixture.unit.selection_token = 3u;
    fixture.unit.pending_association = attempt;
    ++opener->refs;

    memset(&close_request, 0, sizeof(close_request));
    close_request.ios2_Req.io_Message.mn_Length = sizeof(close_request);
    close_request.ios2_Req.io_Unit = (struct Unit *)&fixture.unit;
    close_request.ios2_BufferManagement = opener;
    alloc_signal_result = -1;
    CHECK(Close(&fixture.base,
                (struct IORequest *)&close_request.ios2_Req) == TRUE);
    CHECK(fixture.unit.pending_association == NULL);
    CHECK(fixture.unit.active_association == NULL);
    CHECK(fixture.unit.selection_token == 4u);
    CHECK(fixture.unit.link_opener == NULL && !fixture.unit.link_committed);
    CHECK(close_request.ios2_Req.io_Unit == NULL);
    CHECK(close_request.ios2_BufferManagement == NULL);
    CHECK(association_wipe_checks == 1u);
    CHECK(allocation_calls == 2u && free_calls == 2u);
    CHECK(live_allocations() == 0u);
    CHECK(minlist_count(&fixture.unit.openers) == 1);
    CHECK(minlist_contains(&fixture.unit.openers,
                           &fixture.opener.unit_node));
}

static void test_close_releases_committed_link_pin(void)
{
    struct fixture fixture;
    struct p4wlan_opener *opener;
    struct IOSana2Req close_request;

    reset_observations();
    fixture_init(&fixture);
    opener = heap_opener_for_close(&fixture);
    opener->refs++;
    fixture.unit.selection_token = 8u;
    fixture.unit.link_committed = TRUE;
    fixture.unit.link_opener = opener;
    fixture.unit.link_info.ssid[0] = 'a';
    fixture.unit.link_info.ssid_length = 1u;
    fixture.unit.link_info.bssid[0] = 2u;
    fixture.unit.link_info.channel = 11u;

    memset(&close_request, 0, sizeof(close_request));
    close_request.ios2_Req.io_Message.mn_Length = sizeof(close_request);
    close_request.ios2_Req.io_Unit = (struct Unit *)&fixture.unit;
    close_request.ios2_BufferManagement = opener;
    alloc_signal_result = -1;
    CHECK(Close(&fixture.base,
                (struct IORequest *)&close_request.ios2_Req) == TRUE);
    CHECK(fixture.unit.selection_token == 9u);
    CHECK(!fixture.unit.link_committed && fixture.unit.link_opener == NULL);
    {
        size_t index;
        const UBYTE *bytes = (const UBYTE *)&fixture.unit.link_info;
        for (index = 0; index < sizeof(fixture.unit.link_info); ++index)
            CHECK(bytes[index] == 0u);
    }
    CHECK(close_request.ios2_Req.io_Unit == NULL);
    CHECK(close_request.ios2_BufferManagement == NULL);
    CHECK(allocation_calls == 1u && free_calls == 1u);
    CHECK(live_allocations() == 0u);
    CHECK(minlist_count(&fixture.unit.openers) == 1);
    CHECK(minlist_contains(&fixture.unit.openers,
                           &fixture.opener.unit_node));
}

static void test_event_completion_retains_owner_until_finish(void)
{
    struct fixture fixture;
    struct IOSana2Req match, miss;
    struct p4wlan_request *match_item;
    struct p4wlan_request *miss_item;
    struct MinList done;

    reset_observations();
    fixture_init(&fixture);
    request_init(&match, &fixture);
    request_init(&miss, &fixture);
    match_item = enqueue_request(&fixture, &match, P4WLAN_REQUEST_EVENT);
    miss_item = enqueue_request(&fixture, &miss, P4WLAN_REQUEST_EVENT);
    match_item->event_mask = 0x0cu;
    miss_item->event_mask = 0x10u;
    minlist_init(&done);

    ObtainSemaphore(&fixture.unit.lock);
    p4wlan_event_locked(&fixture.unit, 0x04u, &done);
    ReleaseSemaphore(&fixture.unit.lock);
    CHECK(match_item->state == P4WLAN_REQUEST_COMPLETING);
    CHECK(match_item->completion_wire == 0x04u);
    CHECK(miss_item->state == P4WLAN_REQUEST_QUEUED);
    CHECK(minlist_count(&done) == 1);
    CHECK(find_request(&fixture.opener, &match) == match_item);
    CHECK(minlist_contains(&fixture.opener.requests, &match_item->owner_node));
    CHECK(free_calls == 0u);
    CHECK(reply_calls == 0u);

    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 1u);
    CHECK(replied[0] == &match);
    CHECK(match.ios2_WireError == 0x04u);
    CHECK(find_request(&fixture.opener, &match) == NULL);
    CHECK(find_request(&fixture.opener, &miss) == miss_item);
    CHECK(free_calls == 1u);
    CHECK(fixture.opener.request_count == 1u);
    CHECK(fixture.opener.event_count == 1u);
    CHECK(fixture.opener.refs == 2u);

    minlist_init(&done);
    ObtainSemaphore(&fixture.unit.lock);
    p4wlan_flush_opener_locked(&fixture.unit, &fixture.opener, &done,
                               IOERR_ABORTED, S2WERR_GENERIC_ERROR, TRUE);
    ReleaseSemaphore(&fixture.unit.lock);
    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 2u);
    CHECK(replied[1] == &miss);
    check_fixture_empty(&fixture);
}

static void test_flush_respects_states_and_event_policy(void)
{
    struct fixture fixture;
    struct p4wlan_opener second;
    struct IOSana2Req queued_read, queued_write, queued_event;
    struct IOSana2Req active_read, completing_read, second_read;
    struct p4wlan_request *queued_write_item;
    struct p4wlan_request *active_item;
    struct p4wlan_request *completing_item;
    struct MinList done, already_done, all_done;

    reset_observations();
    fixture_init(&fixture);
    memset(&second, 0, sizeof(second));
    minlist_init(&second.requests);
    second.refs = 1u;
    minlist_addtail(&fixture.unit.openers, &second.unit_node);

    request_init(&queued_read, &fixture);
    request_init(&queued_write, &fixture);
    request_init(&queued_event, &fixture);
    request_init(&active_read, &fixture);
    request_init(&completing_read, &fixture);
    queued_event.ios2_BufferManagement = &fixture.opener;
    active_read.ios2_BufferManagement = &fixture.opener;
    completing_read.ios2_BufferManagement = &fixture.opener;

    (void)enqueue_request(&fixture, &queued_read, P4WLAN_REQUEST_READ);
    queued_write_item =
        enqueue_request(&fixture, &queued_write, P4WLAN_REQUEST_WRITE);
    (void)enqueue_request(&fixture, &queued_event, P4WLAN_REQUEST_EVENT);
    active_item = enqueue_request(&fixture, &active_read, P4WLAN_REQUEST_READ);
    completing_item = enqueue_request(&fixture, &completing_read,
                                       P4WLAN_REQUEST_READ);
    request_init(&second_read, &fixture);
    second_read.ios2_BufferManagement = &second;
    {
        struct p4wlan_request *item =
            p4wlan_alloc_request(&second, &second_read, P4WLAN_REQUEST_READ);
        CHECK(item != NULL);
        ObtainSemaphore(&fixture.unit.lock);
        p4wlan_enqueue_locked(&fixture.unit, &second, item);
        ReleaseSemaphore(&fixture.unit.lock);
    }
    active_item->state = P4WLAN_REQUEST_ACTIVE;

    minlist_init(&done);
    minlist_init(&already_done);
    minlist_init(&all_done);
    p4wlan_add_done_locked(&already_done, completing_item, 0, 0x80u);
    CHECK(completing_item->state == P4WLAN_REQUEST_COMPLETING);

    ObtainSemaphore(&fixture.unit.lock);
    p4wlan_flush_opener_locked(&fixture.unit, &fixture.opener, &done,
                               IOERR_ABORTED, S2WERR_GENERIC_ERROR, FALSE);
    ReleaseSemaphore(&fixture.unit.lock);
    CHECK(minlist_count(&done) == 2);
    CHECK(!minlist_contains(&fixture.unit.write_queue,
                            &queued_write_item->write_node));
    CHECK(find_request(&fixture.opener, &queued_event)->state ==
          P4WLAN_REQUEST_QUEUED);
    CHECK(active_item->cancelled == TRUE);
    CHECK(active_item->state == P4WLAN_REQUEST_ACTIVE);
    CHECK(completing_item->state == P4WLAN_REQUEST_COMPLETING);
    CHECK(minlist_count(&already_done) == 1);
    CHECK(reply_calls == 0u);
    CHECK(free_calls == 0u);

    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 2u);
    CHECK(replied[0] == &queued_read || replied[1] == &queued_read);
    CHECK(replied[0] == &queued_write || replied[1] == &queued_write);
    p4wlan_finish_done(&fixture.unit, &already_done);
    CHECK(reply_calls == 3u);
    CHECK(replied[2] == &completing_read);
    CHECK(completing_read.ios2_WireError == 0x80u);
    CHECK(find_request(&fixture.opener, &queued_event) != NULL);
    CHECK(find_request(&fixture.opener, &active_read) == active_item);
    CHECK(find_request(&second, &second_read) != NULL);

    ObtainSemaphore(&fixture.unit.lock);
    p4wlan_flush_all_locked(&fixture.unit, &all_done, IOERR_ABORTED,
                            S2WERR_GENERIC_ERROR, TRUE);
    ReleaseSemaphore(&fixture.unit.lock);
    CHECK(minlist_count(&all_done) == 2);
    CHECK(find_request(&fixture.opener, &queued_event)->state ==
          P4WLAN_REQUEST_COMPLETING);
    CHECK(active_item->cancelled == TRUE);
    CHECK(active_item->state == P4WLAN_REQUEST_ACTIVE);
    p4wlan_finish_done(&fixture.unit, &all_done);
    CHECK(reply_calls == 5u);
    CHECK((replied[3] == &queued_event && replied[4] == &second_read) ||
          (replied[4] == &queued_event && replied[3] == &second_read));
    CHECK(find_request(&fixture.opener, &active_read) == active_item);

    minlist_init(&done);
    p4wlan_add_done_locked(&done, active_item, IOERR_ABORTED,
                           S2WERR_GENERIC_ERROR);
    p4wlan_finish_done(&fixture.unit, &done);
    CHECK(reply_calls == 6u);
    CHECK(replied[5] == &active_read);
    CHECK(free_calls == allocation_calls);
    CHECK(IsMinListEmpty(&fixture.opener.requests));
    CHECK(IsMinListEmpty(&second.requests));
    CHECK(IsMinListEmpty(&fixture.unit.write_queue));
    CHECK(fixture.opener.refs == 1u);
    CHECK(second.refs == 1u);
    CHECK(fixture.opener.request_count == 0u);
    CHECK(second.request_count == 0u);
    CHECK(fixture.unit.request_count == 0u);
    CHECK(live_allocations() == 0u);
}

int main(void)
{
    test_queued_abort_unlinks_one_write();
    test_active_abort_marks_without_free();
    test_committed_active_write_refuses_abort();
    test_completing_abort_cannot_claim_descriptor();
    test_immediate_io_reuse_has_no_stale_membership();
    test_close_waiter_signalled_after_reply();
    test_close_drains_when_signal_allocation_fails();
    test_close_retires_pending_association_and_credential();
    test_close_releases_committed_link_pin();
    test_event_completion_retains_owner_until_finish();
    test_flush_respects_states_and_event_policy();
    printf("SANA lifetime host checks: %lu\n", checks);
    return 0;
}
