/* Kernel-side binding of raw Task identity lookup to one lifetime gate.
 * Selected only by the gated SMP kernel; full lifecycle is not yet wired. */
#if !defined(P4_TASK_PINS_TEST)
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <proto/exec.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 Task pins require the matched SMP ABI"
#endif
#endif
#include <stdbool.h>
#include "p4_public_spinlock.h"
#include "p4_ipi.h"
#include "p4_task_pins.h"
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
#include "p4_semaphore_boot.h"
#include "p4_semaphore_nodes.h"
#include "p4_semaphore_cancel.h"
#include "p4_semaphore_cancel_retry.h"
#include "../exec/p4_remove_bank.h"
#endif

#define P4_TASK_PIN_GATE_ATTEMPTS 20000u
static spinlock_t p4_task_pin_gate;
static struct p4_task_registry p4_task_records;
static int p4_task_pins_prepared;
static struct Task *p4_cleanup_worker;
static uint32_t p4_cleanup_wake_mask;
static bool p4_cleanup_wake_pending;
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
static struct p4_semaphore_cancel_retry p4_cancel_retry;
static struct p4_remove_bank *p4_removal_bank;
static bool p4_removal_finish_waiting;
static uintptr_t p4_postfree_after;
static bool p4_postfree_blocked;
static bool p4_postfree_restart;
#define P4_POSTFREE_ATTEMPT_BUDGET 8u
#endif

int krnP4TaskPinsPrepare(void)
{
    if (p4_task_pins_prepared)
        return 0;
    if (krnP4PublicSpinInit(&p4_task_pin_gate) != P4_PUBLIC_SPIN_OK)
        return 0;
    p4_task_registry_init(&p4_task_records);
    p4_cleanup_worker = 0;
    p4_cleanup_wake_mask = 0;
    p4_cleanup_wake_pending = false;
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    p4_cancel_retry.active = 0;
    p4_cancel_retry.waiting = 0;
    p4_removal_bank = NULL;
    p4_removal_finish_waiting = false;
    p4_postfree_after = 0u;
    p4_postfree_blocked = false;
    p4_postfree_restart = false;
#endif
    p4_task_pins_prepared = 1;
    return 1;
}

static void p4_task_pins_lock(struct ExecBase *SysBase)
{
    unsigned int attempt;
    (void)SysBase;
    Disable();
    if (!p4_task_pins_prepared)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_NOT_PREPARED);
    for (attempt = 0; attempt < P4_TASK_PIN_GATE_ATTEMPTS; ++attempt)
    {
        enum p4_public_spinlock_result result =
            krnP4PublicSpinTryAcquire(&p4_task_pin_gate, SPINLOCK_MODE_WRITE);
        if (result == P4_PUBLIC_SPIN_OK)
            return;
        if (result != P4_PUBLIC_SPIN_BUSY && result != P4_PUBLIC_SPIN_TRY_TIMEOUT)
            krnP4PublicSpinFatal(result);
    }
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_SERVICE_TIMEOUT);
}

static void p4_task_pins_unlock(struct ExecBase *SysBase)
{
    enum p4_public_spinlock_result result;
    (void)SysBase;
    result = krnP4PublicSpinUnlock(&p4_task_pin_gate);
    if (result != P4_PUBLIC_SPIN_OK)
        krnP4PublicSpinFatal(result);
    Enable();
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
int krnP4TaskPinsAttachRemovalBank(struct p4_remove_bank *bank)
{
    uint32_t count;
    int result = 0;

    /* Bootstrap-only publication: serial zero plus validated slots proves
     * that no ticket has ever been issued. The registry is still empty.
     * Subsequent bank operations use this exact lifetime gate. */
    /* No Exec vectors exist yet: like Prepare, this unpublished bootstrap
     * hook must not call Disable/Enable or acquire the runtime gate. The
     * startup owner holds raw MIE off and hart1 reset/clock-held. */
    if (p4_task_pins_prepared && p4_removal_bank == NULL &&
        p4_task_records.count == 0u && p4_task_records.head == NULL &&
        p4_remove_bank_count_target(bank, (uintptr_t)1, &count) == 1 &&
        bank->serial == 0u)
    {
        p4_removal_bank = bank;
        result = 1;
    }
    return result;
}

static int p4_task_pins_cancel_removal_interest_locked(struct Task *task)
{
    uint32_t dropped = 0u;

    /* Retirement has been published, but neither its wake nor the gate has
     * been released. No cleanup can race this requester-wide cancellation.
     * It drops ticket references only, never the target's service ownership. */
    if (p4_removal_bank != NULL &&
        p4_remove_bank_cancel_requester(p4_removal_bank,
            (uintptr_t)task, &dropped) != 1)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (dropped != 0u && p4_removal_finish_waiting)
    {
        if (p4_postfree_after != 0u)
            p4_postfree_restart = true;
        p4_cleanup_wake_pending = true;
        return 1;
    }
    return 0;
}

int krnP4TaskRecordAdmitRemoval(struct Task *requester, struct Task *target,
    uint32_t hart, uint32_t retire_live, struct p4_remove_handle *handle,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_admit_removal_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), p4_removal_bank, requester, target,
        hart, retire_live, handle);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 0)
    {
        /* New retirement already cancelled requester references atomically.
         * That can release capacity for another postfree owner. Never call
         * the independent bank cancellation mutator after this transaction.
         * The caller still owns its Task/queue gates: only durable work here,
         * no Signal/IPI/poke. Ordinary target Unpin later queues readiness. */
        if (result == 1 && p4_removal_finish_waiting && p4_postfree_after != 0u)
            p4_postfree_restart = true;
        p4_cleanup_wake_pending = true;
    }
    p4_task_pins_unlock(SysBase);
    return result;
}
#endif

