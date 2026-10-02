#define _DEFAULT_SOURCE 1
#define P4_TASK_CLEANUP_TEST 1
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#endif

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct ExecBase { uint32_t unused; };
struct Node { struct Node *ln_Succ, *ln_Pred; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; };
struct MemList { struct Node ml_Node; unsigned id; };
struct ETask { void *et_RegFrame; };
struct Task
{
    uint32_t tc_State;
    uint32_t tc_Flags;
    union { struct ETask *tc_ETask; } tc_UnionETask;
    struct List tc_MemEntry;
};
#include "../../kernel/p4_task_registry.h"
#include "../../kernel/p4_task_lifetime.c"
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
#include "../../kernel/p4_semaphore_ledger.c"
#include "../p4_remove_completion.c"
#include "../p4_remove_bank.c"
#endif
#include "../../kernel/p4_task_registry.c"

#define TS_REMOVED     UINT32_C(1)
#define TS_INVALID     UINT32_C(2)
#define TS_TOMBSTONED  UINT32_C(3)
#define TS_READY       UINT32_C(4)
#define TF_ETASK       UINT32_C(0x10)
#define P4_PUBLIC_SPIN_BAD_STATE 7

static unsigned checks;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static int gate_depth;
static unsigned claim_calls, get_etask_calls, context_calls, cleanup_calls;
static unsigned free_entry_calls, free_mem_calls, fatal_calls;
static unsigned finish_calls;
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static unsigned retry_calls, try_finish_calls, cleanup_poke_calls;
static unsigned irq_depth, disable_calls, enable_calls;
static unsigned gate_acquire_calls, gate_unlock_calls;
static unsigned task_entry_calls;
#endif
static struct p4_task_registry_record *inflight_record;
static struct p4_task_registry_record *expected_free_record;
static struct p4_task_registry cleanup_registry;
static struct p4_task_registry_record *claim_result;
static struct List *task_list;
static struct List *housekeeper_list;
static struct Task *task_page;
static struct ETask *etask_page;
static size_t page_size;
static int task_poisoned, etask_poisoned;
static unsigned free_ids[16];
static unsigned event_count;
enum event_kind { EV_CONTEXT = 1, EV_CLEANUP, EV_ENTRY_BASE = 10, EV_RECORD = 20, EV_FINISH = 21 };
static unsigned events[64];
static jmp_buf fatal_env;
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static struct p4_remove_bank fusion_bank;
static struct p4_remove_bank_slot fusion_slots[4];
static struct p4_remove_handle fusion_ticket;
static struct p4_remove_handle fusion_tickets[4];
static struct p4_task_registry_record *fusion_ticket_records[4];
static unsigned fusion_ticket_count;
static struct p4_task_registry_record fusion_requester_record;
static struct Task fusion_requester;
static int fusion_armed;
static int fusion_pressure;
static int fusion_reserve_ticket;
static int postfree_waiting;
static int cleanup_wake_pending;
static struct p4_task_registry_record retry_records[2];
static struct p4_task_registry_record *retry_old_record;
static struct p4_task_registry_record *retry_new_record;
#define CLEANUP_RECORD_COUNT (fusion_armed ? 2u : 1u)
#else
#define CLEANUP_RECORD_COUNT 1u
#endif

static void callback_outside_gate(void)
{
    CHECK(gate_depth == 0);
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    CHECK(irq_depth == 0u);
#endif
}

#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static void mock_registry_gate_enter(void)
{
    CHECK(gate_depth == 0);
    CHECK(irq_depth == 0u);
    ++disable_calls;
    ++irq_depth;
    ++gate_acquire_calls;
    ++gate_depth;
}

static void mock_registry_gate_leave(void)
{
    CHECK(gate_depth == 1);
    CHECK(irq_depth == 1u);
    --gate_depth;
    ++gate_unlock_calls;
    --irq_depth;
    ++enable_calls;
}
#endif

