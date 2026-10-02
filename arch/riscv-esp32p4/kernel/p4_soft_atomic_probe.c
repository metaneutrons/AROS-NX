/*
 * Private two-hart qualification of cached-PSRAM operations through the
 * internal-SRAM software-atomic lock. No Exec, allocation, driver, or console
 * calls are made here. Cross-hart publication uses uncached SRAM aliases;
 * every PSRAM operand access goes through krnP4SoftAtomicOperate().
 */
#include <stddef.h>
#include <stdint.h>

#include "hardware.h"
#include "psram.h"
#include "p4_soft_atomic.h"
#include "p4_soft_atomic_probe.h"
#include "secondary_hw.h"

#if defined(__GNUC__) && !defined(__clang__)
/* Keep the diagnostic resident and bounded in direct unoptimized builds. */
#pragma GCC optimize ("Os", "no-jump-tables")
#endif

#define P4_SOFT_ATOMIC_PROBE_CODE \
    __attribute__((section(".sramtext"), noinline, used))
#define P4_SOFT_ATOMIC_PROBE_DATA \
    __attribute__((section(".sramdata"), used))
#define INLINE static inline __attribute__((always_inline))

#define PROBE_SCRATCH_BYTES       4096u
#define PROBE_OPERAND_BYTES       64u
#define PROBE_OPERATIONS          4096u
#define PROBE_WAIT_ITERATIONS     2000000u
#define PROBE_WAIT_CYCLES         360000000u
#define PROBE_SERVICE_ATTEMPTS    2000000u
#define PROBE_SERVICE_CYCLES      360000000u
#define PROBE_EXPECTED_SUM        33550336u

#define PROBE_COUNTER_OFFSET      0u
#define PROBE_GUARD0_OFFSET       4u
#define PROBE_GUARD1_OFFSET       8u
#define PROBE_WIDTH_OFFSET        12u
#define PROBE_WRAP_OFFSET         16u
#define PROBE_GUARD_FIRST_OFFSET  20u
#define PROBE_GUARD_PATTERN       0xc0de0000u
#define PROBE_WIDTH_WORD_INITIAL  0xaabbccddu
#define PROBE_WRAP_WORD_INITIAL  0x55667788u

enum p4_soft_atomic_probe_error
{
    PROBE_ERROR_CONTEXT = 1,
    PROBE_ERROR_WAIT,
    PROBE_ERROR_SERVICE,
    PROBE_ERROR_WIDTH,
    PROBE_ERROR_REFUSAL,
    PROBE_ERROR_GUARD,
    PROBE_ERROR_PROGRESS,
    PROBE_ERROR_SUM
};

struct p4_soft_atomic_probe_lane
{
    volatile uint32_t started;
    volatile uint32_t first_update;
    volatile uint32_t completed;
    volatile uint32_t iterations;
    volatile uint32_t failure;
    volatile uint32_t old_sum;
    volatile uint32_t width_pass;
    volatile uint32_t refusal_pass;
    volatile uint32_t guard_pass;
    volatile uint32_t scratch_begin;
    uint32_t reserved[6];
};

struct p4_soft_atomic_probe_publication
{
    struct p4_soft_atomic_probe_lane lane[2];
    volatile uint32_t go;
    uint32_t go_padding[15];
} __attribute__((aligned(64)));

typedef char p4_soft_atomic_probe_lane_must_be_64_bytes[
    sizeof(struct p4_soft_atomic_probe_lane) == 64u ? 1 : -1];
typedef char p4_soft_atomic_probe_go_must_be_cacheline_aligned[
    (offsetof(struct p4_soft_atomic_probe_publication, go) & 63u) == 0 ? 1 : -1];

enum p4_soft_atomic_probe_phase
{
    PROBE_PHASE_EMPTY = 0,
    PROBE_PHASE_ACTIVE,
    PROBE_PHASE_CHECKED,
    PROBE_PHASE_FAILED
};

static struct p4_soft_atomic_probe_publication probe_publication
    P4_SOFT_ATOMIC_PROBE_DATA __attribute__((aligned(64)));