int krnP4TaskRecordPublish(struct Task *task,
    struct p4_task_registry_record *record, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_register_locked(&p4_task_records, record, task);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4SignalTaskPin(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_pin_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4ProtectCleanupWorker(struct Task *task, struct ExecBase *SysBase)
{
    int result = 0;
    p4_task_pins_lock(SysBase);
    if (p4_cleanup_worker == NULL &&
        p4_task_registry_protect_worker_locked(&p4_task_records, task))
    {
        p4_cleanup_worker = task;
        result = 1;
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4ArmCleanupWake(struct Task *task, uint32_t mask,
                        struct ExecBase *SysBase)
{
    int result = 0;
    int re_poke = 0;
    int owner;

    p4_task_pins_lock(SysBase);
    if (task != 0 && task == p4_cleanup_worker && mask != 0 &&
        p4_cleanup_wake_mask == 0 &&
        p4_task_registry_is_worker_locked(&p4_task_records, task) == 1)
    {
        owner = p4_task_registry_running_owner_locked(&p4_task_records, task);
        if (owner > 0)
        {
            p4_cleanup_wake_mask = mask;
            result = 1;
            re_poke = p4_cleanup_wake_pending;
        }
    }
    p4_task_pins_unlock(SysBase);

    /* A ready event may have been durably recorded before the worker armed. */
    if (re_poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4CleanupWorkerReady(struct ExecBase *SysBase)
{
    int result;

    p4_task_pins_lock(SysBase);
    result = p4_cleanup_worker != 0 && p4_cleanup_wake_mask != 0 &&
             p4_task_registry_is_worker_locked(
                 &p4_task_records, p4_cleanup_worker) == 1;
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TakeCleanupWake(struct p4_cleanup_wake *out,
                         struct ExecBase *SysBase)
{
    int result;

    if (out == 0)
        return -1;
    out->worker = 0;
    out->mask = 0;

    p4_task_pins_lock(SysBase);
    if (!p4_cleanup_wake_pending || p4_cleanup_wake_mask == 0)
        result = 0;
    else if (p4_cleanup_worker == 0 ||
             p4_task_registry_is_worker_locked(
                 &p4_task_records, p4_cleanup_worker) != 1)
        result = -1;
    else
    {
        out->worker = p4_cleanup_worker;
        out->mask = p4_cleanup_wake_mask;
        p4_cleanup_wake_pending = false;
        result = 1;
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

/* Called only while the registry gate is held. The pointer returned by the
 * registry is compared as an opaque identity and never dereferenced here. */
static int p4_task_pins_reserve_cleanup_wake_locked(struct Task *task)
{
    int cancel_ready = p4_task_registry_wait_cancel_ready_locked(
        &p4_task_records, task);
    int service_ready;
    if (cancel_ready < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    service_ready = p4_task_registry_take_service_ready_locked(
        &p4_task_records, task) != 0;
    /* A wait-scope pin prevents service readiness. Cancellation therefore
     * owns a wake independently of ordinary pin drain, under the same gate.
     * Claimed or zero-queue RUN-held scopes do not re-poke a busy drain loop. */
    if (!service_ready && !cancel_ready)
        return 0;
    p4_cleanup_wake_pending = true;
    return 1;
}

/* Successful pin/scope drops can unblock a DIFFERENT cancellation owner.
 * Preserve both target readiness and cross-target interest under this gate;
 * neither transition may be skipped because the other already asks a poke. */
static int p4_task_pins_reserve_resource_wake_locked(struct Task *task)
{
    int poke = p4_task_pins_reserve_cleanup_wake_locked(task);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    int interested = p4_semaphore_cancel_retry_event(&p4_cancel_retry);
    if (interested < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (interested == 1 || p4_removal_finish_waiting)
    {
        if (p4_removal_finish_waiting && p4_postfree_after != 0u)
            p4_postfree_restart = true;
        p4_cleanup_wake_pending = true;
        poke = 1;
    }
#endif
    return poke;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
/* RUN remains the requester's storage reference across a normal-Task wait.
 * A retiring requester cancels its bank interests before this gate unlocks;
 * poll/drop then use only registry identity, never a stale slot/Task pointer. */
static int p4_task_pins_removal_requester_locked(
    struct Task *requester, uint32_t hart)
{
    if (hart > 1u || p4_removal_bank == NULL ||
        p4_task_registry_is_worker_locked(&p4_task_records, requester) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (p4_task_registry_running_owner_locked(&p4_task_records, requester) ==
            (int)hart + 1)
        return 1;
    if (p4_task_registry_service_owner_running_locked(
            &p4_task_records, requester, hart))
        return 2;
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    return 0; /* Only for a test fatal adapter that unexpectedly returns. */
}

int krnP4TaskRemovalPoll(struct Task *requester, uint32_t hart,
    struct p4_remove_handle handle, struct ExecBase *SysBase)
{
    int ready = 0, result;
    p4_task_pins_lock(SysBase);
    if (p4_task_pins_removal_requester_locked(requester, hart) == 2)
        result = 2;
    else
    {
        if (p4_remove_bank_ready(p4_removal_bank, handle,
                (uintptr_t)requester, &ready) != 1)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        result = ready;
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TaskRemovalDrop(struct Task *requester, uint32_t hart,
    struct p4_remove_handle handle, struct ExecBase *SysBase)
{
    int reclaim = 0, result = 0, poke = 0;
    p4_task_pins_lock(SysBase);
    if (p4_task_pins_removal_requester_locked(requester, hart) == 1)
    {
        if (p4_remove_bank_drop(p4_removal_bank, handle,
                (uintptr_t)requester, 0, &reclaim) != 1 || reclaim != 1)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        result = 1;
        poke = p4_task_pins_reserve_resource_wake_locked(requester);
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4SemaphoreReleaseDurable(struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_semaphore_nodes_release_durable_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), sem);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 1)
        p4_cleanup_wake_pending = true;
    p4_task_pins_unlock(SysBase);
    return result; /* Outer semaphore gate still belongs to the caller. */
}

int krnP4SemaphoreGrantDurable(struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_semaphore_nodes_grant_durable_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), sem);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 0)
        p4_cleanup_wake_pending = true;
    p4_task_pins_unlock(SysBase);
    return result;
}

/* Caller supplies its current raw identity while its RUN reference owns the
 * continuation. Do not infer worker readiness from Task priority/state. */
static int p4_task_pins_notification_worker_locked(struct Task *worker)
{
    return worker != NULL && worker == p4_cleanup_worker &&
        p4_cleanup_wake_mask != 0 &&
        p4_task_registry_is_worker_locked(&p4_task_records, worker) == 1 &&
        p4_task_registry_running_owner_locked(&p4_task_records, worker) > 0;
}

void krnP4SemaphoreCancelRetryBegin(struct Task *worker,
    struct ExecBase *SysBase)
{
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker) ||
        p4_semaphore_cancel_retry_begin(&p4_cancel_retry) != 1)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
}

void krnP4SemaphoreCancelRetryEnd(struct Task *worker, uint32_t blocked,
    struct ExecBase *SysBase)
{
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker) ||
        p4_semaphore_cancel_retry_end(&p4_cancel_retry, blocked) != 1)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* Never consume the separate wake latch here: a producer during the
     * active pass may already have reserved the next wake. No self-poke. */
    p4_task_pins_unlock(SysBase);
}

int krnP4SemaphoreCancelClaimAfter(struct Task *worker,
    uintptr_t after_identity, struct p4_task_wait_cancel_snapshot *snapshot,
    struct ExecBase *SysBase)
{
    struct p4_semaphore_ledger *ledger;
    struct SemaphoreRequest *pool;
    uintptr_t output = (uintptr_t)snapshot;
    uintptr_t pool_address, pool_bytes;
    int result;
    p4_task_pins_lock(SysBase);
    ledger = krnP4SemaphoreBootLedger();
    pool = krnP4SemaphoreBootRequest(0);
    if (!p4_task_pins_notification_worker_locked(worker) || snapshot == NULL ||
        !p4_task_registry_ledger_disjoint_locked(&p4_task_records, ledger) ||
        pool == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    pool_address = (uintptr_t)pool;
    pool_bytes = (uintptr_t)ledger->entry_capacity * sizeof(*pool);
    if (output > UINTPTR_MAX - sizeof(*snapshot) ||
        pool_address > UINTPTR_MAX - pool_bytes ||
        (output < pool_address + pool_bytes &&
            pool_address < output + sizeof(*snapshot)))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_task_registry_claim_wait_operation_cancel_after_with_ledger_locked(
        &p4_task_records, ledger, after_identity, snapshot);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* These legacy snapshot fields are borrowed storage indices, not needed
     * by the operation consumer. Do not let them survive gate unlock. */
    if (result == 1)
    {
        snapshot->semaphore_index = NULL;
        snapshot->request_index = NULL;
        snapshot->list_index = NULL;
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4SemaphoreCancelDescribe(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal,
    struct p4_semaphore_cancel_entry_snapshot *snapshot,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_semaphore_cancel_describe_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), target,
        generation, ordinal, snapshot);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4SemaphoreCancelReleaseClaim(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t observed_running_harts,
    struct ExecBase *SysBase)
{
    struct p4_semaphore_cancel_entry_snapshot current;
    int result, poke = 0;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker) ||
        observed_running_harts > 2u)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_semaphore_cancel_describe_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), target,
        generation, 0, &current);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result == 1)
    {
        result = p4_task_registry_wait_release_cancel_locked(&p4_task_records,
            target, generation);
        if (result != 1)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        /* RUN-leave could not advertise a still-claimed scope. Only that
         * observed transition needs a wake here. An already-RUN0 blocked
         * owner must not continually wake/reclaim itself without progress. */
        if (observed_running_harts != 0 && current.running_harts == 0)
        {
            p4_cleanup_wake_pending = true;
            poke = 1;
        }
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4SemaphoreCancelWithdraw(struct Task *worker, struct Task *target,
    uint32_t generation, uint32_t ordinal, struct SignalSemaphore *sem,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_semaphore_nodes_withdraw_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), target,
        generation, ordinal, sem);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* A withdrawn cancelled peer may unblock another scope's last-owner
     * handoff. Keep the retry obligation even before a consumer is routed. */
    if (result == 1)
        p4_cleanup_wake_pending = true;
    p4_task_pins_unlock(SysBase);
    return result; /* Caller still owns its outer semaphore gate. */
}

int krnP4SemaphoreCancelRollbackDurable(struct Task *worker,
    struct Task *target, uint32_t generation, uint32_t ordinal,
    struct SignalSemaphore *sem, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_semaphore_nodes_rollback_durable_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), target,
        generation, ordinal, sem);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 0)
        p4_cleanup_wake_pending = true;
    p4_task_pins_unlock(SysBase);
    return result; /* Poke only after the caller releases the semaphore gate. */
}

int krnP4SemaphoreNotificationClaim(struct Task *worker,
    struct p4_semaphore_notification *snapshot, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_task_registry_claim_next_semaphore_notification_locked(
        &p4_task_records, snapshot);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

void krnP4SemaphoreNotificationFinish(struct Task *worker,
    const struct p4_semaphore_notification *snapshot, struct ExecBase *SysBase)
{
    int poke;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker) ||
        p4_task_registry_finish_semaphore_notification_locked(
            &p4_task_records, snapshot) != 1)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* The snapshot lives in protected worker storage. Its raw identity is
     * used only for metadata readiness; no target Task dereference follows. */
    poke = p4_task_pins_reserve_resource_wake_locked(snapshot->identity);
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
}
#endif

