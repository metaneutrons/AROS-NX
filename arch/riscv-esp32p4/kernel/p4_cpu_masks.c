/*
 * ESP32-P4 CPU affinity masks and online-hart publication.
 *
 * Allocated masks are one word.  All post-allocation accesses go through the
 * bounded soft-atomic service because public memory may be cached PSRAM.
 */
#include <stddef.h>
#include <stdint.h>

#include "p4_cpu_masks.h"

#include "p4_soft_atomic.h"

#if defined(P4_CPU_MASKS_TEST)

#define P4_CPU_MASKS_SRAMDATA
#define P4_CPU_MASKS_ANY ((void *)(uintptr_t)UINTPTR_MAX)
#define P4_CPU_MASKS_ALL_BUT_SELF ((void *)(uintptr_t)(UINTPTR_MAX - 1u))
#define P4_CPU_MASKS_MEMF_PUBLIC 0x00000001u
#define P4_CPU_MASKS_MEMF_CLEAR  0x00010000u

extern void *p4CpuMasksTestAllocMem(size_t size, uint32_t flags);
extern void p4CpuMasksTestFreeMem(void *memory, size_t size);
extern int p4CpuMasksTestGetCPUNumber(void);
extern void p4CpuMasksTestDisableInterrupts(void);
extern void p4CpuMasksTestFatal(enum p4_soft_atomic_result reason)
    __attribute__((noreturn));

#define P4_CPU_MASKS_ALLOC(size, flags) p4CpuMasksTestAllocMem((size), (flags))
#define P4_CPU_MASKS_FREE(memory, size) p4CpuMasksTestFreeMem((memory), (size))
#define P4_CPU_MASKS_CURRENT_HART() p4CpuMasksTestGetCPUNumber()

#else

#include <aros/cpu.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include "kernel_intern.h"

#define P4_CPU_MASKS_SRAMDATA P4_SRAMDATA
#define P4_CPU_MASKS_ANY ((void *)(uintptr_t)TASKAFFINITY_ANY)
#define P4_CPU_MASKS_ALL_BUT_SELF \
    ((void *)(uintptr_t)TASKAFFINITY_ALL_BUT_SELF)
#define P4_CPU_MASKS_MEMF_PUBLIC MEMF_PUBLIC
#define P4_CPU_MASKS_MEMF_CLEAR MEMF_CLEAR

#define P4_CPU_MASKS_ALLOC(size, flags) AllocMem((size), (flags))
#define P4_CPU_MASKS_FREE(memory, size) FreeMem((memory), (size))
#define P4_CPU_MASKS_CURRENT_HART() GetCPUNumber()

_Static_assert(sizeof(cpumask_t) == sizeof(uint32_t),
               "ESP32-P4 CPU masks require a 32-bit cpumask_t");

#endif

#define P4_CPU_MASK_BYTES ((size_t)sizeof(uint32_t))
#define P4_CPU_MASK_POSSIBLE_COUNT 2u

/*
 * This is private internal-SRAM state.  It is deliberately separate from
 * possible-hart count: only the root launch path can publish hart 1 online.
 */
static uint32_t p4_online_hart_mask P4_CPU_MASKS_SRAMDATA = 1u;

static void p4_cpu_mask_service_failure(
    enum p4_soft_atomic_result reason) __attribute__((noreturn));

static void p4_cpu_mask_service_failure(enum p4_soft_atomic_result reason)
{
#if defined(P4_CPU_MASKS_TEST)
    p4CpuMasksTestDisableInterrupts();
    p4CpuMasksTestFatal(reason);
#else
    (void)reason;
    /* A broken affinity publication cannot be reported as a false predicate. */
    __asm__ volatile("csrci mstatus,8" ::: "memory");
    for (;;)
        __asm__ volatile("nop" ::: "memory");
#endif
}