#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static int registry_contains_record(
    const struct p4_task_registry_record *wanted)
{
    const struct p4_task_registry_record *record = cleanup_registry.head;
    uint32_t visited = 0u;
    while (record != NULL && visited < cleanup_registry.count &&
           visited < P4_TASK_REGISTRY_LIMIT)
    {
        if (record == wanted)
            return 1;
        record = record->next;
        ++visited;
    }
    return record != NULL || visited != cleanup_registry.count ? -1 : 0;
}
#endif

#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static int fusion_ticket_for_record(
    const struct p4_task_registry_record *record,
    struct p4_remove_handle *out_handle)
{
    unsigned index;
    for (index = 0u; index < fusion_ticket_count; ++index)
    {
        if (fusion_ticket_records[index] == record)
        {
            *out_handle = fusion_tickets[index];
            return 1;
        }
    }
    return 0;
}
#endif
#endif

static void mock_newlist(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)list;
}

static struct Node *mock_remhead(struct List *list)
{
    struct Node *node = list->lh_Head;
    if (node->ln_Succ == NULL)
        return NULL;
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
    return node;
}

static void mock_addtail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
    if (list != task_list)
        housekeeper_list = list;
}

#define NEWLIST(list) mock_newlist((list))
#define RemHead(list) mock_remhead((list))
#define AddTail(list, node) mock_addtail((list), (node))

#if !defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static struct p4_task_registry_record *
krnP4TaskRecordClaimNextRetainedService(struct ExecBase *base)
{
    (void)base;
    ++claim_calls;
    ++gate_depth;
    struct p4_task_registry_record *result = claim_result;
    claim_result = NULL;
    if (result != NULL && result->identity != NULL)
    {
        struct Task *identity = result->identity;
        memset(result, 0, sizeof(*result));
        CHECK(p4_task_registry_register_locked(&cleanup_registry, result, identity) == 1);
        CHECK(p4_task_registry_retire_for_service_locked(&cleanup_registry, identity) == 1);
        CHECK(p4_task_registry_take_service_ready_locked(&cleanup_registry, identity) == identity);
        CHECK(p4_task_registry_claim_next_retained_service_locked(&cleanup_registry, &result) == 1);
        CHECK(cleanup_registry.count == 1 && result->service_inflight == 1);
    }
    inflight_record = result;
    --gate_depth;
    return result;
}

static void krnP4TaskRecordFinishRetainedService(
    struct p4_task_registry_record *record, struct ExecBase *base)
{
    (void)base;
    callback_outside_gate();
    CHECK(record != NULL && record == inflight_record);
    CHECK(free_entry_calls == 3 || free_entry_calls == 0);
    if (task_page != NULL)
        CHECK(task_poisoned && etask_poisoned);
    CHECK(finish_calls == 0);
    ++gate_depth;
    CHECK(cleanup_registry.count == CLEANUP_RECORD_COUNT && record->service_inflight == 1);
    CHECK(p4_task_registry_finish_retained_service_locked(&cleanup_registry, record) == 1);
    CHECK(cleanup_registry.count == 0 && record->service_inflight == 0);
    --gate_depth;
    ++finish_calls;
    inflight_record = NULL;
    expected_free_record = record;
    events[event_count++] = EV_FINISH;
}
#else
static struct p4_task_registry_record *
krnP4TaskRecordClaimNextRetainedService(struct ExecBase *base)
{
    struct p4_task_registry_record *result;
    struct p4_task_registry_record *claimed = NULL;
    struct Task *identity;
    (void)base;
    ++claim_calls;
    mock_registry_gate_enter();
    result = claim_result;
    claim_result = NULL;
    if (result != NULL && result->identity != NULL)
    {
        identity = result->identity;
        memset(result, 0, sizeof(*result));
        CHECK(p4_task_registry_register_locked(&cleanup_registry, result, identity) == 1);
        CHECK(p4_task_registry_retire_for_service_locked(&cleanup_registry, identity) == 1);
        CHECK(p4_task_registry_take_service_ready_locked(&cleanup_registry, identity) == identity);
        CHECK(p4_task_registry_claim_next_retained_service_locked(
            &cleanup_registry, &claimed) == 1 && claimed == result);
        if (fusion_armed)
        {
            if (!registry_contains_record(&fusion_requester_record))
                CHECK(p4_task_registry_register_locked(&cleanup_registry,
                    &fusion_requester_record, &fusion_requester) == 1);
            if (fusion_reserve_ticket)
            {
                struct p4_remove_handle handle;
                CHECK(fusion_ticket_count < 4u);
                if (fusion_pressure)
                    fusion_requester_record.lifetime.notification_pins = UINT32_MAX;
                CHECK(p4_remove_bank_reserve(&fusion_bank,
                    (uintptr_t)&fusion_requester, (uintptr_t)identity,
                    &handle) == 1);
                fusion_tickets[fusion_ticket_count] = handle;
                fusion_ticket_records[fusion_ticket_count] = result;
                if (fusion_ticket_count == 0u)
                    fusion_ticket = handle;
                ++fusion_ticket_count;
            }
        }
    }
    else if (result != NULL)
    {
        /* Preserve the consumer's fail-closed null-identity fixture. Real
         * registry claims cannot publish a null identity. */
        claimed = result;
    }
    inflight_record = claimed;
    task_entry_calls = 0u;
    mock_registry_gate_leave();
    return claimed;
}

