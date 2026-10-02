/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * ESP32-P4 Exec platform mappings. Normal builds include the generic header
 * unchanged. The SMP mapping remains behind an explicit integration gate.
 */

#ifndef P4_EXEC_PLATFORM_H
#define P4_EXEC_PLATFORM_H

#if !defined(__AROSEXEC_SMP__)
#include <aros/config.h>
#endif

#if defined(P4_E3_RUNTIME_READY) && !defined(__AROSEXEC_SMP__)
#error "P4_E3_RUNTIME_READY is valid only for an AROS Exec SMP build"
#endif

#if defined(__AROSEXEC_SMP__)

#if !defined(P4_E3_RUNTIME_READY)
#error "P4 Exec SMP requires the reviewed P4_E3_RUNTIME_READY integration gate"
#endif

#include "../kernel/cpu_local_runtime.h"
#include <aros/types/spinlock_s.h>
#include <utility/hooks.h>
#include "../kernel/p4_task_publish.h"
#include "../kernel/p4_task_pins.h"
#include "../kernel/p4_public_spinlock.h"
#include "p4_softint.h"
#define EXEC_PLATFORM_SOFTINT_PENDING() krnP4SoftIntPending(SysBase)
#include "p4_semaphore_gate.h"
/* Single Obtain/Shared and Release use permanent request/durable notification
 * transactions. LIST and asynchronous messages remain unbound; this is not
 * a complete SMP semaphore or lifecycle contract. Activation stays refused. */
#define EXEC_PLATFORM_SEMAPHORE_LOCK(sem) Exec_P4SemaphoreLock((sem), SysBase)
#define EXEC_PLATFORM_SEMAPHORE_UNLOCK(sem) Exec_P4SemaphoreUnlock((sem), SysBase)
#include "p4_semaphore_public.h"
#define EXEC_PLATFORM_OBTAIN_SEMAPHORE(sem, task, owner) \
    Exec_P4ObtainSemaphore((sem), (task), (owner), SysBase)
#define EXEC_PLATFORM_RELEASE_SEMAPHORE(sem) \
    Exec_P4ReleaseSemaphore((sem), SysBase)

#define EXEC_PLATFORM_TASK_RECORD_CREATE(task) krnP4TaskRecordCreate((task), SysBase)
#include "p4_task_creation.h"
#define EXEC_PLATFORM_TASK_RECORD_RELEASE(task) krnP4SignalTaskUnpin((task), SysBase)
#define EXEC_PLATFORM_NEW_TASK_READY(task) krnP4NewTaskReady((task), SysBase)
#define EXEC_PLATFORM_BOOT_TASK_REGISTER(task) krnP4BootTaskRegister((task), SysBase)
#define EXEC_PLATFORM_TASK_FATAL() krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE)
#define EXEC_PLATFORM_SERVICE_PROTECT(task) krnP4ProtectCleanupWorker((task), SysBase)
#include "p4_cleanup_worker.h"
#define EXEC_PLATFORM_SERVICE_WORKER_RUN() Exec_P4ServiceLoop(SysBase)
#include "p4_self_remove.h"
#define EXEC_PLATFORM_REMOVE_SELF(task) Exec_P4RemoveSelf((task), SysBase)
#include "p4_remove_external.h"
#define EXEC_PLATFORM_REMOVE_EXTERNAL(task, requester) \
    Exec_P4RemoveExternal((task), (requester), SysBase)
#define EXEC_PLATFORM_TASK_REMOVE_ALLOWED(task) krnP4TaskRemovalAllowed((task), SysBase)
/* TaskResource must not touch target storage before core removal admission.
 * Its resident private callback runs only after the retained service claim. */
#define EXEC_PLATFORM_TASKRESOURCE_SERVICE_CLEANUP 1
#include "../kernel/p4_taskresource_bootstrap.h"
#define EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_ISOLATED() \
    krnP4TaskResourceBootstrapIsolated(SysBase)
#define EXEC_PLATFORM_TASKRESOURCE_BOOTSTRAP_TRANSACTION(callback, context) \
    krnP4TaskResourceBootstrapTransaction(SysBase, (callback), (context))
/* Keep topology private until fallible context/record setup passes.
 * Construction ordering alone does not implement the metadata topology gate. */
#define EXEC_PLATFORM_ETASK_STAGED 1
#include "p4_etask_identity_binding.h"
#define EXEC_PLATFORM_ETASK_RESERVE(et) krnP4ETaskIdentityReserve((et), SysBase)
#define EXEC_PLATFORM_ETASK_RELEASE(et) krnP4ETaskIdentityRelease((et), SysBase)
#define EXEC_PLATFORM_ETASK_CLEANUP_BEGIN(et) krnP4ETaskMetadataBeginCleanup((et), SysBase)
#define EXEC_PLATFORM_ETASK_RESULT_RETAIN(et) krnP4ETaskMetadataRetainResult((et), SysBase)
struct Task;
struct ExecBase;
void Exec_PublishConstructedETask(struct Task *, struct Task *, struct ExecBase *);

/* Direct core calls are required before kernel.resource vectors exist.
 * These are the current kernel.conf LVOs, not the stale native-RISC-V ones.
 * The wrappers deliberately do not require an initialized KernelBase. */
extern void Kernel_49_KrnSpinInit(spinlock_t *, void *);
extern spinlock_t *Kernel_52_KrnSpinLock(spinlock_t *, struct Hook *, ULONG,
                                      void *);