static void p4_cpu_mask_store(void *mask, uint32_t value)
{
    uint32_t old_value;
    enum p4_soft_atomic_result result = krnP4SoftAtomicOperate(
        (volatile void *)mask, 4u, P4_SOFT_ATOMIC_STORE, value, &old_value);

    if (result != P4_SOFT_ATOMIC_OK)
        p4_cpu_mask_service_failure(result);
}

static uint32_t p4_cpu_mask_load(const void *mask)
{
    uint32_t value;
    enum p4_soft_atomic_result result = krnP4SoftAtomicOperate(
        (volatile void *)mask, 4u, P4_SOFT_ATOMIC_LOAD, 0u, &value);

    if (result != P4_SOFT_ATOMIC_OK)
        p4_cpu_mask_service_failure(result);

    return value;
}

static int p4_cpu_mask_id_valid(uint32_t number)
{
    return number < P4_CPU_MASK_POSSIBLE_COUNT && number < 32u;
}

static int p4_cpu_mask_pointer_is_special(const void *mask)
{
    return mask == NULL || mask == P4_CPU_MASKS_ANY ||
           mask == P4_CPU_MASKS_ALL_BUT_SELF;
}

void *krnP4AllocCPUMask(void)
{
    return P4_CPU_MASKS_ALLOC(
        P4_CPU_MASK_BYTES,
        P4_CPU_MASKS_MEMF_PUBLIC | P4_CPU_MASKS_MEMF_CLEAR);
}

void krnP4FreeCPUMask(void *mask)
{
    if (!p4_cpu_mask_pointer_is_special(mask))
        P4_CPU_MASKS_FREE(mask, P4_CPU_MASK_BYTES);
}

void krnP4ClearCPUMask(void *mask)
{
    if (p4_cpu_mask_pointer_is_special(mask))
        return;

    p4_cpu_mask_store(mask, 0u);
}

void krnP4GetCPUMask(uint32_t number, void *mask)
{
    if (!p4_cpu_mask_id_valid(number) ||
        p4_cpu_mask_pointer_is_special(mask))
        return;

    /*
     * The Kernel resource API sets a one-CPU mask.  Callers that need to
     * accumulate CPUs clear the mask first and issue another API operation;
     * this port does not change that established SET behavior.
     */
    p4_cpu_mask_store(mask, 1u << number);
}

int krnP4CPUInMask(uint32_t number, const void *mask)
{
    uint32_t word;
    int caller_hart;

    if (!p4_cpu_mask_id_valid(number))
        return 0;

    /* TASKAFFINITY_BOOT is the NULL mask and means hart 0. */
    if (mask == NULL)
        return number == 0u;

    if (mask == P4_CPU_MASKS_ANY)
        return 1;

    if (mask == P4_CPU_MASKS_ALL_BUT_SELF)
    {
        caller_hart = P4_CPU_MASKS_CURRENT_HART();
        if (caller_hart < 0 ||
            (unsigned int)caller_hart >= P4_CPU_MASK_POSSIBLE_COUNT)
            return 0;
        return number != (uint32_t)caller_hart;
    }

    word = p4_cpu_mask_load(mask);
    return (word & (1u << number)) != 0u;
}

unsigned int krnP4GetCPUCount(void)
{
    uint32_t online_mask =
        __atomic_load_n(&p4_online_hart_mask, __ATOMIC_ACQUIRE);

    return (unsigned int)((online_mask & 1u) != 0u) +
           (unsigned int)((online_mask & 2u) != 0u);
}

int krnP4PublishSecondaryOnlineAfterHandshake(int handshake_complete)
{
    if (!handshake_complete || P4_CPU_MASKS_CURRENT_HART() != 0)
        return 0;

    if (__atomic_load_n(&p4_online_hart_mask, __ATOMIC_ACQUIRE) != 1u)
        return 0;

    __atomic_store_n(&p4_online_hart_mask, 3u, __ATOMIC_RELEASE);
    return 1;
}

#if defined(P4_CPU_MASKS_TEST)
uint32_t p4CpuMasksTestGetOnlineMask(void)
{
    return __atomic_load_n(&p4_online_hart_mask, __ATOMIC_ACQUIRE);
}
#endif