static struct p4_soft_atomic_policy probe_policy P4_SOFT_ATOMIC_PROBE_DATA;
static uintptr_t probe_policy_scratch P4_SOFT_ATOMIC_PROBE_DATA;
static uint32_t probe_phase P4_SOFT_ATOMIC_PROBE_DATA;

extern unsigned long __esp32p4_psram_size;
extern void krnP4CacheSyncData(void *addr, unsigned long len);

INLINE void probe_fence(void)
{
    __asm__ volatile("fence iorw,iorw" ::: "memory");
}

INLINE uint32_t probe_cycle_now(void)
{
    uint32_t cycles;

    __asm__ volatile("csrr %0,mcycle" : "=r"(cycles));
    return cycles;
}

INLINE uint32_t probe_hart_id(void)
{
    uint32_t hart;

    __asm__ volatile("csrr %0,mhartid" : "=r"(hart));
    return hart;
}

INLINE uint32_t probe_mstatus(void)
{
    uint32_t status;

    __asm__ volatile("csrr %0,mstatus" : "=r"(status));
    return status;
}

INLINE volatile struct p4_soft_atomic_probe_publication *
probe_publication_uncached(void)
{
    uintptr_t address = (uintptr_t)&probe_publication;

    __asm__ volatile("add %0,%1,%2" : "=r"(address)
                     : "r"(address),
                       "r"((uintptr_t)P4_SECONDARY_UNCACHED_OFFSET));
    return (volatile struct p4_soft_atomic_probe_publication *)address;
}

INLINE volatile struct p4_soft_atomic_probe_lane *probe_lane(uint32_t hart)
{
    return &probe_publication_uncached()->lane[hart];
}

INLINE volatile uint32_t *probe_go(void)
{
    return &probe_publication_uncached()->go;
}

INLINE volatile void *probe_target(uintptr_t base, uint32_t offset)
{
    return (volatile void *)(base + (uintptr_t)offset);
}

INLINE uint32_t probe_guard_value(uint32_t offset)
{
    return PROBE_GUARD_PATTERN ^ offset;
}

INLINE int probe_context_valid(uint32_t expected_hart)
{
    return probe_hart_id() == expected_hart &&
           (probe_mstatus() & 8u) == 0;
}

INLINE int probe_scratch_valid(uintptr_t begin)
{
    uintptr_t psram_begin = (uintptr_t)P4_PSRAM_WINDOW_BASE;
    uintptr_t framebuffer = (uintptr_t)P4_FB_BASE;
    uintptr_t detected_size = (uintptr_t)__esp32p4_psram_size;
    uintptr_t psram_end;

    if (framebuffer < PROBE_SCRATCH_BYTES ||
        begin != framebuffer - PROBE_SCRATCH_BYTES ||
        begin < psram_begin ||
        detected_size < (uintptr_t)PROBE_SCRATCH_BYTES ||
        detected_size > UINTPTR_MAX - psram_begin)
        return 0;

    psram_end = psram_begin + detected_size;
    if (begin > UINTPTR_MAX - (uintptr_t)PROBE_SCRATCH_BYTES ||
        begin + (uintptr_t)PROBE_SCRATCH_BYTES > psram_end ||
        begin + (uintptr_t)PROBE_SCRATCH_BYTES != framebuffer)
        return 0;

    return 1;
}

INLINE int probe_wait_word(volatile uint32_t *word, uint32_t expected,
                           uint32_t peer)
{
    uint32_t start = probe_cycle_now();
    uint32_t left = PROBE_WAIT_ITERATIONS;

    do
    {
        uint32_t value = *word;

        probe_fence();
        if (value == expected)
            return 1;
        if (probe_lane(peer)->failure != 0 || *probe_go() == 2u)
            return 0;
    } while (--left &&
             (uint32_t)(probe_cycle_now() - start) < PROBE_WAIT_CYCLES);

    return 0;
}

INLINE int probe_operate(uintptr_t base, uint32_t offset, uint32_t width,
                         enum p4_soft_atomic_operation operation,
                         uint32_t operand, uint32_t *old_value)
{
    return krnP4SoftAtomicOperate(probe_target(base, offset), width,
                                  operation, operand, old_value) ==
           P4_SOFT_ATOMIC_OK;
}