static void krnP4CleanupPoke(struct ExecBase *base)
{
    (void)base;
    callback_outside_gate();
    ++cleanup_poke_calls;
}

static int krnP4TaskRecordTryFinishPostFree(
    struct p4_task_registry_record *record, struct ExecBase *base)
{
    int result;
    int poke = 0;
    (void)base;
    ++try_finish_calls;
    CHECK(record != NULL && record == inflight_record);
    callback_outside_gate();
    mock_registry_gate_enter();
    if (fusion_armed)
        result = p4_task_registry_finish_removal_locked(
            &cleanup_registry, &fusion_bank, record);
    else
        result = p4_task_registry_finish_retained_service_locked(
            &cleanup_registry, record);
    CHECK(result >= 0);
    if (result == 0)
    {
        CHECK(p4_task_registry_defer_removal_finish_locked(
            &cleanup_registry, record) == 1);
        postfree_waiting = 1;
        inflight_record = NULL;
    }
    else
    {
        ++finish_calls;
        expected_free_record = record;
        inflight_record = NULL;
        events[event_count++] = EV_FINISH;
        if (result > 1)
        {
            cleanup_wake_pending = 1;
            poke = 1;
        }
    }
    mock_registry_gate_leave();
    if (poke)
        krnP4CleanupPoke(base);
    return result > 0;
}

static struct p4_task_registry_record *krnP4TaskRecordRetryPostFree(
    struct ExecBase *base)
{
    struct p4_task_registry_record *record = NULL;
    struct p4_task_registry_record *finished = NULL;
    uintptr_t after = 0u;
    unsigned attempts = 0u;
    int selected, result, poke = 0;
    (void)base;
    ++retry_calls;
    mock_registry_gate_enter();
    while (attempts < 8u)
    {
        selected = p4_task_registry_next_postfree_after_locked(
            &cleanup_registry, after, &record);
        CHECK(selected >= 0);
        if (selected == 0)
            break;
        after = (uintptr_t)record;
        ++attempts;
        result = p4_task_registry_finish_removal_locked(
            &cleanup_registry, &fusion_bank, record);
        CHECK(result >= 0);
        if (result > 0)
        {
            finished = record;
            ++finish_calls;
            expected_free_record = record;
            events[event_count++] = EV_FINISH;
            if (result > 1)
            {
                cleanup_wake_pending = 1;
                poke = 1;
            }
            postfree_waiting = 0;
            break;
        }
        postfree_waiting = 1;
    }
    mock_registry_gate_leave();
    if (poke)
        krnP4CleanupPoke(base);
    return finished;
}
#endif

static struct ETask *GetETask(struct Task *task)
{
    callback_outside_gate();
    ++get_etask_calls;
    if ((task->tc_Flags & TF_ETASK) == 0)
        return NULL;
    return task->tc_UnionETask.tc_ETask;
}

static void KrnDeleteContext(void *context)
{
    callback_outside_gate();
    CHECK(context != NULL);
    ++context_calls;
    events[event_count++] = EV_CONTEXT;
}

