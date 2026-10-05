/*
    Copyright (c) 2015-2026, The AROS Development Team. All rights reserved.

    Desc: inter-hart hook calls for the smp variant of the esp32p4-riscv
          target, from arch/aarch64-native.
*/

#include <aros/kernel.h>
#include <aros/libcall.h>
#include <aros/types/spinlock_s.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <kernel_base.h>
#include <exec_platform.h>

#include "etask.h"
#include "kernel_intern.h"
#include "kernel_ipi.h"
#include "tls.h"

/*
 * Per target hart: a pool of free entries and the queue of pending calls,
 * under one lock. The sender takes an entry from the target's pool, fills
 * it, queues it and raises the target's inter-hart interrupt; the target
 * runs its queue from there. Static pools, not AllocMem(): Signal() is
 * callable from interrupts, and reaches here.
 *
 * Every queue section runs with interrupts masked, so that the drain from
 * the interrupt can never find its own hart holding the lock.
 */
struct CallIPIEntry
{
    struct MinNode cie_Node;
    struct IPIHook cie_IPIH;    /* the hook, then its arguments */
};

#define IPI_CALL_POOL_PER_HART  64

static struct CallIPIEntry ipi_call_pool[P4_TLS_HARTS][IPI_CALL_POOL_PER_HART];
static struct MinList      ipi_call_free[P4_TLS_HARTS];
static struct MinList      ipi_call_queue[P4_TLS_HARTS];
static spinlock_t          ipi_call_lock[P4_TLS_HARTS];
static volatile int        ipi_call_ready;

/* What a hart's drain is running now, so that a canceller can wait out a
   call it no longer finds queued. One writer per hart. */
static volatile APTR       ipi_call_exec_func[P4_TLS_HARTS];
static volatile IPTR       ipi_call_exec_arg1[P4_TLS_HARTS];

static void ipi_call_init(void)
{
    unsigned long s = p4_tls_mask();
    int cpu, i;

    /* Both harts may arrive here first; the lock word of hart 0's queue
       serves as the guard for the one-time setup. */
    EXEC_SPINLOCK_LOCK(&ipi_call_lock[0], NULL, SPINLOCK_MODE_WRITE);
    if (!ipi_call_ready)
    {
        for (cpu = 0; cpu < P4_TLS_HARTS; cpu++)
        {
            NewMinList(&ipi_call_free[cpu]);
            NewMinList(&ipi_call_queue[cpu]);
            for (i = 0; i < IPI_CALL_POOL_PER_HART; i++)
                ADDTAIL((struct List *)&ipi_call_free[cpu],
                        (struct Node *)&ipi_call_pool[cpu][i].cie_Node);
        }
        __atomic_store_n(&ipi_call_ready, 1, __ATOMIC_RELEASE);
    }
    EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[0]);
    p4_tls_unmask(s);
}

static struct CallIPIEntry *take_free(int cpu)
{
    unsigned long s = p4_tls_mask();
    struct CallIPIEntry *cie;

    EXEC_SPINLOCK_LOCK(&ipi_call_lock[cpu], NULL, SPINLOCK_MODE_WRITE);
    cie = (struct CallIPIEntry *)REMHEAD((struct List *)&ipi_call_free[cpu]);
    EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[cpu]);
    p4_tls_unmask(s);
    return cie;
}

static void put_free(struct CallIPIEntry *cie, int cpu)
{
    unsigned long s = p4_tls_mask();

    EXEC_SPINLOCK_LOCK(&ipi_call_lock[cpu], NULL, SPINLOCK_MODE_WRITE);
    ADDTAIL((struct List *)&ipi_call_free[cpu], (struct Node *)&cie->cie_Node);
    EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[cpu]);
    p4_tls_unmask(s);
}

/*
 * A free entry for the target, waiting rather than dropping the call: a
 * dropped signal is a wakeup lost for good. While waiting with interrupts
 * disabled this hart cannot run its own queue from the interrupt, so two
 * harts signalling each other with empty pools would wait on each other;
 * running our own queue here breaks that. No task lock may be held.
 */
struct CallIPIEntry *core_ClaimCallIPI(int cpu)
{
    struct CallIPIEntry *cie;

    if (!__atomic_load_n(&ipi_call_ready, __ATOMIC_ACQUIRE))
        ipi_call_init();

    for (;;)
    {
        if ((cie = take_free(cpu)))
            return cie;
        if (IDNESTCOUNT_GET >= 0)
            core_RunCallIPIs(GetCPUNumber());
    }
}

void core_CommitCallIPI(struct CallIPIEntry *cie, int cpu,
                        struct Hook *hook, int nargs, IPTR *args)
{
    unsigned long s;
    int i;

    if (nargs > IPI_CALL_HOOK_MAX_ARGS)
        nargs = IPI_CALL_HOOK_MAX_ARGS;

    cie->cie_IPIH.ih_Hook = *hook;
    for (i = 0; i < nargs; i++)
        cie->cie_IPIH.ih_Args[i] = args[i];