int krnP4SemaphoreWaitBegin(struct Task *task, uint32_t kind,
    const void *semaphore, const void *request, const void *list,
    uint32_t *generation, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_wait_begin_locked(&p4_task_records, task,
        kind, semaphore, request, list, generation);
    p4_task_pins_unlock(SysBase);
    return result;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
int krnP4SemaphoreOperationReserve(struct Task *task, uint32_t entry_count,
    struct p4_task_wait_operation *output, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_wait_reserve_operation_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), task, entry_count, output);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4SemaphoreOperationConfigure(struct Task *task, uint32_t generation,
    uint32_t ordinal, struct SignalSemaphore *sem, uint32_t shared,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_semaphore_nodes_configure_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), task,
        generation, ordinal, sem, shared);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result; /* Caller retains the semaphore gate; never poke here. */
}

int krnP4SemaphoreOperationAcquire(struct Task *task, uint32_t generation,
    uint32_t ordinal, struct SignalSemaphore *sem, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_semaphore_nodes_acquire_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), krnP4SemaphoreBootRequest(0), task,
        generation, ordinal, sem);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result; /* Preserve queued result2, not a truthy malformed result. */
}

int krnP4SemaphoreOperationSetEntry(struct Task *task, uint32_t generation,
    uint32_t index, const void *semaphore, const void *request,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_wait_operation_set_entry_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), task, generation, index, semaphore, request);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4SemaphoreOperationClose(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_wait_operation_close_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), task, generation);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

