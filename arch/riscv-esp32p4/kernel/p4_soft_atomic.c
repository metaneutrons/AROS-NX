/*
 * Private P4 software atomics for caller-approved cached memory.
 *
 * A single packed lock in internal SRAM serializes this service's operations.
 * Operand memory is accessed only through ordinary volatile 8/16/32-bit
 * loads and stores: this file never emits an AMO or LR/SC against PSRAM.
 */
#include <stddef.h>
#include <stdint.h>

#include "p4_soft_atomic.h"
#include "p4_spinlock.h"
#if !defined(P4_SOFT_ATOMIC_TEST)
#include "hardware.h"
#endif

#if defined(P4_SOFT_ATOMIC_TEST)
#ifndef P4_SOFT_ATOMIC_CODE
#define P4_SOFT_ATOMIC_CODE
#endif
#ifndef P4_SOFT_ATOMIC_DATA
#define P4_SOFT_ATOMIC_DATA
#endif
#else
#if !defined(__riscv)
#error "p4_soft_atomic.c is ESP32-P4 RISC-V code; use the host fixture for tests"
#endif
#if defined(__GNUC__) && !defined(__clang__)
/* The bounded SRAM path must stay small even in an unoptimized kernel build. */
#pragma GCC optimize ("Os", "no-jump-tables")
#endif
#define P4_SOFT_ATOMIC_CODE \
    __attribute__((section(".sramtext"), noinline, used))
#define P4_SOFT_ATOMIC_DATA \
    __attribute__((section(".sramdata"), used))
#endif

#define P4_SOFT_ATOMIC_READY_EMPTY 0u
#define P4_SOFT_ATOMIC_READY_INIT  1u
#define P4_SOFT_ATOMIC_READY       2u
#define P4_SOFT_ATOMIC_MIE         8u
#define P4_SOFT_ATOMIC_CYCLE_MAX   0x7fffffffu

/*
 * Low SRAM uses the fixed data/code window. High SRAM requires its separate
 * region kind and validation against the live L2 carve-out at preparation.
 * The caller's exact extents still exclude image-resident and
 * otherwise unowned bytes. PSRAM uses its canonical cached 64-MiB aperture;
 * exact caller extents must exclude framebuffer/device ownership.
 */
#define P4_SOFT_ATOMIC_SRAM_BASE  ((uintptr_t)0x4ff00000u)
#define P4_SOFT_ATOMIC_SRAM_END   ((uintptr_t)0x4ff3ffc0u)
#define P4_SOFT_ATOMIC_PSRAM_BASE ((uintptr_t)0x48000000u)
#define P4_SOFT_ATOMIC_PSRAM_END  ((uintptr_t)0x4c000000u)

#ifndef P4_SOFT_ATOMIC_HIGH_CONFIG_READ
#if defined(P4_SOFT_ATOMIC_TEST)
/* Off-target fixtures must opt in explicitly to a high-bank configuration. */
#define P4_SOFT_ATOMIC_HIGH_CONFIG_READ() 2u
#else
#define P4_SOFT_ATOMIC_HIGH_CONFIG_READ() \
    (*(volatile uint32_t *)(P4_CACHE_BASE + P4_L2_CACHESIZE_CONF))
#endif
#endif

static inline __attribute__((always_inline)) uintptr_t
p4_soft_atomic_high_sram_end(void)
{
    uint32_t config = P4_SOFT_ATOMIC_HIGH_CONFIG_READ();
    uintptr_t l2_bytes;

    /* Same live sizing rule as krnRAMInit; never admit the cache reservation.
     * Snapshot only while preparing the immutable policy, not on hot accesses. */
    if (config & 2u)
        l2_bytes = 512u * 1024u;
    else if (config & 1u)
        l2_bytes = 256u * 1024u;
    else
        l2_bytes = 128u * 1024u;
    return (uintptr_t)0x4ff40000u + 0x80000u - l2_bytes;
}

/* Only this lock word is an LR/SC target, and it lives in .sramdata. */
static spinlock_t p4_soft_atomic_lock P4_SOFT_ATOMIC_DATA
    __attribute__((aligned(4)));
