/* Owner-local startup and generation-bound ACK for the real Exec hart1 entry.
 * No hardware release or public startup caller is installed by this file.
 */
#include <stddef.h>
#include <asm/cpu.h>
#include "kernel_intern.h"
#include "kernel_syscall.h"
#include "hardware.h"
#include "secondary_hw.h"
#include "cpu_local_runtime.h"
#include "p4_isr_stack.h"
#include "p4_ipi.h"
#include "p4_cpu_masks.h"
#include "p4_task_pins.h"
#include "p4_public_spinlock.h"
#include "p4_secondary_exec.h"

#if !defined(__AROSEXEC_SMP__) || !defined(__AROSPLATFORM_SMP__)
#error "Production secondary Exec setup requires a matched SMP ABI"
#endif
_Static_assert(sizeof(void *) == 4, "secondary command uses the RV32 ABI");
_Static_assert(offsetof(struct p4_secondary_exec_exchange, reply) == 64,
               "assembly reply offset must match");
_Static_assert(sizeof(struct p4_secondary_exec_exchange) == 128,
               "independent command and reply cache lines");
struct p4_secondary_exec_exchange __p4_secondary_exec_exchange
    __attribute__((section(".bss.p4_secondary_exec_exchange"), aligned(64)));

static volatile struct p4_secondary_exec_exchange *exchange(void)
{
    return (volatile struct p4_secondary_exec_exchange *)(
        (uintptr_t)&__p4_secondary_exec_exchange + P4_SECONDARY_UNCACHED_OFFSET);
}

int krnP4SecondaryExecPrepare(uint32_t generation, struct ExecBase *SysBase,
    struct KernelBase *kernel_base)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    volatile struct p4_secondary_exec_exchange *x;
    unsigned int i;
    if (generation == 0 || kernel_base == NULL || runtime == NULL ||
        runtime->hart_id != 0 ||
        runtime->stage != P4_EXEC_RUNTIME_STAGE_FINALIZED ||
        runtime->cpu_local->kernel_base != kernel_base ||
        runtime->cpu_local->id_nest < 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        (p4_r32(P4_SECONDARY_RESET_REG) & P4_SECONDARY_RESET_BIT) == 0 ||
        (p4_r32(P4_SECONDARY_CLOCK_REG) & P4_SECONDARY_CLOCK_BIT) != 0 ||
        !krnP4CleanupWorkerReady(SysBase))
        return 0;
    /* Preparation is one-shot. Flush inherited BSS aliases before switching
     * this exchange permanently to uncached access, while hart1 is held. */
    krnP4CacheSyncData(&__p4_secondary_exec_exchange,
        sizeof(__p4_secondary_exec_exchange));
    x = exchange();
    if (x->command[0] != 0 || x->command[1] != 0 || x->reply[0] != 0)
        return 0;
    krnP4CacheWriteback(); /* publish primary-built runtime/global data */
    for (i = 0; i < 16; ++i)
    {
        x->command[i] = 0;
        x->reply[i] = 0;
    }
    x->command[1] = generation;
    x->command[2] = (uint32_t)(uintptr_t)SysBase;
    x->command[3] = (uint32_t)(uintptr_t)kernel_base;
    __asm__ volatile("fence rw,rw" ::: "memory");
    x->command[0] = P4_SECONDARY_EXEC_PREPARED;
    __asm__ volatile("fence rw,rw" ::: "memory");
    return 1;
}

int krnP4SecondaryExecReady(uint32_t generation)
{
    volatile struct p4_secondary_exec_exchange *x = exchange();
    if (P4_CPU_LOCAL_READ_MHARTID() != 0 || generation == 0 ||
        x->reply[0] != P4_SECONDARY_EXEC_READY)
        return 0;
    __asm__ volatile("fence rw,rw" ::: "memory");
    return x->command[0] == P4_SECONDARY_EXEC_PREPARED &&
        x->command[1] == generation && x->reply[1] == generation &&
        x->reply[2] == 1 && x->reply[3] == 0 && x->reply[4] == 0 &&
        x->reply[5] != 0 && (x->reply[5] & 15u) == 0 && x->reply[6] == 0;
}

int krnP4SecondaryExecGo(uint32_t generation)
{
    if ((csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        !krnP4SecondaryExecReady(generation) || krnP4GetCPUCount() != 2)
        return 0;
    __asm__ volatile("fence rw,rw" ::: "memory");
    exchange()->command[0] = P4_SECONDARY_EXEC_GO;
    __asm__ volatile("fence rw,rw" ::: "memory");
    return 1;
}

int krnP4SecondaryExecBootstrap(uint32_t generation, struct ExecBase *SysBase,
    struct KernelBase *kernel_base)
{
    volatile struct p4_secondary_exec_exchange *x = exchange();
    struct p4_exec_runtime *runtime;
    if (P4_CPU_LOCAL_READ_MHARTID() != 1 || generation == 0 ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0 ||
        x->command[0] != P4_SECONDARY_EXEC_PREPARED ||
        x->command[1] != generation ||
        x->command[2] != (uint32_t)(uintptr_t)SysBase ||
        x->command[3] != (uint32_t)(uintptr_t)kernel_base ||
        !krnP4ExecRuntimeFinalize(SysBase, kernel_base))
        return 0;
    runtime = p4_exec_runtime_current(SysBase);
    if (runtime == NULL || runtime->cpu_local->current_task != NULL ||
        runtime->cpu_local->trap_depth != 0 || runtime->wait_pending != NULL ||
        !krnP4ISRStackInstall())
        return 0;
    /* The registry query in IpiArm balances Disable/Enable; retain nesting0
     * so that it cannot enable MIE before ACK/GO and the first genuine trap. */
    runtime->cpu_local->id_nest = 0;
    krnCLICInit(); /* current-core-relative CLIC, never remote alias */
    krnP4IpiArm(SysBase);
    return 1;
}

void krnP4SecondaryExecDispatch(struct ExecBase *SysBase)
{
    struct p4_exec_runtime *runtime = p4_exec_runtime_current(SysBase);
    if (P4_CPU_LOCAL_READ_MHARTID() != 1 || runtime == NULL ||
        runtime->cpu_local->current_task != NULL ||
        runtime->cpu_local->trap_depth != 0 || runtime->wait_pending != NULL ||
        (csr_read(mstatus) & MSTATUS_MIE) != 0)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    runtime->cpu_local->id_nest = -1;
    /* Entry through the existing trap builds the real ISR/FPU frame and
     * establishes trap_depth. Never synthesize a frame or call cpu_Dispatch
     * outside the trap handler. A successful dispatch replaces this context. */
    __asm__ volatile("li a7,%0\n\tecall" : : "i"(SC_DISPATCH) : "a7", "memory");
    krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}