    s = p4_tls_mask();
    EXEC_SPINLOCK_LOCK(&ipi_call_lock[cpu], NULL, SPINLOCK_MODE_WRITE);
    ADDTAIL((struct List *)&ipi_call_queue[cpu], (struct Node *)&cie->cie_Node);
    EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[cpu]);
    p4_tls_unmask(s);

    krnP4IPISend(cpu, P4_IPI_CALL_HOOK);
}

void core_AbortCallIPI(struct CallIPIEntry *cie, int cpu)
{
    put_free(cie, cpu);
}

/*
 * Withdraw the queued calls matching (h_Entry, ih_Args[1]) on every hart,
 * then wait out one already running. On return no hart will touch the
 * object, provided the caller has made new commits impossible.
 */
void core_CancelCallIPIs(APTR hookEntry, IPTR matchArg)
{
    int cpu;

    if (!__atomic_load_n(&ipi_call_ready, __ATOMIC_ACQUIRE))
        return;

    for (cpu = 0; cpu < P4_TLS_HARTS; cpu++)
    {
        unsigned long s = p4_tls_mask();
        struct MinNode *node, *next;

        EXEC_SPINLOCK_LOCK(&ipi_call_lock[cpu], NULL, SPINLOCK_MODE_WRITE);
        for (node = ipi_call_queue[cpu].mlh_Head;
             (next = node->mln_Succ) != NULL; node = next)
        {
            struct CallIPIEntry *cie = (struct CallIPIEntry *)node;

            if ((APTR)cie->cie_IPIH.ih_Hook.h_Entry == hookEntry &&
                cie->cie_IPIH.ih_Args[1] == matchArg)
            {
                REMOVE((struct Node *)node);
                ADDTAIL((struct List *)&ipi_call_free[cpu], (struct Node *)node);
            }
        }
        EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[cpu]);
        p4_tls_unmask(s);

        while (ipi_call_exec_func[cpu] == hookEntry &&
               ipi_call_exec_arg1[cpu] == matchArg)
            EXEC_MEMORY_BARRIER();
    }
}

int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async,
                   int nargs, IPTR *args, APTR _KB)
{
    ULONG mask;
    int cpu;

    (void)_KB;

    /* Only asynchronous calls; signal.c is the one caller */
    if (!hook || !cpu_mask || !async || nargs > IPI_CALL_HOOK_MAX_ARGS)
        return 0;

    if ((IPTR)cpu_mask == TASKAFFINITY_ANY)
        mask = (1UL << P4_TLS_HARTS) - 1;
    else
        mask = *(ULONG *)cpu_mask;

    for (cpu = 0; cpu < P4_TLS_HARTS; cpu++)
    {
        if (mask & (1UL << cpu))
            core_CommitCallIPI(core_ClaimCallIPI(cpu), cpu, hook, nargs, args);
    }
    return 1;
}

/*
 * Run this hart's queued calls. From the inter-hart interrupt, or from a
 * claim that waits with interrupts disabled. Dispatch is blocked across
 * it: the hooks Enable() and Reschedule(), which must not switch tasks
 * from inside the interrupt; the switch happens on the way out.
 */
void core_RunCallIPIs(int cpu)
{
    if (!__atomic_load_n(&ipi_call_ready, __ATOMIC_ACQUIRE))
        return;

    EXEC_BLOCK_DISPATCH_INC;
    for (;;)
    {
        unsigned long s = p4_tls_mask();
        struct CallIPIEntry *cie;

        EXEC_SPINLOCK_LOCK(&ipi_call_lock[cpu], NULL, SPINLOCK_MODE_WRITE);
        cie = (struct CallIPIEntry *)REMHEAD((struct List *)&ipi_call_queue[cpu]);
        if (cie)
        {
            /* Off the queue the call is invisible to a canceller's scan,
               so it waits on this instead */
            ipi_call_exec_func[cpu] = (APTR)cie->cie_IPIH.ih_Hook.h_Entry;
            ipi_call_exec_arg1[cpu] = cie->cie_IPIH.ih_Args[1];
        }
        EXEC_SPINLOCK_UNLOCK(&ipi_call_lock[cpu]);
        p4_tls_unmask(s);

        if (!cie)
            break;

        /* signal_hook: A0 = the IPIHook, A1 and A2 unused */
        AROS_UFC3NR(void, cie->cie_IPIH.ih_Hook.h_Entry,
            AROS_UFCA(struct IPIHook *, &cie->cie_IPIH, A0),
            AROS_UFCA(APTR, NULL, A2),
            AROS_UFCA(APTR, NULL, A1));

        /* Its effects visible before a canceller is released */
        EXEC_MEMORY_BARRIER();
        ipi_call_exec_func[cpu] = NULL;

        put_free(cie, cpu);
    }
    EXEC_BLOCK_DISPATCH_DEC;
}