static struct p4_spinlock_policy p4_soft_atomic_lock_policy
    P4_SOFT_ATOMIC_DATA;
static struct p4_soft_atomic_policy p4_soft_atomic_active_policy
    P4_SOFT_ATOMIC_DATA;
static volatile uint32_t p4_soft_atomic_ready P4_SOFT_ATOMIC_DATA;

static inline __attribute__((always_inline)) int
p4_soft_atomic_region_canonical(const struct p4_soft_atomic_region *region)
{
    if (region == NULL || region->begin >= region->end)
        return 0;

    if (region->kind == P4_SOFT_ATOMIC_REGION_SRAM)
        return region->begin >= P4_SOFT_ATOMIC_SRAM_BASE &&
               region->end <= P4_SOFT_ATOMIC_SRAM_END;

    if (region->kind == P4_SOFT_ATOMIC_REGION_PSRAM)
        return region->begin >= P4_SOFT_ATOMIC_PSRAM_BASE &&
               region->end <= P4_SOFT_ATOMIC_PSRAM_END;

    if (region->kind == P4_SOFT_ATOMIC_REGION_SRAM_HIGH)
        return region->begin >= (uintptr_t)0x4ff40000u &&
               region->end <= p4_soft_atomic_high_sram_end();

    return 0;
}

#ifndef P4_SOFT_ATOMIC_REGION_ADMITTED
#define P4_SOFT_ATOMIC_REGION_ADMITTED(region) \
    p4_soft_atomic_region_canonical(region)
#endif

#ifndef P4_SOFT_ATOMIC_READY_LOAD
#define P4_SOFT_ATOMIC_READY_LOAD() \
    __atomic_load_n(&p4_soft_atomic_ready, __ATOMIC_ACQUIRE)
#endif

#ifndef P4_SOFT_ATOMIC_READY_STORE
#define P4_SOFT_ATOMIC_READY_STORE(value) \
    __atomic_store_n(&p4_soft_atomic_ready, (value), __ATOMIC_RELEASE)
#endif

#ifndef P4_SOFT_ATOMIC_IRQ_SAVE
static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_irq_save(void)
{
    uint32_t status;

    __asm__ volatile("csrrci %0,mstatus,8"
                     : "=r"(status) : : "memory");
    return status & P4_SOFT_ATOMIC_MIE;
}
#define P4_SOFT_ATOMIC_IRQ_SAVE() p4_soft_atomic_irq_save()
#endif

#ifndef P4_SOFT_ATOMIC_IRQ_RESTORE
static inline __attribute__((always_inline)) void
p4_soft_atomic_irq_restore(uint32_t saved_mie)
{
    if (saved_mie & P4_SOFT_ATOMIC_MIE)
        __asm__ volatile("csrsi mstatus,8" : : : "memory");
}
#define P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie) \
    p4_soft_atomic_irq_restore(saved_mie)
#endif

#ifndef P4_SOFT_ATOMIC_HART_ID
static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_hart_id(void)
{
    uint32_t hart;

    __asm__ volatile("csrr %0,mhartid" : "=r"(hart));
    return hart;
}
#define P4_SOFT_ATOMIC_HART_ID() p4_soft_atomic_hart_id()
#endif

#ifndef P4_SOFT_ATOMIC_CYCLE_NOW
static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_cycle_now(void)
{
    uint32_t cycles;

    __asm__ volatile("csrr %0,mcycle" : "=r"(cycles));
    return cycles;
}
#define P4_SOFT_ATOMIC_CYCLE_NOW() p4_soft_atomic_cycle_now()
#endif

#ifndef P4_SOFT_ATOMIC_FULL_FENCE
static inline __attribute__((always_inline)) void
p4_soft_atomic_full_fence(void)
{
    __asm__ volatile("fence iorw,iorw" : : : "memory");
}
#define P4_SOFT_ATOMIC_FULL_FENCE() p4_soft_atomic_full_fence()
#endif

