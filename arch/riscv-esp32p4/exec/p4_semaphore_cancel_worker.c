/* Operation cancellation consumer in the gated SMP ServiceTask continuation.
 * No borrowed arena/request pointers survive an unlock. Public semaphore
 * routing and the full concurrent runtime still require qualification. */
#if !defined(P4_SEMAPHORE_CANCEL_WORKER_TEST)
#include <exec/execbase.h>
#include <proto/exec.h>
#include "p4_signal_adapter.h"
#include "p4_semaphore_gate.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_ipi.h"
#endif
#include "../kernel/p4_semaphore_cancel.h"
#include "../kernel/p4_semaphore_cancel_retry.h"
#include "p4_semaphore_cancel_worker.h"

int Exec_P4SemaphoreCancellationPass(struct Task *worker,
    struct ExecBase *SysBase)
{
    uintptr_t cursor = 0;
    uint32_t visited, blocked = 0;
    int progress = 0;

    /* A concurrent capacity drain after this registration must retain a wake
     * even if it occurs before the consumer publishes its blocked result. */
    krnP4SemaphoreCancelRetryBegin(worker, SysBase);
    for (visited = 0; visited < P4_TASK_REGISTRY_LIMIT; ++visited)
    {
        struct p4_task_wait_cancel_snapshot scope;
        struct p4_semaphore_cancel_entry_snapshot entry;
        uint32_t ordinal, entry_count, observed_running;
        int result = krnP4SemaphoreCancelClaimAfter(worker, cursor, &scope,
            SysBase);
        if (result == 0)
            break;
        if (result != 1 || scope.identity == NULL || scope.generation == 0 ||
            scope.kind != P4_TASK_WAIT_OPERATION ||
            (uintptr_t)scope.identity <= cursor ||
            scope.semaphore_index != NULL || scope.request_index != NULL ||
            scope.list_index != NULL)
            p4_signal_adapter_fail_closed();
        cursor = (uintptr_t)scope.identity;
        if (krnP4SemaphoreCancelDescribe(worker, scope.identity,
                scope.generation, 0, &entry, SysBase) != 1 ||
            entry.entry_count == 0 ||
            entry.entry_count > P4_SEMAPHORE_LEDGER_LIMIT)
            p4_signal_adapter_fail_closed();
        entry_count = entry.entry_count;
        observed_running = entry.running_harts;
        for (ordinal = 0; ordinal < entry_count; ++ordinal)
        {
            if (krnP4SemaphoreCancelDescribe(worker, scope.identity,
                    scope.generation, ordinal, &entry, SysBase) != 1 ||
                entry.entry_count != entry_count || entry.committed > 1u ||
                entry.running_harts > 2u)
                p4_signal_adapter_fail_closed();
            if (entry.state == P4_SEMAPHORE_LEDGER_ENTRY_UNVISITED ||
                entry.state == P4_SEMAPHORE_LEDGER_ENTRY_RELEASED ||
                entry.state == P4_SEMAPHORE_LEDGER_ENTRY_WITHDRAWN)
                continue;
            if (entry.state != P4_SEMAPHORE_LEDGER_ENTRY_QUEUED &&
                entry.state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
                p4_signal_adapter_fail_closed();
            if (entry.committed != 0)
            {
                /* Completed acquisitions retain ownership; reclaim only
                 * bookkeeping after RUN drains, never undo their units. */
                if (entry.state != P4_SEMAPHORE_LEDGER_ENTRY_OWNED)
                    p4_signal_adapter_fail_closed();
                continue;
            }
            if (entry.state == P4_SEMAPHORE_LEDGER_ENTRY_OWNED &&
                entry.running_harts != 0)
            {
                blocked = 1u;
                continue;
            }
            if (entry.semaphore == NULL)
                p4_signal_adapter_fail_closed();
            /* No registry gate remains. The scope claim retains operation
             * metadata, not the independently caller-owned semaphore storage.
             * Exactly one initialized semaphore gate is held at a time. */
            Exec_P4SemaphoreLock(entry.semaphore, SysBase);
            if (entry.state == P4_SEMAPHORE_LEDGER_ENTRY_QUEUED)
                result = krnP4SemaphoreCancelWithdraw(worker, scope.identity,
                    scope.generation, ordinal, entry.semaphore, SysBase);
            else
                result = krnP4SemaphoreCancelRollbackDurable(worker,
                    scope.identity, scope.generation, ordinal,
                    entry.semaphore, SysBase);
            Exec_P4SemaphoreUnlock(entry.semaphore, SysBase);
            if (result < 0)
                p4_signal_adapter_fail_closed();
            if (result > 0)
            {
                progress = 1;
                krnP4CleanupPoke(SysBase);
            }
            else
                blocked = 1u;
        }
        result = krnP4SemaphoreOperationFinishCancel(scope.identity,
            scope.generation, SysBase);
        if (result == 1)
            progress = 1;
        else
        {
            if (result != 0 || krnP4SemaphoreCancelReleaseClaim(worker,
                    scope.identity, scope.generation, observed_running,
                    SysBase) != 1)
                p4_signal_adapter_fail_closed();
            blocked = 1u;
        }
    }
    /* Newly eligible higher addresses may require another bounded pass. A
     * producer wake or actual progress drives it; no blocked-owner self-poke. */
    if (visited == P4_TASK_REGISTRY_LIMIT)
        blocked = 1u;
    krnP4SemaphoreCancelRetryEnd(worker, blocked, SysBase);
    return progress;
}
