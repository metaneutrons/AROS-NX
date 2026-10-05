/*
    Copyright (c) 2015-2026, The AROS Development Team. All rights reserved.

    Desc: the bootstrap task of a secondary hart, for the smp variant of
          the esp32p4-riscv target; from arch/aarch64-native.
*/

#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <aros/riscv/cpucontext.h>

#include <kernel_base.h>
#include <proto/kernel.h>

#include "etask.h"
#include "exec_intern.h"
#include "kernel_intern.h"

#undef bug
#include <kernel_debug.h>

extern BOOL Exec_InitETask(struct Task *, struct Task *, struct ExecBase *);

/*
 * A task for the hart to be when it first switches: cpu_Switch() needs one
 * to save the context into. It runs at -128, below the hart's idle task,
 * so it never runs again once the idle task exists, and only on this hart.
 * Its stack is the one the hart is already running on, so that the stack
 * check in core_Switch() holds.
 */
struct Task *cpu_InitBootStrap(struct ExecBase *SysBase, APTR splower,
                               APTR spupper)
{
    struct ExceptionContext *bsctx;
    struct MemList *ml;
    struct Task *bstask;
    ULONG cpunum = GetCPUNumber();
    void *aff;
    char *name;

    ml = AllocMem(sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR);
    if (!ml)
        return NULL;
    ml->ml_NumEntries = 1;

    ml->ml_ME[0].me_Length = sizeof(struct Task);
    ml->ml_ME[0].me_Addr = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    bsctx = KrnCreateContext();
    name = AllocVec(sizeof("CPU #00 Bootstrap"), MEMF_PUBLIC | MEMF_CLEAR);

    if (!ml->ml_ME[0].me_Addr || !bsctx || !name)
        goto fail;

    bstask = ml->ml_ME[0].me_Addr;
    bstask->tc_SPLower = splower;
    bstask->tc_SPUpper = spupper;

    NEWLIST(&bstask->tc_MemEntry);
    AddHead(&bstask->tc_MemEntry, &ml->ml_Node);

    CopyMem("CPU #00 Bootstrap", name, sizeof("CPU #00 Bootstrap"));
    name[5] = '0' + (cpunum / 10) % 10;
    name[6] = '0' + cpunum % 10;
    bstask->tc_Node.ln_Name = name;
    bstask->tc_Node.ln_Type = NT_TASK;
    bstask->tc_Node.ln_Pri  = -128;
    bstask->tc_State        = TS_READY;
    bstask->tc_SigAlloc     = 0xFFFF;

    if (!Exec_InitETask(bstask, NULL, SysBase))
        goto fail;
    bstask->tc_UnionETask.tc_ETask->et_RegFrame = bsctx;

    /* This hart only */
    IntETask(bstask->tc_UnionETask.tc_ETask)->iet_CpuNumber = cpunum;
    aff = KrnAllocCPUMask();
    if (aff)
        KrnGetCPUMask(cpunum, aff);
    IntETask(bstask->tc_UnionETask.tc_ETask)->iet_CpuAffinity = aff;

    bsctx->fp = 0;
    bsctx->ra = (ULONG)(IPTR)SysBase->TaskExitCode;

    return bstask;

fail:
    bug("[Kernel:%02d] no memory for the bootstrap task\n", cpunum);
    if (name)
        FreeVec(name);
    if (bsctx)
        KrnDeleteContext(bsctx);
    if (ml->ml_ME[0].me_Addr)
        FreeMem(ml->ml_ME[0].me_Addr, ml->ml_ME[0].me_Length);
    FreeMem(ml, sizeof(struct MemList));
    return NULL;
}

void cpu_BootStrap(struct Task *bstask, struct ExecBase *SysBase)
{
    bstask->tc_State = TS_RUN;
    SET_THIS_TASK(bstask);
}