extern void Kernel_53_KrnSpinUnLock(spinlock_t *, void *);
#define EXEC_SPINLOCK_INIT(lock) Kernel_49_KrnSpinInit((lock), NULL)
#define EXEC_SPINLOCK_LOCK(lock, hook, mode) \
    Kernel_52_KrnSpinLock((lock), (hook), (mode), NULL)
#define EXEC_SPINLOCK_UNLOCK(lock) Kernel_53_KrnSpinUnLock((lock), NULL)

/* PrepareExecBase calls the platform hook immediately after allocating its
 * new SysBase, before vectors, spinlocks or CPU-local Exec macros are used. */
#define HAVE_PREPAREPLATFORM

/* P4 cpu_Dispatch owns the launch callback after context/FPU restoration
 * and Exception(). The generic dispatcher must not call it a second time.
 * This selection is SMP-only; normal P4 behavior is unchanged here. */
#define EXEC_DISPATCH_LAUNCH_IN_CPU 1

#define SCHEDQUANTUM_VALUE P4_EXEC_RUNTIME_DEFAULT_QUANTUM

struct Exec_PlatformData
{
    /* No ExecBase layout extension; CPU state lives in internal SRAM. */
};

/*
 * The caller's A6 SysBase remains explicit. These accessors select state by
 * bounds-checked mhartid and validate the recorded SysBase before use.
 */
#define P4_EXEC_RUNTIME(SysBaseArg) \
    p4_exec_runtime_current((SysBaseArg))
#define P4_EXEC_CPU_LOCAL(SysBaseArg) \
    (P4_EXEC_RUNTIME((SysBaseArg))->cpu_local)

#define IDNESTCOUNT_INC \
    do { P4_EXEC_CPU_LOCAL(SysBase)->id_nest++; } while (0)
#define IDNESTCOUNT_DEC \
    do { P4_EXEC_CPU_LOCAL(SysBase)->id_nest--; } while (0)
#define TDNESTCOUNT_INC \
    do { P4_EXEC_CPU_LOCAL(SysBase)->td_nest++; } while (0)
#define TDNESTCOUNT_DEC \
    do { P4_EXEC_CPU_LOCAL(SysBase)->td_nest--; } while (0)

#define IDNESTCOUNT_GET \
    (P4_EXEC_CPU_LOCAL(SysBase)->id_nest)
#define IDNESTCOUNT_SET(val) \
    (P4_EXEC_CPU_LOCAL(SysBase)->id_nest = (val))
#define TDNESTCOUNT_GET \
    (P4_EXEC_CPU_LOCAL(SysBase)->td_nest)
#define TDNESTCOUNT_SET(val) \
    (P4_EXEC_CPU_LOCAL(SysBase)->td_nest = (val))

#define FLAG_SCHEDQUANTUM_CLEAR \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags &= \
             ~P4_EXEC_SCHEDULE_QUANTUM; } while (0)
#define FLAG_SCHEDQUANTUM_SET \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags |= \
             P4_EXEC_SCHEDULE_QUANTUM; } while (0)
#define FLAG_SCHEDSWITCH_CLEAR \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags &= \
             ~P4_EXEC_SCHEDULE_SWITCH; } while (0)
#define FLAG_SCHEDSWITCH_SET \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags |= \
             P4_EXEC_SCHEDULE_SWITCH; } while (0)
#define FLAG_SCHEDDISPATCH_CLEAR \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags &= \
             ~P4_EXEC_SCHEDULE_DISPATCH; } while (0)
#define FLAG_SCHEDDISPATCH_SET \
    do { P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags |= \
             P4_EXEC_SCHEDULE_DISPATCH; } while (0)

#define FLAG_SCHEDQUANTUM_ISSET \
    (P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags & \
     P4_EXEC_SCHEDULE_QUANTUM)
#define FLAG_SCHEDSWITCH_ISSET \
    (P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags & \
     P4_EXEC_SCHEDULE_SWITCH)
#define FLAG_SCHEDDISPATCH_ISSET \
    (P4_EXEC_CPU_LOCAL(SysBase)->schedule_flags & \
     P4_EXEC_SCHEDULE_DISPATCH)

#define SCHEDQUANTUM_SET(val) \
    (P4_EXEC_RUNTIME(SysBase)->quantum = (val))
#define SCHEDQUANTUM_GET \
    (P4_EXEC_RUNTIME(SysBase)->quantum)
#define SCHEDELAPSED_SET(val) \
    (P4_EXEC_RUNTIME(SysBase)->elapsed = (val))
#define SCHEDELAPSED_GET \
    (P4_EXEC_RUNTIME(SysBase)->elapsed)

#define GET_THIS_TASK \
    (P4_EXEC_CPU_LOCAL(SysBase)->current_task)
#define SET_THIS_TASK(task) \
    do { \
        (void)p4_exec_runtime_set_current_task(SysBase, (task)); \
    } while (0)

#else

/* Preserve the existing single-hart P4 behavior byte-for-byte at the macro
 * level until __AROSEXEC_SMP__ is explicitly selected and reviewed. */
#include "../../../rom/exec/exec_platform.h"

#endif /* __AROSEXEC_SMP__ */

#endif /* P4_EXEC_PLATFORM_H */