static void CleanupETask(struct Task *task)
{
    callback_outside_gate();
    CHECK(task != NULL);
    ++cleanup_calls;
    events[event_count++] = EV_CLEANUP;
    if (etask_page != NULL && !etask_poisoned)
    {
        CHECK(task->tc_UnionETask.tc_ETask == etask_page);
        CHECK(etask_page->et_RegFrame == NULL);
        CHECK(mprotect(etask_page, page_size, PROT_NONE) == 0);
        etask_poisoned = 1;
    }
}

static void FreeEntry(struct MemList *entry)
{
    callback_outside_gate();
    /* Separate diagnostic gate probe, not a gate held by the consumer across
     * this callback. The Task may already be PROT_NONE; identity stays opaque. */
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    mock_registry_gate_enter();
    CHECK(registry_contains_record(inflight_record) == 1);
    CHECK(inflight_record != NULL && inflight_record->service_inflight == 1);
    CHECK(p4_task_registry_pin_locked(&cleanup_registry,
        inflight_record->identity) == 0);
    {
        struct p4_remove_handle handle;
        if (fusion_ticket_for_record(inflight_record, &handle))
        {
            int ready = -1;
            CHECK(p4_remove_bank_ready(&fusion_bank, handle,
                (uintptr_t)&fusion_requester, &ready) == 1 && ready == 0);
        }
    }
    mock_registry_gate_leave();
#else
    ++gate_depth;
    CHECK(cleanup_registry.count == CLEANUP_RECORD_COUNT && inflight_record != NULL);
    CHECK(inflight_record->service_inflight == 1);
    CHECK(p4_task_registry_pin_locked(&cleanup_registry, inflight_record->identity) == 0);
    --gate_depth;
#endif
    CHECK(entry != NULL);
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    CHECK(task_entry_calls < 3u);
    free_ids[free_entry_calls] = entry->id;
#else
    CHECK(free_entry_calls < 3);
    free_ids[free_entry_calls] = entry->id;
#endif
    events[event_count++] = EV_ENTRY_BASE + entry->id;

#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    if (task_entry_calls == 0u && task_page != NULL && !task_poisoned)
#else
    if (free_entry_calls == 0 && task_page != NULL && !task_poisoned)
#endif
    {
        CHECK(task_page->tc_MemEntry.lh_Head ==
              (struct Node *)&task_page->tc_MemEntry.lh_Tail);
        CHECK(task_page->tc_MemEntry.lh_TailPred ==
              (struct Node *)&task_page->tc_MemEntry);
        CHECK(task_page->tc_UnionETask.tc_ETask == NULL);
        CHECK((task_page->tc_Flags & TF_ETASK) == 0);
        CHECK(housekeeper_list != NULL);
        CHECK(entry->ml_Node.ln_Succ !=
              (struct Node *)&task_page->tc_MemEntry.lh_Tail);
        CHECK(entry->ml_Node.ln_Pred != (struct Node *)&task_page->tc_MemEntry);

        struct Node *node = housekeeper_list->lh_Head;
        unsigned count = 0;
        while (node->ln_Succ != NULL)
        {
            CHECK(count < 2);
            CHECK(node->ln_Succ != (struct Node *)&task_page->tc_MemEntry.lh_Tail);
            CHECK(node->ln_Pred != (struct Node *)&task_page->tc_MemEntry);
            node = node->ln_Succ;
            ++count;
        }
        CHECK(count == 2);
        CHECK(mprotect(task_page, page_size, PROT_NONE) == 0);
        task_poisoned = 1;
    }
    else if (task_page != NULL)
    {
        CHECK(task_poisoned);
    }
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    ++task_entry_calls;
#endif
    ++free_entry_calls;
}

