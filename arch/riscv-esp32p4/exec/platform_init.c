/* Copyright (c) 2026, The AROS Development Team. All rights reserved. */
#include <stddef.h>
#include "exec_intern.h"

#if defined(__AROSEXEC_SMP__)
#include "../kernel/cpu_local_runtime.h"
#include "p4_etask_gate.h"

BOOL Exec_PreparePlatform(struct Exec_PlatformData *pd, struct TagItem *tags)
{
    struct IntExecBase *base;

    (void)tags;
    if (pd == NULL)
        return FALSE;

    /* This hook cannot call Exec: even the function vectors do not exist yet.
     * Derive the exact allocated base from the platform-data member rather
     * than looking up a task or attaching KernelBase prematurely. */
    base = (struct IntExecBase *)((char *)pd -
                                offsetof(struct IntExecBase, PlatformData));
    if (!krnP4ExecRuntimeBootstrapBind(&base->pub))
        return FALSE;
    return krnP4ETaskGatePrepare() ? TRUE : FALSE;
}
#endif