static int p4_task_pins_operation_finish(struct Task *task, uint32_t generation,
    int cancelled, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    if (cancelled)
        result = p4_task_registry_wait_operation_finish_cancel_locked(
            &p4_task_records, krnP4SemaphoreBootLedger(), task, generation);
    else
        result = p4_task_registry_wait_operation_complete_locked(
            &p4_task_records, krnP4SemaphoreBootLedger(), task, generation);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result == 1)
        poke = p4_task_pins_reserve_resource_wake_locked(task);
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4SemaphoreOperationComplete(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    return p4_task_pins_operation_finish(task, generation, 0, SysBase);
}

int krnP4SemaphoreOperationFinishCancel(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    return p4_task_pins_operation_finish(task, generation, 1, SysBase);
}
#endif

/* These leaf calls expose no sidecar pointers to the caller. The scope and
 * any notification reference remain indexed throughout the transition. */
#define P4_WAIT_BINDING(name, helper) \
int name(struct Task *task, uint32_t generation, struct ExecBase *SysBase) \
{ \
    int result; \
    p4_task_pins_lock(SysBase); \
    result = helper(&p4_task_records, task, generation); \
    p4_task_pins_unlock(SysBase); \
    return result; \
}

P4_WAIT_BINDING(krnP4SemaphoreWaitAddRequest,
    p4_task_registry_wait_add_request_locked)