static void FreeMem(void *memory, size_t size)
{
    callback_outside_gate();
    CHECK(memory != NULL);
    CHECK(size == sizeof(struct p4_task_registry_record));
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    CHECK(memory == expected_free_record && expected_free_record != NULL);
    CHECK(finish_calls == free_mem_calls + 1u);
    CHECK((free_entry_calls % 3u) == 0u);
    mock_registry_gate_enter();
    CHECK(registry_contains_record(memory) == 0);
    {
        struct p4_remove_handle handle;
        if (fusion_ticket_for_record(memory, &handle))
        {
            int ready = -1;
            CHECK(p4_remove_bank_ready(&fusion_bank, handle,
                (uintptr_t)&fusion_requester, &ready) == 1 && ready == 1);
        }
    }
    mock_registry_gate_leave();
    expected_free_record = NULL;
#else
    CHECK(finish_calls == 1 && inflight_record == NULL);
    CHECK(free_entry_calls == 3 || free_entry_calls == 0);
#endif
    if (task_page != NULL)
        CHECK(task_poisoned);
    if (etask_page != NULL)
        CHECK(etask_poisoned);
    ++free_mem_calls;
    events[event_count++] = EV_RECORD;
}

static _Noreturn void krnP4PublicSpinFatal(int reason)
{
    callback_outside_gate();
    CHECK(reason == P4_PUBLIC_SPIN_BAD_STATE);
    ++fatal_calls;
    longjmp(fatal_env, 1);
}

#include "../p4_task_cleanup.c"

static void reset_observation(void)
{
    gate_depth = 0;
    claim_calls = 0;
    finish_calls = 0;
    inflight_record = NULL;
    expected_free_record = NULL;
    p4_task_registry_init(&cleanup_registry);
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    retry_calls = 0;
    try_finish_calls = 0;
    cleanup_poke_calls = 0;
    irq_depth = 0;
    disable_calls = 0;
    enable_calls = 0;
    gate_acquire_calls = 0;
    gate_unlock_calls = 0;
    task_entry_calls = 0;
    postfree_waiting = 0;
    cleanup_wake_pending = 0;
    fusion_pressure = 0;
    fusion_reserve_ticket = 1;
    fusion_ticket_count = 0;
    memset(fusion_tickets, 0, sizeof(fusion_tickets));
    memset(fusion_ticket_records, 0, sizeof(fusion_ticket_records));
    memset(retry_records, 0, sizeof(retry_records));
    retry_old_record = NULL;
    retry_new_record = NULL;
    memset(&fusion_bank, 0, sizeof(fusion_bank));
    memset(fusion_slots, 0, sizeof(fusion_slots));
    memset(&fusion_requester_record, 0, sizeof(fusion_requester_record));
    memset(&fusion_requester, 0, sizeof(fusion_requester));
    fusion_armed = 0;
    CHECK(p4_remove_bank_init(&fusion_bank, fusion_slots, 4u) == 1);
#endif
    get_etask_calls = 0;
    context_calls = 0;
    cleanup_calls = 0;
    free_entry_calls = 0;
    free_mem_calls = 0;
    fatal_calls = 0;
    claim_result = NULL;
    task_list = NULL;
    housekeeper_list = NULL;
    task_page = NULL;
    etask_page = NULL;
    task_poisoned = 0;
    etask_poisoned = 0;
    event_count = 0;
    memset(free_ids, 0, sizeof(free_ids));
    memset(events, 0, sizeof(events));
}

static void *new_page(void)
{
    void *page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    return page;
}

static void test_empty_claim_does_not_touch_task(void)
{
    struct ExecBase base = {0};
    reset_observation();
    CHECK(krnP4CleanupOneRetiredTask(&base) == 0);
    CHECK(claim_calls == 1);
    CHECK(get_etask_calls == 0);
    CHECK(context_calls == 0);
    CHECK(cleanup_calls == 0);
    CHECK(free_entry_calls == 0);
    CHECK(free_mem_calls == 0);
    CHECK(fatal_calls == 0);
    CHECK(gate_depth == 0);
}

static void expect_fatal_for(struct Task *task)
{
    struct ExecBase base = {0};
    struct p4_task_registry_record record = { .identity = task };
    claim_result = &record;
    if (setjmp(fatal_env) == 0)
    {
        (void)krnP4CleanupOneRetiredTask(&base);
        CHECK(0 && "expected fail-closed fatal");
    }
    CHECK(fatal_calls == 1);
    CHECK(get_etask_calls == 0);
    CHECK(context_calls == 0);
    CHECK(cleanup_calls == 0);
    CHECK(free_entry_calls == 0);
    CHECK(free_mem_calls == 0);
    CHECK(gate_depth == 0);
}

