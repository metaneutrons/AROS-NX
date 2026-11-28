#ifndef KERNEL_IPI_H_
#define KERNEL_IPI_H_
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: inter-hart calls for the smp variant of the esp32p4-riscv target.
*/

#include <utility/hooks.h>

/*
 * The interface rom/exec/signal.c needs, in its cancelable form: a call
 * is claimed and committed in two steps, so that exec can check the
 * target's state between them, and RemTask() can withdraw every call
 * still queued for a task before its memory goes. The calls are queued
 * per target hart and run there from its inter-hart interrupt.
 */

#define IPI_CALL_HOOK_MAX_ARGS  5

struct IPIHook
{
    struct Hook ih_Hook;
    IPTR        ih_Args[IPI_CALL_HOOK_MAX_ARGS];
};

struct CallIPIEntry;

extern struct CallIPIEntry *core_ClaimCallIPI(int cpu);
extern void core_CommitCallIPI(struct CallIPIEntry *cie, int cpu,
                               struct Hook *hook, int nargs, IPTR *args);
extern void core_AbortCallIPI(struct CallIPIEntry *cie, int cpu);
extern void core_CancelCallIPIs(APTR hookEntry, IPTR matchArg);
extern int core_DoCallIPI(struct Hook *hook, void *cpu_mask, int async,
                          int nargs, IPTR *args, APTR _KB);
#define KERNEL_IPI_CALL_CANCELABLE

/* Port side: run this hart's queued calls (kernel_smp.c's IPI handler) */
extern void core_RunCallIPIs(int cpu);

#endif /* KERNEL_IPI_H_ */