P4_WAIT_BINDING(krnP4SemaphoreWaitClose, p4_task_registry_wait_close_locked)
P4_WAIT_BINDING(krnP4SemaphoreWaitReady, p4_task_registry_wait_ready_locked)
P4_WAIT_BINDING(krnP4SemaphoreWaitGrantRequest,
    p4_task_registry_wait_grant_request_locked)
P4_WAIT_BINDING(krnP4SemaphoreWaitWithdrawRequest,
    p4_task_registry_wait_withdraw_request_locked)
#undef P4_WAIT_BINDING

int krnP4SemaphoreWaitReleaseCancel(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_wait_release_cancel_locked(
        &p4_task_records, task, generation);
    /* RUN may have drained while this exclusive claim was held. Its leave
     * could not reserve cancellation readiness then; publish it now rather
     * than requiring the consumer to rescan before sleeping. */
    if (result)
        poke = p4_task_pins_reserve_cleanup_wake_locked(task);
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

static int p4_task_pins_wait_finish(struct Task *task, uint32_t generation,
    int cancelled, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    if (cancelled)
        result = p4_task_registry_wait_finish_cancel_locked(
            &p4_task_records, task, generation);
    else
        result = p4_task_registry_wait_finish_locked(
            &p4_task_records, task, generation);
    if (result)
        poke = p4_task_pins_reserve_resource_wake_locked(task);
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4SemaphoreWaitFinish(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    return p4_task_pins_wait_finish(task, generation, 0, SysBase);
}

int krnP4SemaphoreWaitFinishCancel(struct Task *task, uint32_t generation,
    struct ExecBase *SysBase)
{
    return p4_task_pins_wait_finish(task, generation, 1, SysBase);
}

int krnP4SemaphoreWaitClaimCancel(struct p4_task_wait_cancel_snapshot *snapshot,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    result = p4_task_registry_claim_next_wait_cancel_with_ledger_locked(
        &p4_task_records, krnP4SemaphoreBootLedger(), snapshot);
#else
    result = p4_task_registry_claim_next_wait_cancel_locked(
        &p4_task_records, snapshot);
#endif
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TaskRemovalAllowed(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_is_worker_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return result == 0;
}

int krnP4TaskRecordPublishPinned(struct Task *task,
    struct p4_task_registry_record *record, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_register_locked(&p4_task_records, record, task);
    if (result && !p4_task_registry_pin_locked(&p4_task_records, task))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TaskResourceObserve(struct Task *task, struct Library **observed,
    struct ExecBase *SysBase)
{
    struct Library *resource;
    int result;
    if (task == NULL || observed == NULL || SysBase == NULL)
        return 0;
    p4_task_pins_lock(SysBase);
    resource = __atomic_load_n(&SysBase->lb_TaskResBase, __ATOMIC_ACQUIRE);
    result = p4_task_registry_taskresource_observer_locked(&p4_task_records,
        task, resource == NULL);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result == 1)
        *observed = resource;
    p4_task_pins_unlock(SysBase);
    return result == 1;
}

/* Caller holds the lifetime gate. This conservative policy is intentionally
 * unchanged until the complete observer-debt inventory is integrated. */
static int p4_taskresource_bootstrap_quiescent_locked(void)
{
    struct p4_task_registry_record *record;
    int result;
    result = p4_task_registry_validate_locked(&p4_task_records);
    if (result)
    {
        for (record = p4_task_records.head; record != NULL; record = record->next)
        {
            uint64_t durable_pins = (uint64_t)record->worker_protected +
                record->wait_scope.active + record->semaphore_pending +
                record->semaphore_inflight;
            /* No Task identity is dereferenced. Every observer-NULL
             * constructor still owns its temporary ordinary pin, from
             * RecordPublishPinned through TaskLaunch and its final log.
             * Designated construction/channel scopes also refuse cutover.
             * Refusing other ordinary pins is deliberate bootstrap safety,
             * not proof that a transient refusal can recover at coldstart. */
            if (record->lifetime.flags != 0u ||
                record->construction_creator != NULL ||
                record->construction_children != 0u ||
                record->creation_frame != 0u ||
                record->lifetime.notification_pins != durable_pins)
            {
                result = 0;
                break;
            }
        }
    }
    return result;
}

int krnP4TaskResourceBootstrapQuiescent(struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_taskresource_bootstrap_quiescent_locked();
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TaskResourceBootstrapApply(int (*transaction)(void *), void *context,
    struct ExecBase *SysBase)
{
    int result = 0;
    if (transaction == NULL || context == NULL || SysBase == NULL)
        return 0;
    p4_task_pins_lock(SysBase);
    if (p4_taskresource_bootstrap_quiescent_locked())
        result = transaction(context);
    if (result < 0 || result > 2)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
#if !defined(P4_TASK_PINS_TEST)
/* Gate held, local IRQ masked: re-read actual hart after every continuation,
 * including after a hook/Wait that may have migrated the constructor. */
static struct p4_exec_runtime *p4_creation_current_locked(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    if (runtime == NULL || runtime->cpu_local == NULL ||
        runtime->hart_id >= P4_EXEC_RUNTIME_HART_COUNT ||
        runtime->cpu_local->trap_depth != 0u ||
        runtime->cpu_local->current_task == NULL ||
        p4_task_registry_running_owner_locked(&p4_task_records,
            runtime->cpu_local->current_task) != (int)(runtime->hart_id + 1u))
        return NULL;
    return runtime;
}

static int p4_creation_stack_frame(struct Task *owner,
    struct p4_creation_channel *frame)
{
    uintptr_t address = (uintptr_t)frame;
    uintptr_t lower = (uintptr_t)owner->tc_SPLower;
    uintptr_t upper = (uintptr_t)owner->tc_SPUpper;
    /* Owner is live sole RUN under the registry gate before any Task read. */
    return frame != NULL &&
        address % _Alignof(struct p4_creation_channel) == 0u &&
        lower < upper && address >= lower && address <= upper &&
        sizeof(*frame) <= upper - address;
}

int krnP4CreationChannelBegin(struct Task *target, struct p4_creation_channel *frame,
    struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime;
    int result = 0;
    p4_task_pins_lock(SysBase);
    runtime = p4_creation_current_locked(SysBase);
    if (runtime != NULL)
    {
        if (!p4_creation_stack_frame(runtime->cpu_local->current_task, frame))
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        result = p4_task_registry_creation_begin_locked(&p4_task_records,
            runtime->cpu_local->current_task, runtime->hart_id, target, frame);
        if (result < 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4CreationChannelEnd(uint32_t expected_generation,
    struct p4_creation_channel *frame,
    struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime;
    int result = 0;
    p4_task_pins_lock(SysBase);
    runtime = p4_creation_current_locked(SysBase);
    if (runtime != NULL)
    {
        if (!p4_creation_stack_frame(runtime->cpu_local->current_task, frame))
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        result = p4_task_registry_creation_end_locked(&p4_task_records,
            runtime->cpu_local->current_task, runtime->hart_id,
            expected_generation, frame);
        if (result < 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    }
    p4_task_pins_unlock(SysBase);
    return result;
}

uint32_t *krnP4CreationChannelResolve(struct Task *target, uint32_t generation,
    uintptr_t tag_value,
    struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime;
    uintptr_t address = 0u;
    p4_task_pins_lock(SysBase);
    runtime = p4_creation_current_locked(SysBase);
    if (runtime != NULL &&
        p4_creation_stack_frame(runtime->cpu_local->current_task,
            (struct p4_creation_channel *)tag_value))
        address = p4_task_registry_creation_resolve_locked(&p4_task_records,
            runtime->cpu_local->current_task, runtime->hart_id, target,
            generation, tag_value);
    p4_task_pins_unlock(SysBase);
    /* Only this caller's still-owned RUN continuation may use the address.
     * Nothing in this function dereferences caller-supplied tag_value. */
    return (uint32_t *)address;
}
#endif

int krnP4TaskConstructionPublish(struct Task *task,
    struct p4_task_registry_record *record, struct Task *creator,
    uint32_t hart, uint32_t *generation, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_register_constructing_locked(&p4_task_records,
        record, task, creator, hart, generation);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result;
}

int krnP4TaskConstructionFinish(struct Task *task, struct Task *creator,
    uint32_t generation, uint32_t hart, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_finish_construction_locked(&p4_task_records,
        task, creator, generation, hart);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 0)
    {
        /* Do not short-circuit: each identity can release independent work.
         * Only durable wake reservations occur while the gate is held. */
        poke = p4_task_pins_reserve_resource_wake_locked(task);
        if (p4_task_pins_reserve_resource_wake_locked(creator))
            poke = 1;
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}
#endif

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
int krnP4TaskConstructionOrphanFinish(struct Task *worker, struct Task *target,
    struct Task *creator, uint32_t generation, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(worker))
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_task_registry_finish_orphan_construction_locked(
        &p4_task_records, target, creator, generation);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result > 0)
    {
        poke = p4_task_pins_reserve_resource_wake_locked(target);
        if (p4_task_pins_reserve_resource_wake_locked(creator))
            poke = 1;
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}
#endif

void krnP4SignalTaskUnpin(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_unpin_locked(&p4_task_records, task);
    if (result)
        poke = p4_task_pins_reserve_resource_wake_locked(task);
    p4_task_pins_unlock(SysBase);
    if (!result)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (poke)
        krnP4CleanupPoke(SysBase);
}

int krnP4TaskRecordRetire(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    result = p4_task_registry_retire_with_ledger_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), task, 0);
#else
    result = p4_task_registry_retire_locked(&p4_task_records, task);
#endif
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result)
    {
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
        poke = p4_task_pins_cancel_removal_interest_locked(task);
#endif
        if (p4_task_pins_reserve_cleanup_wake_locked(task))
            poke = 1;
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4TaskRecordRetireForService(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    int owner;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    /* Snapshot the sole live RUN owner in the same gate transaction that
     * publishes retirement. After publication only this copied destination
     * crosses the gate; no Task field is inspected to discover its hart. */
    owner = p4_task_registry_running_owner_locked(&p4_task_records, task);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    result = p4_task_registry_retire_with_ledger_locked(&p4_task_records,
        krnP4SemaphoreBootLedger(), task, 1);
#else
    result = p4_task_registry_retire_for_service_locked(&p4_task_records, task);
#endif
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result)
    {
        if (owner < 0 || owner > 2)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
        poke = p4_task_pins_cancel_removal_interest_locked(task);
#endif
        if (p4_task_pins_reserve_cleanup_wake_locked(task))
            poke = 1;
    }
    p4_task_pins_unlock(SysBase);
    if (result && owner > 0)
        krnP4RetiredOwnerPoke((uint32_t)owner - 1u, SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result;
}

int krnP4TaskRecordEnterRunning(struct Task *task, unsigned int hart,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_enter_running_locked(&p4_task_records, task, hart);
    p4_task_pins_unlock(SysBase);
    return result;
}

void krnP4TaskRecordLeaveRunning(struct Task *task, unsigned int hart,
    struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_leave_running_locked(&p4_task_records, task, hart);
    if (result)
        poke = p4_task_pins_reserve_cleanup_wake_locked(task);
    p4_task_pins_unlock(SysBase);
    if (!result)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (poke)
        krnP4CleanupPoke(SysBase);
}

struct p4_task_registry_record *krnP4TaskRecordClaimReclaim(struct Task *task,
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record;
    p4_task_pins_lock(SysBase);
    record = p4_task_registry_claim_reclaim_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return record;
}

struct p4_task_registry_record *krnP4TaskRecordClaimService(struct Task *task,
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record;
    p4_task_pins_lock(SysBase);
    record = p4_task_registry_claim_service_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return record;
}

struct p4_task_registry_record *krnP4TaskRecordClaimQueuedService(struct Task *task,
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record;
    p4_task_pins_lock(SysBase);
    record = p4_task_registry_claim_queued_service_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return record;
}

struct p4_task_registry_record *krnP4TaskRecordClaimNextService(
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record = 0;
    int result;

    p4_task_pins_lock(SysBase);
    result = p4_task_registry_claim_next_service_locked(
        &p4_task_records, &record);
    if (result < 0)
    {
        /* Do not re-enable interrupts over a corrupt registry. The fatal path
         * halts with interrupts disabled; no Task identity is dereferenced. */
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        return 0;
    }
    p4_task_pins_unlock(SysBase);
    return result == 1 ? record : 0;
}

int krnP4TaskRecordRunningOwner(struct Task *task, struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_running_owner_locked(&p4_task_records, task);
    p4_task_pins_unlock(SysBase);
    return result;
}

struct p4_task_registry_record *krnP4TaskRecordClaimNextRetainedService(
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record = 0;
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_claim_next_retained_service_locked(
        &p4_task_records, &record);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    return result == 1 ? record : 0;
}

void krnP4TaskRecordFinishRetainedService(struct p4_task_registry_record *record,
    struct ExecBase *SysBase)
{
    int result;
    int poke = 0;
    p4_task_pins_lock(SysBase);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    result = p4_removal_bank != NULL ?
        p4_task_registry_finish_removal_locked(&p4_task_records,
            p4_removal_bank, record) :
        p4_task_registry_finish_retained_service_locked(&p4_task_records, record);
    if (result > 1)
    {
        p4_cleanup_wake_pending = true;
        poke = p4_cleanup_wake_mask != 0u;
    }
#else
    result = p4_task_registry_finish_retained_service_locked(&p4_task_records, record);
#endif
    /* This legacy void boundary still fails closed on unavailable headroom.
     * A progress-capable post-free retry owner must replace it before public
     * removal admission/SMP activation; never detach or ACK partially. */
    if (result <= 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
}

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
int krnP4TaskRecordTryFinishPostFree(struct p4_task_registry_record *record,
    struct ExecBase *SysBase)
{
    int result, poke = 0;
    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(p4_cleanup_worker) ||
        p4_removal_bank == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    result = p4_task_registry_finish_removal_locked(&p4_task_records,
        p4_removal_bank, record);
    if (result < 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    if (result == 0)
    {
        /* Refusal also includes a nonfinishable raw record. Prove this is
         * still a valid exclusive claim before treating it as pressure. */
        if (p4_task_registry_defer_removal_finish_locked(
                &p4_task_records, record) != 1)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        p4_removal_finish_waiting = true;
        if (p4_postfree_after != 0u && (uintptr_t)record <= p4_postfree_after)
            p4_postfree_restart = true;
        /* No self-poke merely because pressure persists. A subsequent
         * resource producer shares this gate and reserves a durable wake. */
    }
    else if (result > 1)
    {
        p4_cleanup_wake_pending = true;
        poke = 1;
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return result > 0 ? 1 : 0;
}

struct p4_task_registry_record *krnP4TaskRecordRetryPostFree(
    struct ExecBase *SysBase)
{
    struct p4_task_registry_record *record = NULL;
    struct p4_task_registry_record *finished = NULL;
    unsigned int attempted = 0u;
    int result, selected, poke = 0;

    p4_task_pins_lock(SysBase);
    if (!p4_task_pins_notification_worker_locked(p4_cleanup_worker) ||
        p4_removal_bank == NULL)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    while (attempted < P4_POSTFREE_ATTEMPT_BUDGET)
    {
        selected = p4_task_registry_next_postfree_after_locked(
            &p4_task_records, p4_postfree_after, &record);
        if (selected < 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        if (selected == 0)
        {
            p4_removal_finish_waiting = p4_postfree_blocked || p4_postfree_restart;
            if (p4_postfree_restart)
            {
                p4_cleanup_wake_pending = true;
                poke = 1;
            }
            p4_postfree_after = 0u;
            p4_postfree_blocked = false;
            p4_postfree_restart = false;
            break;
        }
        p4_postfree_after = (uintptr_t)record;
        ++attempted;
        result = p4_task_registry_finish_removal_locked(&p4_task_records,
            p4_removal_bank, record);
        if (result < 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        if (result > 0)
        {
            finished = record;
            /* Other postfree records may remain. Keep conservative interest
             * until a complete scan; never erase a producer's wake latch. */
            p4_removal_finish_waiting = true;
            if (result > 1)
            {
                p4_cleanup_wake_pending = true;
                poke = 1;
            }
            break;
        }
        p4_postfree_blocked = true;
    }
    if (attempted == P4_POSTFREE_ATTEMPT_BUDGET && finished == NULL)
    {
        /* A scalar cursor, not a borrowed record pointer, survives unlock.
         * Publish continuation only if the bounded slice has more to visit.
         * A completely pressure-blocked pass eventually becomes quiet. */
        selected = p4_task_registry_next_postfree_after_locked(
            &p4_task_records, p4_postfree_after, &record);
        if (selected < 0)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        p4_removal_finish_waiting = true;
        if (selected == 1)
        {
            p4_cleanup_wake_pending = true;
            poke = 1;
        }
        else
        {
            if (p4_postfree_restart)
            {
                p4_cleanup_wake_pending = true;
                poke = 1;
            }
            p4_postfree_after = 0u;
            p4_postfree_blocked = false;
            p4_postfree_restart = false;
        }
    }
    p4_task_pins_unlock(SysBase);
    if (poke)
        krnP4CleanupPoke(SysBase);
    return finished;
}
#endif

int krnP4TaskRecordServiceOwnerRunning(struct Task *task, unsigned int hart,
    struct ExecBase *SysBase)
{
    int result;
    p4_task_pins_lock(SysBase);
    result = p4_task_registry_service_owner_running_locked(&p4_task_records, task, hart);
    p4_task_pins_unlock(SysBase);
    return result;
}