static void test_null_identity_and_nonterminal_state_refuse(void)
{
    struct Task task = {0};
    reset_observation();
    expect_fatal_for(NULL);

    reset_observation();
    task.tc_State = TS_READY;
    expect_fatal_for(&task);
    CHECK(task.tc_State == TS_READY);
}

static void test_null_etask_and_null_frame(void)
{
    struct ExecBase base = {0};
    struct Task task = {0};
    struct p4_task_registry_record record = { .identity = &task };
    reset_observation();
    task.tc_State = TS_REMOVED;
    mock_newlist(&task.tc_MemEntry);
    task_list = &task.tc_MemEntry;
    claim_result = &record;
    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(get_etask_calls == 1);
    CHECK(context_calls == 0);
    CHECK(cleanup_calls == 1);
    CHECK(free_entry_calls == 0);
    CHECK(free_mem_calls == 1);
    CHECK(task.tc_UnionETask.tc_ETask == NULL);
    CHECK((task.tc_Flags & TF_ETASK) == 0);
    CHECK(events[0] == EV_CLEANUP);
    CHECK(events[1] == EV_FINISH);
    CHECK(events[2] == EV_RECORD);

    struct ETask et = { NULL };
    task.tc_State = TS_INVALID;
    task.tc_Flags = TF_ETASK;
    task.tc_UnionETask.tc_ETask = &et;
    record.identity = &task;
    reset_observation();
    mock_newlist(&task.tc_MemEntry);
    task_list = &task.tc_MemEntry;
    claim_result = &record;
    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(context_calls == 0);
    CHECK(cleanup_calls == 1);
    CHECK(free_mem_calls == 1);
}

static void test_context_metadata_then_detach_before_free(void)
{
    struct ExecBase base = {0};
    struct p4_task_registry_record record = {0};
    struct MemList entries[3] = { { .id = 1 }, { .id = 2 }, { .id = 3 } };

    reset_observation();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    fusion_armed = 1;
#endif
    CHECK(page_size >= sizeof(struct Task));
    task_page = (struct Task *)new_page();
    etask_page = (struct ETask *)new_page();
    struct Task *task = task_page;
    task->tc_State = TS_TOMBSTONED;
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = etask_page;
    etask_page->et_RegFrame = (void *)(uintptr_t)0x1234;
    mock_newlist(&task->tc_MemEntry);
    task_list = &task->tc_MemEntry;
    for (unsigned i = 0; i < 3; ++i)
        mock_addtail(task_list, &entries[i].ml_Node);
    record.identity = task;
    claim_result = &record;

    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(claim_calls == 1);
    CHECK(context_calls == 1);
    CHECK(cleanup_calls == 1);
    CHECK(free_entry_calls == 3);
    CHECK(free_mem_calls == 1);
    CHECK(free_ids[0] == 1 && free_ids[1] == 2 && free_ids[2] == 3);
    CHECK(events[0] == EV_CONTEXT);
    CHECK(events[1] == EV_CLEANUP);
    CHECK(events[2] == EV_ENTRY_BASE + 1);
    CHECK(events[3] == EV_ENTRY_BASE + 2);
    CHECK(events[4] == EV_ENTRY_BASE + 3);
    CHECK(events[5] == EV_FINISH);
    CHECK(events[6] == EV_RECORD);
    CHECK(task_poisoned && etask_poisoned);
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    {
        struct p4_semaphore_notification notification = {0};
        int reclaim = -1;
        mock_registry_gate_enter();
        CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
            &cleanup_registry, &notification) == 1);
        CHECK(notification.identity == &fusion_requester &&
            notification.count == 1u && notification.generation == 1u);
        CHECK(p4_task_registry_finish_semaphore_notification_locked(
            &cleanup_registry, &notification) == 1);
        CHECK(fusion_requester_record.lifetime.notification_pins == 0u);
        CHECK(p4_remove_bank_drop(&fusion_bank, fusion_ticket,
            (uintptr_t)&fusion_requester, 0, &reclaim) == 1 && reclaim == 1);
        mock_registry_gate_leave();
    }
