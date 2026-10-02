/* Host fixture for the actual P4 semaphore cancellation worker body. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Task { uint32_t opaque; };
struct ExecBase { uint32_t opaque; };
struct SignalSemaphore { uint32_t opaque; };

#include "../../kernel/p4_semaphore_cancel.h"
#include "../../kernel/p4_semaphore_cancel_retry.h"

void krnP4SemaphoreCancelRetryBegin(struct Task *, struct ExecBase *);
void krnP4SemaphoreCancelRetryEnd(struct Task *, uint32_t, struct ExecBase *);
int krnP4SemaphoreCancelClaimAfter(struct Task *, uintptr_t,
    struct p4_task_wait_cancel_snapshot *, struct ExecBase *);
int krnP4SemaphoreCancelDescribe(struct Task *, struct Task *, uint32_t,
    uint32_t, struct p4_semaphore_cancel_entry_snapshot *, struct ExecBase *);
void Exec_P4SemaphoreLock(struct SignalSemaphore *, struct ExecBase *);
void Exec_P4SemaphoreUnlock(struct SignalSemaphore *, struct ExecBase *);
int krnP4SemaphoreCancelWithdraw(struct Task *, struct Task *, uint32_t,
    uint32_t, struct SignalSemaphore *, struct ExecBase *);
int krnP4SemaphoreCancelRollbackDurable(struct Task *, struct Task *,
    uint32_t, uint32_t, struct SignalSemaphore *, struct ExecBase *);
int krnP4SemaphoreOperationFinishCancel(struct Task *, uint32_t,
    struct ExecBase *);
int krnP4SemaphoreCancelReleaseClaim(struct Task *, struct Task *, uint32_t,
    uint32_t, struct ExecBase *);
void krnP4CleanupPoke(struct ExecBase *);
void p4_signal_adapter_fail_closed(void) __attribute__((noreturn));

#define P4_SEMAPHORE_CANCEL_WORKER_TEST 1
#include "../p4_semaphore_cancel_worker.c"

#define MAX_SCOPES 8u
#define MAX_ENTRIES 8u
#define MAX_EVENTS 1024u

static unsigned checks;
static jmp_buf fatal_jump;
static int fatal_jump_armed;
static struct Task worker_task;
static struct Task target_tasks[MAX_SCOPES];
static struct ExecBase sys_base;
static struct SignalSemaphore semaphores[4];

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

#define MUST(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "MOCK INVARIANT line %d: %s\n", __LINE__, #condition); \
        abort(); \
    } \
} while (0)

enum Event
{
    EVENT_RETRY_BEGIN = 1,
    EVENT_CLAIM,
    EVENT_DESCRIBE,
    EVENT_SEM_LOCK,
    EVENT_WITHDRAW,
    EVENT_ROLLBACK,
    EVENT_SEM_UNLOCK,
    EVENT_CLEANUP_POKE,
    EVENT_FINISH,
    EVENT_RELEASE,
    EVENT_RETRY_END
};

struct MockEntry
{
    struct SignalSemaphore *semaphore;
    uint32_t state;
    uint32_t committed;
    uint32_t running_harts;
    int mutation_result;
};

struct MockScope
{
    struct Task *identity;
    uint32_t generation;
    uint32_t kind;
    uint32_t queued_count;
    uint32_t construction_closed;
    uint32_t cancel_requested;
    const void *semaphore_index;
    const void *request_index;
    const void *list_index;
    uint32_t entry_count;
    struct MockEntry entries[MAX_ENTRIES];
    int available;
    int claimed;
    int finished;
    int finish_result;
    int release_result;
    uint32_t expected_observed_running;
    uint32_t current_running;
    int drain_run_after_initial_describe;
    unsigned entry0_describes;
};

struct Mock
{
    struct MockScope scopes[MAX_SCOPES];
    unsigned scope_count;
    struct MockScope *current;
    unsigned claim_calls;
    unsigned positive_claims;
    struct Task *claimed_identities[MAX_SCOPES];
    uintptr_t cursors[MAX_SCOPES + 2u];
    unsigned describe_calls;
    unsigned withdraw_calls;
    unsigned rollback_calls;
    unsigned lock_calls;
    unsigned unlock_calls;
    unsigned cleanup_pokes;
    unsigned finish_calls;
    unsigned release_calls;
    unsigned retry_begin_calls;
    unsigned retry_end_calls;
    uint32_t retry_end_blocked;
    uint32_t release_observed_running;
    unsigned semaphore_depth;
    unsigned registry_depth;
    unsigned event_count;
    enum Event events[MAX_EVENTS];
    int claim_override;
    int describe_override;
    int force_duplicate_after_first;
    int force_return_nonoperation;
    int force_entry_count_mismatch;
    int finish_result_override;
    int release_result_override;
    int mutation_result_override;
    uint32_t fatal_tag;
    unsigned fail_closed_calls;
};

static struct Mock mock;

static void event(enum Event value)
{
    MUST(mock.event_count < MAX_EVENTS);
    mock.events[mock.event_count++] = value;
}

static void gate_enter(void)
{
    MUST(mock.registry_depth == 0u);
    mock.registry_depth = 1u;
}

static void gate_leave(void)
{
    MUST(mock.registry_depth == 1u);
    mock.registry_depth = 0u;
}

static void mock_reset(void)
{
    unsigned i;
    memset(&mock, 0, sizeof(mock));
    mock.claim_override = INT32_MIN;
    mock.describe_override = INT32_MIN;
    mock.finish_result_override = INT32_MIN;
    mock.release_result_override = INT32_MIN;
    mock.mutation_result_override = INT32_MIN;
    for (i = 0; i < MAX_SCOPES; ++i)
        target_tasks[i].opaque = i + 1u;
    worker_task.opaque = 100u;
}

static struct MockScope *add_scope(unsigned task_index, uint32_t entry_count)
{
    struct MockScope *scope;
    unsigned i;

    MUST(mock.scope_count < MAX_SCOPES);
    MUST(task_index < MAX_SCOPES);
    MUST(entry_count <= MAX_ENTRIES);
    scope = &mock.scopes[mock.scope_count++];
    memset(scope, 0, sizeof(*scope));
    scope->identity = &target_tasks[task_index];
    scope->generation = 100u + task_index;
    scope->kind = P4_TASK_WAIT_OPERATION;
    scope->queued_count = 0;
    scope->construction_closed = 1;
    scope->cancel_requested = 1;
    scope->entry_count = entry_count;
    scope->available = 1;
    scope->finish_result = 1;
    scope->release_result = 1;
    for (i = 0; i < entry_count; ++i)
    {
        scope->entries[i].state = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
        scope->entries[i].mutation_result = 1;
    }
    return scope;
}

static void set_entry(struct MockScope *scope, unsigned ordinal,
    struct SignalSemaphore *semaphore, uint32_t state,
    uint32_t committed, uint32_t running_harts)
{
    MUST(scope != NULL && ordinal < scope->entry_count);
    scope->entries[ordinal].semaphore = semaphore;
    scope->entries[ordinal].state = state;
    scope->entries[ordinal].committed = committed;
    scope->entries[ordinal].running_harts = running_harts;
    scope->current_running = running_harts;
}

static struct MockScope *find_scope(struct Task *identity, uint32_t generation)
{
    unsigned i;
    for (i = 0; i < mock.scope_count; ++i)
        if (mock.scopes[i].identity == identity &&
            mock.scopes[i].generation == generation)
            return &mock.scopes[i];
    return NULL;
}

static void fill_scope_snapshot(struct MockScope *source,
    struct p4_task_wait_cancel_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->identity = source->identity;
    out->generation = source->generation;
    out->kind = source->kind;
    out->queued_count = source->queued_count;
    out->construction_closed = source->construction_closed;
    out->cancel_requested = source->cancel_requested;
    out->semaphore_index = source->semaphore_index;
    out->request_index = source->request_index;
    out->list_index = source->list_index;
}

void krnP4SemaphoreCancelRetryBegin(struct Task *worker,
    struct ExecBase *base)
{
    MUST(worker == &worker_task && base == &sys_base);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    gate_enter();
    ++mock.retry_begin_calls;
    event(EVENT_RETRY_BEGIN);
    gate_leave();
}

void krnP4SemaphoreCancelRetryEnd(struct Task *worker, uint32_t blocked,
    struct ExecBase *base)
{
    MUST(worker == &worker_task && base == &sys_base);
    MUST(blocked <= 1u);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    gate_enter();
    ++mock.retry_end_calls;
    mock.retry_end_blocked = blocked;
    event(EVENT_RETRY_END);
    gate_leave();
}

int krnP4SemaphoreCancelClaimAfter(struct Task *worker, uintptr_t cursor,
    struct p4_task_wait_cancel_snapshot *out, struct ExecBase *base)
{
    struct MockScope *selected = NULL;
    unsigned i;

    MUST(worker == &worker_task && base == &sys_base && out != NULL);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    gate_enter();
    ++mock.claim_calls;
    MUST(mock.claim_calls <= MAX_SCOPES + 1u);
    mock.cursors[mock.claim_calls - 1u] = cursor;
    event(EVENT_CLAIM);

    if (mock.claim_override != INT32_MIN)
    {
        mock.fatal_tag = 1u;
        gate_leave();
        return mock.claim_override;
    }
    if (mock.force_duplicate_after_first && mock.positive_claims == 1u)
    {
        selected = &mock.scopes[0];
        fill_scope_snapshot(selected, out);
        ++mock.positive_claims;
        mock.claimed_identities[mock.positive_claims - 1u] = selected->identity;
        mock.current = selected;
        mock.fatal_tag = 1u;
        gate_leave();
        return 1;
    }
    for (i = 0; i < mock.scope_count; ++i)
    {
        struct MockScope *candidate = &mock.scopes[i];
        if (!candidate->available || candidate->claimed || candidate->finished ||
            (uintptr_t)candidate->identity <= cursor ||
            (candidate->kind != P4_TASK_WAIT_OPERATION &&
             !mock.force_return_nonoperation))
            continue;
        if (selected == NULL ||
            (uintptr_t)candidate->identity < (uintptr_t)selected->identity)
            selected = candidate;
    }
    if (selected == NULL)
    {
        memset(out, 0, sizeof(*out));
        gate_leave();
        return 0;
    }
    selected->claimed = 1;
    fill_scope_snapshot(selected, out);
    mock.current = selected;
    ++mock.positive_claims;
    MUST(mock.positive_claims <= MAX_SCOPES);
    mock.claimed_identities[mock.positive_claims - 1u] = selected->identity;
    mock.fatal_tag = 1u;
    gate_leave();
    return 1;
}

int krnP4SemaphoreCancelDescribe(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal,
    struct p4_semaphore_cancel_entry_snapshot *out, struct ExecBase *base)
{
    struct MockScope *scope;
    struct MockEntry *entry;
    MUST(worker == &worker_task && base == &sys_base && out != NULL);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    gate_enter();
    ++mock.describe_calls;
    event(EVENT_DESCRIBE);
    scope = find_scope(target, generation);
    MUST(scope != NULL && scope->claimed);
    mock.fatal_tag = 2u;
    if (mock.describe_override != INT32_MIN)
    {
        gate_leave();
        return mock.describe_override;
    }
    if (ordinal >= scope->entry_count)
    {
        gate_leave();
        return 0;
    }
    entry = &scope->entries[ordinal];
    memset(out, 0, sizeof(*out));
    out->semaphore = entry->semaphore;
    out->entry_count = scope->entry_count;
    out->state = entry->state;
    out->committed = entry->committed;
    out->running_harts = entry->running_harts;
    if (mock.force_entry_count_mismatch && ordinal == 0u)
    {
        ++scope->entry0_describes;
        if (scope->entry0_describes >= 2u)
            ++out->entry_count;
    }
    if (ordinal == 0u && scope->drain_run_after_initial_describe &&
        scope->entry0_describes++ == 0u)
    {
        unsigned i;
        for (i = 0; i < scope->entry_count; ++i)
            scope->entries[i].running_harts = 0;
        scope->current_running = 0;
    }
    gate_leave();
    return 1;
}

void Exec_P4SemaphoreLock(struct SignalSemaphore *semaphore,
    struct ExecBase *base)
{
    MUST(semaphore != NULL && base == &sys_base);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    ++mock.semaphore_depth;
    ++mock.lock_calls;
    event(EVENT_SEM_LOCK);
}

void Exec_P4SemaphoreUnlock(struct SignalSemaphore *semaphore,
    struct ExecBase *base)
{
    MUST(semaphore != NULL && base == &sys_base);
    MUST(mock.semaphore_depth == 1u && mock.registry_depth == 0u);
    --mock.semaphore_depth;
    ++mock.unlock_calls;
    event(EVENT_SEM_UNLOCK);
}

static int mutate_entry(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *semaphore,
    uint32_t wanted_state, enum Event which,
    struct ExecBase *base)
{
    struct MockScope *scope;
    struct MockEntry *entry;
    int result;

    MUST(worker == &worker_task && base == &sys_base &&
        mock.semaphore_depth == 1u && mock.registry_depth == 0u);
    gate_enter();
    event(which);
    scope = find_scope(target, generation);
    MUST(scope != NULL && scope->claimed && ordinal < scope->entry_count);
    entry = &scope->entries[ordinal];
    MUST(entry->semaphore == semaphore && entry->state == wanted_state);
    result = mock.mutation_result_override != INT32_MIN ?
        mock.mutation_result_override : entry->mutation_result;
    mock.fatal_tag = 5u;
    if (result > 0)
        entry->state = which == EVENT_WITHDRAW ?
            P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN :
            P4_SEMAPHORE_LEDGER_ENTRY_RELEASED;
    gate_leave();
    return result;
}

int krnP4SemaphoreCancelWithdraw(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *semaphore,
    struct ExecBase *base)
{
    ++mock.withdraw_calls;
    return mutate_entry(worker, target, generation, ordinal, semaphore,
        P4_SEMAPHORE_LEDGER_ENTRY_QUEUED, EVENT_WITHDRAW, base);
}

int krnP4SemaphoreCancelRollbackDurable(struct Task *worker,
    struct Task *target, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *semaphore, struct ExecBase *base)
{
    ++mock.rollback_calls;
    return mutate_entry(worker, target, generation, ordinal, semaphore,
        P4_SEMAPHORE_LEDGER_ENTRY_OWNED, EVENT_ROLLBACK, base);
}

int krnP4SemaphoreOperationFinishCancel(struct Task *target,
    uint32_t generation, struct ExecBase *base)
{
    struct MockScope *scope;
    unsigned i;
    int result;

    MUST(base == &sys_base && mock.semaphore_depth == 0u &&
        mock.registry_depth == 0u);
    gate_enter();
    ++mock.finish_calls;
    event(EVENT_FINISH);
    scope = find_scope(target, generation);
    MUST(scope != NULL && scope->claimed);
    mock.fatal_tag = 3u;
    result = mock.finish_result_override != INT32_MIN ?
        mock.finish_result_override : scope->finish_result;
    if (result == 1)
    {
        for (i = 0; i < scope->entry_count; ++i)
            if (scope->entries[i].committed == 0u)
                MUST(scope->entries[i].state != P4_SEMAPHORE_LEDGER_ENTRY_QUEUED &&
                    scope->entries[i].state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
        scope->finished = 1;
        scope->claimed = 0;
    }
    gate_leave();
    return result;
}

int krnP4SemaphoreCancelReleaseClaim(struct Task *worker,
    struct Task *target, uint32_t generation,
    uint32_t observed_running_harts, struct ExecBase *base)
{
    struct MockScope *scope;
    int result;

    MUST(worker == &worker_task && base == &sys_base &&
        mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    gate_enter();
    ++mock.release_calls;
    event(EVENT_RELEASE);
    scope = find_scope(target, generation);
    MUST(scope != NULL && scope->claimed);
    mock.release_observed_running = observed_running_harts;
    MUST(observed_running_harts == scope->expected_observed_running);
    mock.fatal_tag = 4u;
    result = mock.release_result_override != INT32_MIN ?
        mock.release_result_override : scope->release_result;
    if (result == 1)
        scope->claimed = 0;
    gate_leave();
    return result;
}

void krnP4CleanupPoke(struct ExecBase *base)
{
    MUST(base == &sys_base);
    MUST(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    ++mock.cleanup_pokes;
    event(EVENT_CLEANUP_POKE);
}

void p4_signal_adapter_fail_closed(void)
{
    unsigned tag = mock.fatal_tag;
    ++mock.fail_closed_calls;
    if (fatal_jump_armed)
        longjmp(fatal_jump, tag == 0u ? 99 : (int)tag);
    abort();
}

static int event_index(enum Event value, unsigned start, unsigned *out)
{
    unsigned i;
    for (i = start; i < mock.event_count; ++i)
        if (mock.events[i] == value)
        {
            *out = i;
            return 1;
        }
    return 0;
}

static int test_no_work_and_legacy_scope_skip(void)
{
    struct MockScope *legacy;
    int result;

    mock_reset();
    legacy = add_scope(0, 1);
    legacy->kind = P4_TASK_WAIT_SINGLE;
    legacy->available = 1;
    result = Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
    CHECK(result == 0);
    CHECK(mock.retry_begin_calls == 1u && mock.retry_end_calls == 1u);
    CHECK(mock.retry_end_blocked == 0u);
    CHECK(mock.claim_calls == 1u && mock.positive_claims == 0u);
    CHECK(mock.finish_calls == 0u && mock.release_calls == 0u);
    CHECK(mock.cleanup_pokes == 0u && mock.lock_calls == 0u);
    CHECK(mock.event_count >= 3u);
    CHECK(mock.events[0] == EVENT_RETRY_BEGIN);
    CHECK(mock.events[1] == EVENT_CLAIM);
    CHECK(mock.events[mock.event_count - 1u] == EVENT_RETRY_END);
    return 1;
}

static int test_ordered_scopes_states_and_unlock_before_poke(void)
{
    struct MockScope *first, *second, *legacy;
    unsigned withdraw, unlock1, poke1, rollback, unlock2, poke2;
    int result;

    mock_reset();
    first = add_scope(0, 6);
    first->queued_count = 1;
    set_entry(first, 0, NULL, P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED, 0, 0);
    set_entry(first, 1, NULL, P4_SEMAPHORE_LEDGER_ENTRY_RELEASED, 0, 0);
    set_entry(first, 2, NULL, P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN, 0, 0);
    set_entry(first, 3, &semaphores[0], P4_SEMAPHORE_LEDGER_ENTRY_QUEUED, 0, 0);
    set_entry(first, 4, &semaphores[1], P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 0, 0);
    set_entry(first, 5, &semaphores[1], P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 1, 0);
    second = add_scope(1, 2);
    set_entry(second, 0, NULL, P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED, 0, 0);
    set_entry(second, 1, &semaphores[2], P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 1, 0);
    legacy = add_scope(2, 1);
    legacy->kind = P4_TASK_WAIT_LIST;

    result = Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
    CHECK(result == 1);
    CHECK(mock.positive_claims == 2u);
    CHECK(mock.claimed_identities[0] == &target_tasks[0]);
    CHECK(mock.claimed_identities[1] == &target_tasks[1]);
    CHECK(mock.cursors[0] == 0u);
    CHECK(mock.cursors[1] == (uintptr_t)&target_tasks[0]);
    CHECK(mock.cursors[2] == (uintptr_t)&target_tasks[1]);
    CHECK(mock.withdraw_calls == 1u && mock.rollback_calls == 1u);
    CHECK(first->entries[3].state == P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN);
    CHECK(first->entries[4].state == P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(first->entries[5].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(second->entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED);
    CHECK(mock.finish_calls == 2u && mock.release_calls == 0u);
    CHECK(mock.lock_calls == 2u && mock.unlock_calls == 2u);
    CHECK(mock.cleanup_pokes == 2u && mock.retry_end_blocked == 0u);
    CHECK(mock.semaphore_depth == 0u && mock.registry_depth == 0u);

    CHECK(event_index(EVENT_WITHDRAW, 0, &withdraw));
    CHECK(event_index(EVENT_SEM_UNLOCK, withdraw, &unlock1));
    CHECK(event_index(EVENT_CLEANUP_POKE, unlock1, &poke1));
    CHECK(withdraw < unlock1 && unlock1 < poke1);
    CHECK(event_index(EVENT_ROLLBACK, poke1 + 1u, &rollback));
    CHECK(event_index(EVENT_SEM_UNLOCK, rollback, &unlock2));
    CHECK(event_index(EVENT_CLEANUP_POKE, unlock2, &poke2));
    CHECK(rollback < unlock2 && unlock2 < poke2);
    CHECK(mock.events[0] == EVENT_RETRY_BEGIN);
    CHECK(mock.events[mock.event_count - 1u] == EVENT_RETRY_END);
    return 1;
}

static int test_run_held_owner_quietly_releases_claim(void)
{
    struct MockScope *scope;
    int result;

    mock_reset();
    scope = add_scope(0, 2);
    scope->finish_result = 0;
    scope->expected_observed_running = 1;
    scope->entries[0].state = P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED;
    scope->entries[0].running_harts = 1;
    scope->entries[1].semaphore = &semaphores[0];
    scope->entries[1].state = P4_SEMAPHORE_LEDGER_ENTRY_OWNED;
    scope->entries[1].running_harts = 1;

    result = Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
    CHECK(result == 0);
    CHECK(mock.lock_calls == 0u && mock.cleanup_pokes == 0u);
    CHECK(mock.finish_calls == 1u && mock.release_calls == 1u);
    CHECK(mock.release_observed_running == 1u);
    CHECK(mock.retry_end_blocked == 1u);
    CHECK(scope->claimed == 0);
    return 1;
}

static int test_observed_run_snapshot_survives_run_drain(void)
{
    struct MockScope *scope;
    int result;

    mock_reset();
    scope = add_scope(0, 2);
    scope->finish_result = 0;
    scope->expected_observed_running = 1;
    scope->drain_run_after_initial_describe = 1;
    set_entry(scope, 0, NULL, P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED, 0, 1);
    set_entry(scope, 1, &semaphores[0], P4_SEMAPHORE_LEDGER_ENTRY_OWNED, 0, 1);

    result = Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
    CHECK(result == 1);
    CHECK(scope->entries[1].state == P4_SEMAPHORE_LEDGER_ENTRY_RELEASED);
    CHECK(mock.release_calls == 1u);
    CHECK(mock.release_observed_running == 1u);
    CHECK(mock.retry_end_blocked == 1u);
    CHECK(mock.cleanup_pokes == 1u);
    CHECK(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    return 1;
}

static int test_resource_refusal_does_not_self_poke(void)
{
    struct MockScope *scope;
    int result;

    mock_reset();
    scope = add_scope(0, 1);
    scope->finish_result = 0;
    scope->entries[0].semaphore = &semaphores[0];
    scope->entries[0].state = P4_SEMAPHORE_LEDGER_ENTRY_QUEUED;
    scope->entries[0].mutation_result = 0;

    result = Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
    CHECK(result == 0);
    CHECK(scope->entries[0].state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED);
    CHECK(mock.withdraw_calls == 1u && mock.lock_calls == 1u &&
        mock.unlock_calls == 1u);
    CHECK(mock.cleanup_pokes == 0u);
    CHECK(mock.finish_calls == 1u && mock.release_calls == 1u);
    CHECK(mock.retry_end_blocked == 1u);
    return 1;
}

enum FatalCase
{
    FATAL_CLAIM_RESULT,
    FATAL_DESCRIBE_RESULT,
    FATAL_SCOPE_GENERATION,
    FATAL_SCOPE_KIND,
    FATAL_SCOPE_ORDER,
    FATAL_BORROWED_INDEX,
    FATAL_ENTRY_COUNT,
    FATAL_ENTRY_STATE,
    FATAL_MUTATION_RESULT,
    FATAL_FINISH_RESULT,
    FATAL_RELEASE_RESULT
};

static int test_fatal_case(enum FatalCase which)
{
    struct MockScope *scope;
    int jump_value;

    mock_reset();
    scope = add_scope(0, 1);
    scope->entries[0].semaphore = &semaphores[0];
    scope->entries[0].state = P4_SEMAPHORE_LEDGER_ENTRY_QUEUED;
    if (which == FATAL_CLAIM_RESULT)
        mock.claim_override = -1;
    else if (which == FATAL_DESCRIBE_RESULT)
        mock.describe_override = -1;
    else if (which == FATAL_SCOPE_GENERATION)
        scope->generation = 0;
    else if (which == FATAL_SCOPE_KIND)
    {
        scope->kind = P4_TASK_WAIT_SINGLE;
        mock.force_return_nonoperation = 1;
    }
    else if (which == FATAL_SCOPE_ORDER)
        mock.force_duplicate_after_first = 1;
    else if (which == FATAL_BORROWED_INDEX)
        scope->list_index = &mock;
    else if (which == FATAL_ENTRY_COUNT)
        mock.force_entry_count_mismatch = 1;
    else if (which == FATAL_ENTRY_STATE)
        scope->entries[0].state = UINT32_MAX;
    else if (which == FATAL_MUTATION_RESULT)
        mock.mutation_result_override = -1;
    else if (which == FATAL_FINISH_RESULT)
        mock.finish_result_override = -1;
    else if (which == FATAL_RELEASE_RESULT)
    {
        scope->finish_result = 0;
        mock.release_result_override = -1;
    }

    fatal_jump_armed = 1;
    jump_value = setjmp(fatal_jump);
    if (jump_value == 0)
    {
        (void)Exec_P4SemaphoreCancellationPass(&worker_task, &sys_base);
        fatal_jump_armed = 0;
        CHECK(0 && "expected fail-closed path");
    }
    fatal_jump_armed = 0;
    CHECK(mock.fail_closed_calls == 1u);
    CHECK((uint32_t)jump_value == mock.fatal_tag);
    CHECK(mock.semaphore_depth == 0u && mock.registry_depth == 0u);
    return 1;
}

static int test_fatal_validation_paths(void)
{
    CHECK(test_fatal_case(FATAL_CLAIM_RESULT));
    CHECK(test_fatal_case(FATAL_DESCRIBE_RESULT));
    CHECK(test_fatal_case(FATAL_SCOPE_GENERATION));
    CHECK(test_fatal_case(FATAL_SCOPE_KIND));
    CHECK(test_fatal_case(FATAL_SCOPE_ORDER));
    CHECK(test_fatal_case(FATAL_BORROWED_INDEX));
    CHECK(test_fatal_case(FATAL_ENTRY_COUNT));
    CHECK(test_fatal_case(FATAL_ENTRY_STATE));
    CHECK(test_fatal_case(FATAL_MUTATION_RESULT));
    CHECK(test_fatal_case(FATAL_FINISH_RESULT));
    CHECK(test_fatal_case(FATAL_RELEASE_RESULT));
    return 1;
}

int main(void)
{
    if (!test_no_work_and_legacy_scope_skip() ||
        !test_ordered_scopes_states_and_unlock_before_poke() ||
        !test_run_held_owner_quietly_releases_claim() ||
        !test_observed_run_snapshot_survives_run_drain() ||
        !test_resource_refusal_does_not_self_poke() ||
        !test_fatal_validation_paths())
        return 1;
    printf("p4 semaphore cancellation worker checks: %u\n", checks);
    return 0;
}
