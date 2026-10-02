/* Never install a retired callback recipient's saved return/FPU frame. */
#if !defined(P4_DISPATCH_CALLBACKS_TEST)
#include <exec/execbase.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <aros/asmcall.h>
#include "exec_intern.h"
#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#include "p4_dispatch_callbacks.h"
#include "p4_callback_escape.h"
#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "P4 dispatch callbacks require matched SMP"
#endif
#endif

static int callback_current_live(struct Task *task, struct ExecBase *SysBase,
                                 ULONG *flags, APTR *launch)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    int live;
    KrnCli();
    if (!task || !runtime || !runtime->cpu_local ||
        runtime->cpu_local->current_task != task)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    /* The RUN lease outlives the final Task unlock. No ordinary pin can
     * replace it: a callback can retire this task while still returning. */
    EXEC_SPINLOCK_LOCK(&task->tc_SpinLock, NULL, SPINLOCK_MODE_WRITE);
    live = task->tc_State == TS_RUN;
    if (!live && task->tc_State != TS_REMOVED && task->tc_State != TS_INVALID &&
        task->tc_State != TS_TOMBSTONED)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    *flags = live ? task->tc_Flags : 0;
    *launch = live ? (APTR)task->tc_Launch : NULL;
    EXEC_SPINLOCK_UNLOCK(&task->tc_SpinLock);
    return live;
}

struct p4_dispatch_callback_args
{
    struct Task *task;
    struct ExecBase *sys_base;
    int live;
};

static void dispatch_callbacks_invoke(void *opaque)
{
    struct p4_dispatch_callback_args *args = opaque;
    struct Task *task = args->task;
    struct ExecBase *SysBase = args->sys_base;
    ULONG flags;
    APTR launch;
    if (!callback_current_live(task, SysBase, &flags, &launch))
        return;
    if (flags & TF_EXCEPT)
        Exception();
    if (!callback_current_live(task, SysBase, &flags, &launch))
        return;
    if (flags & TF_LAUNCH)
    {
        if (!launch)
            krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        AROS_UFC1(void, launch, AROS_UFCA(struct ExecBase *, SysBase, A6));
    }
    args->live = callback_current_live(task, SysBase, &flags, &launch);
}

int krnP4DispatchCallbacks(struct Task *task, struct ExecBase *SysBase)
{
    struct p4_dispatch_callback_args args = {task, SysBase, 0};
    if (!krnP4RunTaskCallback(dispatch_callbacks_invoke, &args, SysBase))
        return 0;
    return args.live;
}