#ifndef P4_SOFT_ATOMIC_ADDRESS_ADMITTED
#define P4_SOFT_ATOMIC_ADDRESS_ADMITTED(address, width) \
    p4_soft_atomic_address_allowed((uintptr_t)(address), (width))
#endif

#if !defined(P4_SOFT_ATOMIC_TEST)
static P4_SOFT_ATOMIC_CODE __attribute__((noreturn))
void p4_soft_atomic_fatal(enum p4_soft_atomic_result reason)
{
    (void)reason;
    /* Release failure is fail-closed: MIE stays clear and this never returns. */
    for (;;)
        __asm__ volatile("nop" : : : "memory");
}
#ifndef P4_SOFT_ATOMIC_FATAL
#define P4_SOFT_ATOMIC_FATAL(reason) p4_soft_atomic_fatal(reason)
#endif
#endif

static inline __attribute__((always_inline)) int
p4_soft_atomic_span(uintptr_t begin, uint32_t bytes, uintptr_t *end)
{
    if (begin > UINTPTR_MAX - (uintptr_t)bytes)
        return 0;
    *end = begin + (uintptr_t)bytes;
    return 1;
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_policy_valid(const struct p4_soft_atomic_policy *policy)
{
    uint32_t i, j;

    if (policy == NULL || policy->region_count == 0 ||
        policy->region_count > P4_SOFT_ATOMIC_MAX_REGIONS ||
        policy->max_attempts == 0 || policy->max_cycles == 0 ||
        policy->max_cycles > P4_SOFT_ATOMIC_CYCLE_MAX)
        return 0;

    for (i = 0; i < policy->region_count; ++i)
    {
        const struct p4_soft_atomic_region *region = &policy->regions[i];

        if (!P4_SOFT_ATOMIC_REGION_ADMITTED(region))
            return 0;

        for (j = 0; j < i; ++j)
        {
            const struct p4_soft_atomic_region *previous =
                &policy->regions[j];

            if (region->begin < previous->end &&
                previous->begin < region->end)
                return 0;
        }
    }

    return 1;
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_target_allowed(uintptr_t address, uint32_t width)
{
    uintptr_t end;
    uint32_t i;

    if ((width != 1u && width != 2u && width != 4u) ||
        (address & (uintptr_t)(width - 1u)) != 0 ||
        !p4_soft_atomic_span(address, width, &end))
        return 0;

    if (P4_SOFT_ATOMIC_READY_LOAD() != P4_SOFT_ATOMIC_READY)
        return 0;

    for (i = 0; i < p4_soft_atomic_active_policy.region_count; ++i)
    {
        const struct p4_soft_atomic_region *region =
            &p4_soft_atomic_active_policy.regions[i];

        if (address >= region->begin && end <= region->end)
            return 1;
    }

    return 0;
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_address_allowed(uintptr_t address, uint32_t width)
{
    return p4_soft_atomic_target_allowed(address, width);
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_policy_copy(const struct p4_soft_atomic_policy *source)
{
    uint32_t i;

    if (source->region_count > P4_SOFT_ATOMIC_MAX_REGIONS)
        return 0;

    p4_soft_atomic_active_policy.region_count = source->region_count;
    p4_soft_atomic_active_policy.max_attempts = source->max_attempts;
    p4_soft_atomic_active_policy.max_cycles = source->max_cycles;
    for (i = 0; i < P4_SOFT_ATOMIC_MAX_REGIONS; ++i)
    {
        if (i < source->region_count)
        {
            p4_soft_atomic_active_policy.regions[i].begin =
                source->regions[i].begin;
            p4_soft_atomic_active_policy.regions[i].end =
                source->regions[i].end;
            p4_soft_atomic_active_policy.regions[i].kind =
                source->regions[i].kind;
        }
        else
        {
            p4_soft_atomic_active_policy.regions[i].begin = 0;
            p4_soft_atomic_active_policy.regions[i].end = 0;
            p4_soft_atomic_active_policy.regions[i].kind = 0;
        }
    }

    return 1;
}

static inline __attribute__((always_inline)) void
p4_soft_atomic_lock_policy_init(void)
{
    const uintptr_t begin = (uintptr_t)&p4_soft_atomic_lock;

    p4_soft_atomic_lock_policy.regions[0].begin = begin;
    p4_soft_atomic_lock_policy.regions[0].end =
        begin + sizeof(p4_soft_atomic_lock);
    p4_soft_atomic_lock_policy.regions[1].begin = 0;
    p4_soft_atomic_lock_policy.regions[1].end = 0;
    p4_soft_atomic_lock_policy.region_count = 1;
}

P4_SOFT_ATOMIC_CODE enum p4_soft_atomic_result
krnP4SoftAtomicPrepare(const struct p4_soft_atomic_policy *policy)
{
    enum p4_spinlock_result spin_result;
    uint32_t saved_mie;

    if (policy == NULL)
        return P4_SOFT_ATOMIC_BAD_ARGUMENT;
    if (((uintptr_t)policy &
         ((uintptr_t)_Alignof(struct p4_soft_atomic_policy) - 1u)) != 0)
        return P4_SOFT_ATOMIC_BAD_ARGUMENT;

    saved_mie = P4_SOFT_ATOMIC_IRQ_SAVE();
    if (P4_SOFT_ATOMIC_HART_ID() != 0 || saved_mie != 0)
    {
        P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
        return P4_SOFT_ATOMIC_BAD_CONTEXT;
    }

    if (P4_SOFT_ATOMIC_READY_LOAD() != P4_SOFT_ATOMIC_READY_EMPTY)
    {
        P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
        return P4_SOFT_ATOMIC_ALREADY_PREPARED;
    }

    if (!p4_soft_atomic_policy_valid(policy))
    {
        P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
        return P4_SOFT_ATOMIC_BAD_POLICY;
    }

    P4_SOFT_ATOMIC_READY_STORE(P4_SOFT_ATOMIC_READY_INIT);
    (void)p4_soft_atomic_policy_copy(policy);
    p4_soft_atomic_lock_policy_init();

    spin_result = krnP4SpinInit(&p4_soft_atomic_lock,
                                &p4_soft_atomic_lock_policy);
    if (spin_result != P4_SPIN_OK)
    {
        P4_SOFT_ATOMIC_READY_STORE(P4_SOFT_ATOMIC_READY_EMPTY);
        P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
        return P4_SOFT_ATOMIC_LOCK_ERROR;
    }

    P4_SOFT_ATOMIC_FULL_FENCE();
    P4_SOFT_ATOMIC_READY_STORE(P4_SOFT_ATOMIC_READY);
    P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
    return P4_SOFT_ATOMIC_OK;
}

P4_SOFT_ATOMIC_CODE int krnP4SoftAtomicAddressAllowed(
    const volatile void *address, uint32_t width)
{
    if (address == NULL)
        return 0;

    return P4_SOFT_ATOMIC_ADDRESS_ADMITTED(address, width);
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_operation_valid(enum p4_soft_atomic_operation operation)
{
    return operation == P4_SOFT_ATOMIC_LOAD ||
           operation == P4_SOFT_ATOMIC_STORE ||
           operation == P4_SOFT_ATOMIC_ADD ||
           operation == P4_SOFT_ATOMIC_AND ||
           operation == P4_SOFT_ATOMIC_OR;
}

static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_read_target(volatile void *address, uint32_t width)
{
    if (width == 1u)
        return *(volatile uint8_t *)address;
    if (width == 2u)
        return *(volatile uint16_t *)address;
    return *(volatile uint32_t *)address;
}

static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_width_mask(uint32_t width)
{
    return width == 1u ? 0xffu :
           width == 2u ? 0xffffu : UINT32_MAX;
}

static inline __attribute__((always_inline)) void
p4_soft_atomic_write_target(volatile void *address, uint32_t width,
                            uint32_t value)
{
    if (width == 1u)
        *(volatile uint8_t *)address = (uint8_t)value;
    else if (width == 2u)
        *(volatile uint16_t *)address = (uint16_t)value;
    else
        *(volatile uint32_t *)address = value;
}

static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_apply_target(volatile void *address, uint32_t width,
                            enum p4_soft_atomic_operation operation,
                            uint32_t operand)
{
    uint32_t old_value = p4_soft_atomic_read_target(address, width);
    uint32_t mask = p4_soft_atomic_width_mask(width);
    uint32_t new_value = old_value;

    if (operation == P4_SOFT_ATOMIC_STORE)
        new_value = operand;
    else if (operation == P4_SOFT_ATOMIC_ADD)
        new_value = old_value + operand;
    else if (operation == P4_SOFT_ATOMIC_AND)
        new_value = old_value & operand;
    else if (operation == P4_SOFT_ATOMIC_OR)
        new_value = old_value | operand;

    if (operation != P4_SOFT_ATOMIC_LOAD)
    {
        new_value &= mask;
        p4_soft_atomic_write_target(address, width, new_value);
    }

    return old_value;
}

static inline __attribute__((always_inline)) uint32_t
p4_soft_atomic_compare_exchange_target(volatile void *address, uint32_t width,
                                      uint32_t expected, uint32_t desired)
{
    uint32_t old_value = p4_soft_atomic_read_target(address, width);
    uint32_t mask = p4_soft_atomic_width_mask(width);

    if (old_value == (expected & mask))
        p4_soft_atomic_write_target(address, width, desired & mask);

    return old_value;
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_output_allowed(uintptr_t address, uintptr_t target_begin,
                             uintptr_t target_end)
{
    uintptr_t output_end;

    if (address == 0 || (address & 3u) != 0 ||
        !p4_soft_atomic_span(address, sizeof(uint32_t), &output_end))
        return 0;

    return output_end <= target_begin || target_end <= address;
}

static inline __attribute__((always_inline)) int
p4_soft_atomic_expired(uint32_t start)
{
    return (uint32_t)(P4_SOFT_ATOMIC_CYCLE_NOW() - start) >=
           p4_soft_atomic_active_policy.max_cycles;
}

static inline __attribute__((always_inline)) enum p4_soft_atomic_result
p4_soft_atomic_acquire(uint32_t *saved_mie)
{
    uint32_t attempt, start;

    *saved_mie = P4_SOFT_ATOMIC_IRQ_SAVE();
    P4_SOFT_ATOMIC_FULL_FENCE();
    start = P4_SOFT_ATOMIC_CYCLE_NOW();

    for (attempt = 0;
         attempt < p4_soft_atomic_active_policy.max_attempts;
         ++attempt)
    {
        enum p4_spinlock_result result;

        if (p4_soft_atomic_expired(start))
            break;

        result = krnP4SpinTryAcquire(&p4_soft_atomic_lock,
                                    SPINLOCK_MODE_WRITE,
                                    &p4_soft_atomic_lock_policy);
        if (result == P4_SPIN_OK)
        {
            P4_SOFT_ATOMIC_FULL_FENCE();
            return P4_SOFT_ATOMIC_OK;
        }

        if (result != P4_SPIN_BUSY)
            return P4_SOFT_ATOMIC_LOCK_ERROR;
    }

    return P4_SOFT_ATOMIC_TIMEOUT;
}

#ifdef P4_SOFT_ATOMIC_TEST
#ifndef P4_SOFT_ATOMIC_FATAL
#error "P4_SOFT_ATOMIC_TEST must supply a noreturn P4_SOFT_ATOMIC_FATAL hook"
#endif
#endif

static inline __attribute__((always_inline)) void
p4_soft_atomic_release(uint32_t saved_mie)
{
    uint32_t attempt, start;

    P4_SOFT_ATOMIC_FULL_FENCE();
    start = P4_SOFT_ATOMIC_CYCLE_NOW();

    for (attempt = 0;
         attempt < p4_soft_atomic_active_policy.max_attempts;
         ++attempt)
    {
        enum p4_spinlock_result result;

        if (p4_soft_atomic_expired(start))
            break;

        result = krnP4SpinUnlock(&p4_soft_atomic_lock,
                                 SPINLOCK_MODE_WRITE,
                                 &p4_soft_atomic_lock_policy);
        if (result == P4_SPIN_OK)
        {
            P4_SOFT_ATOMIC_FULL_FENCE();
            P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
            return;
        }

        if (result != P4_SPIN_BUSY)
            P4_SOFT_ATOMIC_FATAL(P4_SOFT_ATOMIC_LOCK_ERROR);
    }

    /* Keep local interrupts masked: continuing after a held-lock failure is
       unsafe, even if the caller's original MIE state was enabled. */
    P4_SOFT_ATOMIC_FATAL(P4_SOFT_ATOMIC_LOCK_ERROR);
}

static P4_SOFT_ATOMIC_CODE enum p4_soft_atomic_result
p4_soft_atomic_execute(volatile void *address, uint32_t width,
                       enum p4_soft_atomic_operation operation,
                       uint32_t operand, int compare_exchange,
                       uint32_t expected, uint32_t desired,
                       uint32_t *old_value)
{
    uintptr_t target_begin, target_end, output_address;
    uint32_t saved_mie, before;
    enum p4_soft_atomic_result result;

    if (address == NULL || old_value == NULL)
        return P4_SOFT_ATOMIC_BAD_ARGUMENT;
    if (!compare_exchange && !p4_soft_atomic_operation_valid(operation))
        return P4_SOFT_ATOMIC_BAD_OPERATION;
    if (width != 1u && width != 2u && width != 4u)
        return P4_SOFT_ATOMIC_BAD_WIDTH;

    target_begin = (uintptr_t)address;
    if ((target_begin & (uintptr_t)(width - 1u)) != 0)
        return P4_SOFT_ATOMIC_BAD_ALIGNMENT;
    if (!p4_soft_atomic_span(target_begin, width, &target_end))
        return P4_SOFT_ATOMIC_BAD_ADDRESS;

    output_address = (uintptr_t)old_value;
    if (!p4_soft_atomic_output_allowed(output_address, target_begin,
                                       target_end))
        return P4_SOFT_ATOMIC_BAD_ARGUMENT;

    if (P4_SOFT_ATOMIC_READY_LOAD() != P4_SOFT_ATOMIC_READY)
        return P4_SOFT_ATOMIC_NOT_PREPARED;
    if (!P4_SOFT_ATOMIC_ADDRESS_ADMITTED(address, width))
        return P4_SOFT_ATOMIC_BAD_ADDRESS;

    result = p4_soft_atomic_acquire(&saved_mie);
    if (result != P4_SOFT_ATOMIC_OK)
    {
        if (result == P4_SOFT_ATOMIC_TIMEOUT)
        {
            P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
            return result;
        }

        P4_SOFT_ATOMIC_IRQ_RESTORE(saved_mie);
        return result;
    }

    if (compare_exchange)
        before = p4_soft_atomic_compare_exchange_target(
            address, width, expected, desired);
    else
        before = p4_soft_atomic_apply_target(address, width, operation,
                                             operand);
    P4_SOFT_ATOMIC_FULL_FENCE();
    p4_soft_atomic_release(saved_mie);
    *old_value = before;
    return P4_SOFT_ATOMIC_OK;
}

P4_SOFT_ATOMIC_CODE enum p4_soft_atomic_result krnP4SoftAtomicOperate(
    volatile void *address, uint32_t width,
    enum p4_soft_atomic_operation operation, uint32_t operand,
    uint32_t *old_value)
{
    return p4_soft_atomic_execute(address, width, operation, operand, 0,
                                  0u, 0u, old_value);
}

P4_SOFT_ATOMIC_CODE enum p4_soft_atomic_result
krnP4SoftAtomicCompareExchange(volatile void *address, uint32_t width,
                               uint32_t expected, uint32_t desired,
                               uint32_t *old_value)
{
    return p4_soft_atomic_execute(address, width, P4_SOFT_ATOMIC_LOAD, 0u, 1,
                                  expected, desired, old_value);
}