INLINE int probe_expect(uintptr_t base, uint32_t offset, uint32_t width,
                        enum p4_soft_atomic_operation operation,
                        uint32_t operand, uint32_t expected_old)
{
    uint32_t old_value = 0xdecafbadU;

    return probe_operate(base, offset, width, operation, operand, &old_value) &&
           old_value == expected_old;
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_store32(uintptr_t base,
                                                    uint32_t offset,
                                                    uint32_t value)
{
    uint32_t old_value;

    return probe_operate(base, offset, 4u, P4_SOFT_ATOMIC_STORE, value,
                         &old_value);
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_load32(uintptr_t base,
                                                   uint32_t offset,
                                                   uint32_t expected)
{
    uint32_t value = 0xdecafbadU;

    return probe_operate(base, offset, 4u, P4_SOFT_ATOMIC_LOAD, 0u, &value) &&
           value == expected;
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_initialize_operands(uintptr_t base)
{
    uint32_t offset;

    for (offset = 0; offset < PROBE_OPERAND_BYTES; offset += 4u)
    {
        uint32_t initial;

        if (offset == PROBE_COUNTER_OFFSET)
            initial = 0u;
        else if (offset == PROBE_WIDTH_OFFSET)
            initial = PROBE_WIDTH_WORD_INITIAL;
        else if (offset == PROBE_WRAP_OFFSET)
            initial = PROBE_WRAP_WORD_INITIAL;
        else
            initial = probe_guard_value(offset);

        if (!probe_store32(base, offset, initial))
            return 0;
    }

    probe_fence();
    return 1;
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_width_operations(uintptr_t base)
{
    if (!probe_expect(base, PROBE_WIDTH_OFFSET, 1u, P4_SOFT_ATOMIC_STORE,
                      0x1ffu, 0xddu) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET, 1u, P4_SOFT_ATOMIC_ADD,
                      2u, 0xffu) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET, 1u, P4_SOFT_ATOMIC_OR,
                      0x82u, 1u) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET, 1u, P4_SOFT_ATOMIC_AND,
                      0x3fu, 0x83u) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET, 1u, P4_SOFT_ATOMIC_LOAD,
                      0u, 3u) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET + 1u, 1u,
                      P4_SOFT_ATOMIC_LOAD, 0u, 0xccu))
        return 0;

    if (!probe_expect(base, PROBE_WIDTH_OFFSET + 2u, 2u,
                      P4_SOFT_ATOMIC_STORE, 0x1fffeu, 0xaabbu) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET + 2u, 2u,
                      P4_SOFT_ATOMIC_ADD, 5u, 0xfffeu) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET + 2u, 2u,
                      P4_SOFT_ATOMIC_OR, 0xa200u, 3u) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET + 2u, 2u,
                      P4_SOFT_ATOMIC_AND, 0x00ffu, 0xa203u) ||
        !probe_expect(base, PROBE_WIDTH_OFFSET + 2u, 2u,
                      P4_SOFT_ATOMIC_LOAD, 0u, 3u))
        return 0;

    if (!probe_expect(base, PROBE_WRAP_OFFSET, 4u, P4_SOFT_ATOMIC_STORE,
                      0xfffffffeu, PROBE_WRAP_WORD_INITIAL) ||
        !probe_expect(base, PROBE_WRAP_OFFSET, 4u, P4_SOFT_ATOMIC_ADD,
                      5u, 0xfffffffeu) ||
        !probe_expect(base, PROBE_WRAP_OFFSET, 4u, P4_SOFT_ATOMIC_OR,
                      0x100u, 3u) ||
        !probe_expect(base, PROBE_WRAP_OFFSET, 4u, P4_SOFT_ATOMIC_AND,
                      0xffu, 0x103u) ||
        !probe_expect(base, PROBE_WRAP_OFFSET, 4u, P4_SOFT_ATOMIC_LOAD,
                      0u, 3u))
        return 0;

    probe_fence();
    return probe_load32(base, PROBE_WIDTH_OFFSET, 0x0003cc03u) &&
           probe_load32(base, PROBE_WRAP_OFFSET, 3u);
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_refusal_operations(uintptr_t base)
{
    uint32_t old_value;
    enum p4_soft_atomic_result result;
    uintptr_t uncached = base + (uintptr_t)P4_SECONDARY_UNCACHED_OFFSET;

    old_value = 0xdecafbadU;
    result = krnP4SoftAtomicOperate(probe_target(base, PROBE_OPERAND_BYTES),
                                    4u, P4_SOFT_ATOMIC_ADD, 1u, &old_value);
    if (result != P4_SOFT_ATOMIC_BAD_ADDRESS || old_value != 0xdecafbadU)
        return 0;

    old_value = 0xdecafbadU;
    result = krnP4SoftAtomicOperate((volatile void *)uncached, 4u,
                                    P4_SOFT_ATOMIC_ADD, 1u, &old_value);
    if (result != P4_SOFT_ATOMIC_BAD_ADDRESS || old_value != 0xdecafbadU)
        return 0;

    old_value = 0xdecafbadU;
    result = krnP4SoftAtomicOperate(probe_target(base, 1u), 4u,
                                    P4_SOFT_ATOMIC_ADD, 1u, &old_value);
    if (result != P4_SOFT_ATOMIC_BAD_ALIGNMENT ||
        old_value != 0xdecafbadU)
        return 0;

    old_value = 0xdecafbadU;
    result = krnP4SoftAtomicOperate(
        (volatile void *)(UINTPTR_MAX - (uintptr_t)3u), 4u,
        P4_SOFT_ATOMIC_ADD, 1u, &old_value);
    if (result != P4_SOFT_ATOMIC_BAD_ADDRESS || old_value != 0xdecafbadU)
        return 0;

    result = krnP4SoftAtomicOperate(probe_target(base, PROBE_COUNTER_OFFSET),
                                    4u, P4_SOFT_ATOMIC_ADD, 1u,
                                    (uint32_t *)probe_target(
                                        base, PROBE_COUNTER_OFFSET));
    if (result != P4_SOFT_ATOMIC_BAD_ARGUMENT)
        return 0;

    probe_fence();
    return probe_load32(base, PROBE_COUNTER_OFFSET, 0u);
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_guards_valid(uintptr_t base)
{
    uint32_t offset;

    if (!probe_load32(base, PROBE_GUARD0_OFFSET,
                      probe_guard_value(PROBE_GUARD0_OFFSET)) ||
        !probe_load32(base, PROBE_GUARD1_OFFSET,
                      probe_guard_value(PROBE_GUARD1_OFFSET)))
        return 0;

    for (offset = PROBE_GUARD_FIRST_OFFSET;
         offset < PROBE_OPERAND_BYTES; offset += 4u)
        if (!probe_load32(base, offset, probe_guard_value(offset)))
            return 0;

    return 1;
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_fail(
    uint32_t hart, uint32_t reason)
{
    volatile struct p4_soft_atomic_probe_lane *lane = probe_lane(hart);

    lane->failure = reason;
    probe_fence();
    if (hart == 0u)
    {
        *probe_go() = 2u;
        probe_fence();
    }
    return 0;
}

static P4_SOFT_ATOMIC_PROBE_CODE int probe_prepare_operands(uintptr_t base)
{
    return probe_initialize_operands(base) &&
           probe_width_operations(base) &&
           probe_refusal_operations(base) &&
           probe_guards_valid(base);
}

P4_SOFT_ATOMIC_PROBE_CODE int
krnP4SoftAtomicProbePrepare(uintptr_t scratch_begin)
{
    uint32_t phase;
    uint32_t actual_hart, status;
    volatile struct p4_soft_atomic_probe_publication *publication;
    uint32_t hart;

    __asm__ volatile("csrr %0,mhartid\ncsrr %1,mstatus"
                     : "=r"(actual_hart), "=r"(status));
    if (actual_hart != 0u || (status & 8u) != 0u ||
        !probe_scratch_valid(scratch_begin))
        return 0;

    phase = probe_phase;
    if (phase == PROBE_PHASE_EMPTY)
    {
        probe_policy.regions[0].begin = scratch_begin;
        probe_policy.regions[0].end = scratch_begin + PROBE_OPERAND_BYTES;
        probe_policy.regions[0].kind = P4_SOFT_ATOMIC_REGION_PSRAM;
        probe_policy.region_count = 1u;
        probe_policy.max_attempts = PROBE_SERVICE_ATTEMPTS;
        probe_policy.max_cycles = PROBE_SERVICE_CYCLES;

        if (krnP4SoftAtomicPrepare(&probe_policy) != P4_SOFT_ATOMIC_OK)
        {
            probe_phase = PROBE_PHASE_FAILED;
            return 0;
        }
        probe_policy_scratch = scratch_begin;
    }
    else if (phase != PROBE_PHASE_CHECKED ||
             scratch_begin != probe_policy_scratch ||
             probe_policy.regions[0].begin != scratch_begin ||
             probe_policy.regions[0].end !=
                 scratch_begin + PROBE_OPERAND_BYTES ||
             probe_policy.regions[0].kind != P4_SOFT_ATOMIC_REGION_PSRAM ||
             probe_policy.region_count != 1u ||
             probe_policy.max_attempts != PROBE_SERVICE_ATTEMPTS ||
             probe_policy.max_cycles != PROBE_SERVICE_CYCLES)
        return 0;

    krnP4CacheSyncData(&probe_publication, sizeof probe_publication);
    publication = probe_publication_uncached();
    for (hart = 0; hart < 2u; ++hart)
    {
        volatile struct p4_soft_atomic_probe_lane *lane =
            &publication->lane[hart];
        uint32_t i;

        lane->started = 0u;
        lane->first_update = 0u;
        lane->completed = 0u;
        lane->iterations = 0u;
        lane->failure = 0u;
        lane->old_sum = 0u;
        lane->width_pass = 0u;
        lane->refusal_pass = 0u;
        lane->guard_pass = 0u;
        lane->scratch_begin = (uint32_t)scratch_begin;
        for (i = 0; i < 6u; ++i)
            lane->reserved[i] = 0u;
    }
    publication->go = 0u;
    for (hart = 0; hart < 15u; ++hart)
        publication->go_padding[hart] = 0u;
    probe_fence();

    if (!probe_prepare_operands(scratch_begin))
    {
        probe_phase = PROBE_PHASE_FAILED;
        return 0;
    }

    probe_phase = PROBE_PHASE_ACTIVE;
    return 1;
}

P4_SOFT_ATOMIC_PROBE_CODE int
krnP4SoftAtomicProbeExercise(uint32_t hart)
{
    volatile struct p4_soft_atomic_probe_lane *lane;
    volatile struct p4_soft_atomic_probe_lane *peer;
    volatile uint32_t *go;
    uint32_t actual_hart, status, base, i, old_value, old_sum = 0u;

    if (hart > 1u)
        return 0;

    lane = probe_lane(hart);
    peer = probe_lane(hart ^ 1u);
    go = probe_go();
    __asm__ volatile("csrr %0,mhartid\ncsrr %1,mstatus"
                     : "=r"(actual_hart), "=r"(status));
    if (actual_hart != hart || (status & 8u) != 0u)
        return probe_fail(hart, PROBE_ERROR_CONTEXT);

    base = lane->scratch_begin;
    if (base == 0u)
        return probe_fail(hart, PROBE_ERROR_CONTEXT);

    lane->started = 1u;
    probe_fence();

    if (hart == 1u)
    {
        if (!probe_wait_word(go, 1u, 0u))
            return probe_fail(hart, PROBE_ERROR_WAIT);
    }
    else
    {
        if (!probe_wait_word(&peer->started, 1u, 1u))
            return probe_fail(hart, PROBE_ERROR_WAIT);

        /* E2's payload phase may overwrite the scratch between Prepare and
           this command. Hart 1 is waiting at GO while hart 0 restores and
           verifies every operand through the service. */
        if (!probe_initialize_operands((uintptr_t)base))
            return probe_fail(hart, PROBE_ERROR_SERVICE);
        if (!probe_width_operations((uintptr_t)base))
            return probe_fail(hart, PROBE_ERROR_WIDTH);
        lane->width_pass = 1u;

        if (!probe_refusal_operations((uintptr_t)base))
            return probe_fail(hart, PROBE_ERROR_REFUSAL);
        lane->refusal_pass = 1u;

        if (!probe_guards_valid((uintptr_t)base))
            return probe_fail(hart, PROBE_ERROR_GUARD);
        lane->guard_pass = 1u;

        probe_fence();
        *go = 1u;
        probe_fence();
    }

    for (i = 0u; i < PROBE_OPERATIONS; ++i)
    {
        if (!probe_operate((uintptr_t)base, PROBE_COUNTER_OFFSET, 4u,
                           P4_SOFT_ATOMIC_ADD, 1u, &old_value))
            return probe_fail(hart, PROBE_ERROR_SERVICE);

        old_sum += old_value;
        lane->iterations = i + 1u;
        probe_fence();
        if (i == 0u)
        {
            lane->first_update = 1u;
            probe_fence();
            if (!probe_wait_word(&peer->first_update, 1u, hart ^ 1u))
                return probe_fail(hart, PROBE_ERROR_PROGRESS);
        }
    }

    lane->old_sum = old_sum;
    probe_fence();
    lane->completed = 1u;
    probe_fence();
    return 1;
}

P4_SOFT_ATOMIC_PROBE_CODE int krnP4SoftAtomicProbeCheck(void)
{
    volatile struct p4_soft_atomic_probe_lane *lane0 = probe_lane(0u);
    volatile struct p4_soft_atomic_probe_lane *lane1 = probe_lane(1u);
    uint32_t base, total_sum, final_count;

    if (!probe_context_valid(0u))
        return 0;
    if (probe_phase == PROBE_PHASE_CHECKED)
        return 1;
    if (probe_phase != PROBE_PHASE_ACTIVE || *probe_go() != 1u ||
        lane0->started != 1u || lane1->started != 1u ||
        lane0->completed != 1u || lane1->completed != 1u ||
        lane0->failure != 0u || lane1->failure != 0u ||
        lane0->first_update != 1u || lane1->first_update != 1u ||
        lane0->iterations != PROBE_OPERATIONS ||
        lane1->iterations != PROBE_OPERATIONS ||
        lane0->width_pass != 1u || lane0->refusal_pass != 1u ||
        lane0->guard_pass != 1u)
    {
        probe_phase = PROBE_PHASE_FAILED;
        return 0;
    }

    probe_fence();
    total_sum = lane0->old_sum + lane1->old_sum;
    if (total_sum != PROBE_EXPECTED_SUM)
    {
        probe_phase = PROBE_PHASE_FAILED;
        return 0;
    }

    base = lane0->scratch_begin;
    if (base == 0u || base != lane1->scratch_begin ||
        !probe_load32((uintptr_t)base, PROBE_COUNTER_OFFSET,
                      2u * PROBE_OPERATIONS) ||
        !probe_load32((uintptr_t)base, PROBE_WIDTH_OFFSET, 0x0003cc03u) ||
        !probe_load32((uintptr_t)base, PROBE_WRAP_OFFSET, 3u) ||
        !probe_guards_valid((uintptr_t)base))
    {
        probe_phase = PROBE_PHASE_FAILED;
        return 0;
    }

    final_count = 0xdecafbadU;
    if (!probe_operate((uintptr_t)base, PROBE_COUNTER_OFFSET, 4u,
                       P4_SOFT_ATOMIC_LOAD, 0u, &final_count) ||
        final_count != 2u * PROBE_OPERATIONS)
    {
        probe_phase = PROBE_PHASE_FAILED;
        return 0;
    }

    probe_fence();
    probe_phase = PROBE_PHASE_CHECKED;
    return 1;
}