#endif
    CHECK(munmap(task_page, page_size) == 0);
    CHECK(munmap(etask_page, page_size) == 0);
    task_page = NULL;
    etask_page = NULL;
}

#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
static void prepare_poisonable_task(
    struct Task **task_out, struct ETask **etask_out,
    struct MemList entries[3], unsigned first_id)
{
    struct Task *task = (struct Task *)new_page();
    struct ETask *etask = (struct ETask *)new_page();
    task->tc_State = TS_TOMBSTONED;
    task->tc_Flags = TF_ETASK;
    task->tc_UnionETask.tc_ETask = etask;
    etask->et_RegFrame = (void *)(uintptr_t)(0x1234u + first_id);
    mock_newlist(&task->tc_MemEntry);
    task_page = task;
    etask_page = etask;
    task_list = &task->tc_MemEntry;
    task_poisoned = 0;
    etask_poisoned = 0;
    for (unsigned index = 0u; index < 3u; ++index)
    {
        entries[index].id = first_id + index;
        mock_addtail(task_list, &entries[index].ml_Node);
    }
    *task_out = task;
    *etask_out = etask;
}

static void test_postfree_pressure_retries_without_blocking_new_cleanup(void)
{
    struct ExecBase base = {0};
    struct Task *old_task, *new_task;
    struct ETask *old_etask, *new_etask;
    struct MemList old_entries[3] = {{0}}, new_entries[3] = {{0}};
    struct p4_remove_handle handle;
    struct p4_semaphore_notification notification = {0};
    int ready = -1;
    int reclaim = -1;

    reset_observation();
    fusion_armed = 1;
    fusion_pressure = 1;
    fusion_reserve_ticket = 1;
    prepare_poisonable_task(&old_task, &old_etask, old_entries, 1u);
    retry_records[0].identity = old_task;
    retry_old_record = &retry_records[0];
    claim_result = retry_old_record;

    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(claim_calls == 1u && retry_calls == 1u && try_finish_calls == 1u);
    CHECK(context_calls == 1u && cleanup_calls == 1u && free_entry_calls == 3u);
    CHECK(finish_calls == 0u && free_mem_calls == 0u);
    CHECK(task_poisoned && etask_poisoned && postfree_waiting == 1);
    mock_registry_gate_enter();
    CHECK(registry_contains_record(retry_old_record) == 1);
    CHECK(retry_old_record->service_inflight == 1u &&
        retry_old_record->service_postfree == 1u);
    mock_registry_gate_leave();
    CHECK(fusion_ticket_count == 1u && fusion_ticket_for_record(
        retry_old_record, &handle));
    mock_registry_gate_enter();
    CHECK(p4_remove_bank_ready(&fusion_bank, handle,
        (uintptr_t)&fusion_requester, &ready) == 1 && ready == 0);
    mock_registry_gate_leave();
    CHECK(fusion_requester_record.lifetime.notification_pins == UINT32_MAX);
    CHECK(cleanup_poke_calls == 0u && gate_depth == 0 && irq_depth == 0u);
    CHECK(disable_calls == enable_calls && gate_acquire_calls == gate_unlock_calls);

    /* Repeated pressure-only service is quiet and does not release the
     * separately allocated retained record or reread the poisoned Task. */
    CHECK(krnP4CleanupOneRetiredTask(&base) == 0);
    CHECK(retry_calls == 2u && claim_calls == 2u && try_finish_calls == 1u);
    CHECK(context_calls == 1u && cleanup_calls == 1u && free_entry_calls == 3u);
    CHECK(finish_calls == 0u && free_mem_calls == 0u && cleanup_poke_calls == 0u);
    mock_registry_gate_enter();
    CHECK(registry_contains_record(retry_old_record) == 1 &&
        retry_old_record->service_postfree == 1u);
    mock_registry_gate_leave();

    /* A distinct newly ready task still receives cleanup while the earlier
     * post-free completion remains blocked. Its own ticketless fused finish
     * may complete and free only its own registry record. */
    prepare_poisonable_task(&new_task, &new_etask, new_entries, 4u);
    retry_records[1].identity = new_task;
    retry_new_record = &retry_records[1];
    fusion_reserve_ticket = 0;
    claim_result = retry_new_record;
    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(retry_calls == 3u && claim_calls == 3u && try_finish_calls == 2u);
    CHECK(context_calls == 2u && cleanup_calls == 2u && free_entry_calls == 6u);
    CHECK(finish_calls == 1u && free_mem_calls == 1u);
    mock_registry_gate_enter();
    CHECK(registry_contains_record(retry_old_record) == 1 &&
        retry_old_record->service_postfree == 1u);
    CHECK(registry_contains_record(retry_new_record) == 0);
    mock_registry_gate_leave();
    CHECK(old_task != new_task && old_etask != new_etask);
    CHECK(task_poisoned && etask_poisoned && cleanup_poke_calls == 0u);
    CHECK(fusion_requester_record.lifetime.notification_pins == UINT32_MAX);

    /* Relinquish exactly enough ordinary pin headroom. A later actual retry
     * fuses the old ACK and detach; only then may FreeMem receive that record. */
    mock_registry_gate_enter();
    CHECK(p4_task_registry_unpin_locked(&cleanup_registry,
        &fusion_requester) == 1);
    mock_registry_gate_leave();
    CHECK(fusion_requester_record.lifetime.notification_pins == UINT32_MAX - 1u);
    CHECK(krnP4CleanupOneRetiredTask(&base) == 1);
    CHECK(retry_calls == 4u && claim_calls == 3u && try_finish_calls == 2u);
    CHECK(context_calls == 2u && cleanup_calls == 2u && free_entry_calls == 6u);
    CHECK(finish_calls == 2u && free_mem_calls == 2u);
    mock_registry_gate_enter();
    CHECK(registry_contains_record(retry_old_record) == 0);
    CHECK(registry_contains_record(retry_new_record) == 0);
    mock_registry_gate_leave();
    mock_registry_gate_enter();
    CHECK(p4_remove_bank_ready(&fusion_bank, handle,
        (uintptr_t)&fusion_requester, &ready) == 1 && ready == 1);
    mock_registry_gate_leave();
    CHECK(cleanup_poke_calls == 1u && cleanup_wake_pending == 1);
    CHECK(gate_depth == 0 && irq_depth == 0u &&
        disable_calls == enable_calls && gate_acquire_calls == gate_unlock_calls);
    CHECK(free_ids[0] == 1u && free_ids[1] == 2u && free_ids[2] == 3u);
    CHECK(free_ids[3] == 4u && free_ids[4] == 5u && free_ids[5] == 6u);

    /* Consume the durable notification and requester ticket with real leaves,
     * leaving no fixture-owned bank slot active. */
    mock_registry_gate_enter();
    CHECK(p4_task_registry_claim_next_semaphore_notification_locked(
        &cleanup_registry, &notification) == 1);
    CHECK(notification.identity == &fusion_requester && notification.count == 1u);
    CHECK(p4_task_registry_finish_semaphore_notification_locked(
        &cleanup_registry, &notification) == 1);
    CHECK(p4_remove_bank_drop(&fusion_bank, handle,
        (uintptr_t)&fusion_requester, 0, &reclaim) == 1 && reclaim == 1);
    mock_registry_gate_leave();

    CHECK(munmap(old_task, page_size) == 0);
    CHECK(munmap(old_etask, page_size) == 0);
    CHECK(munmap(new_task, page_size) == 0);
    CHECK(munmap(new_etask, page_size) == 0);
    task_page = NULL;
    etask_page = NULL;
}
#endif

int main(void)
{
    test_empty_claim_does_not_touch_task();
    test_null_identity_and_nonterminal_state_refuse();
    test_null_etask_and_null_frame();
    test_context_metadata_then_detach_before_free();
#if defined(P4_REMOVE_CLEANUP_FUSION_TEST)
    test_postfree_pressure_retries_without_blocking_new_cleanup();
#endif
    printf("p4-task-cleanup-test: %u checks passed\n", checks);
    return 0;
}
