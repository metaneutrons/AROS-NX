/*
 * Private bounded software atomics for explicitly admitted P4 memory.
 *
 * This is not a replacement for AROS compiler atomics and does not make
 * ordinary accesses to the same object safe. Every concurrent access to a
 * target protected here must use this API.
 */
#ifndef P4_SOFT_ATOMIC_H
#define P4_SOFT_ATOMIC_H

#include <stdint.h>

#define P4_SOFT_ATOMIC_MAX_REGIONS 4u

enum p4_soft_atomic_region_kind
{
    P4_SOFT_ATOMIC_REGION_SRAM = 1,
    P4_SOFT_ATOMIC_REGION_PSRAM = 2,
    P4_SOFT_ATOMIC_REGION_SRAM_HIGH = 3
};

struct p4_soft_atomic_region
{
    uintptr_t begin;
    uintptr_t end;
    uint32_t kind;
};

/*
 * Supply exact, immutable, caller-owned extents in readable internal SRAM.
 * SRAM ranges must exclude the dynamic L2 reservation. PSRAM ranges must be
 * physically populated, initialized, and owned by the caller (bounded by the
 * detected PSRAM size), and exclude framebuffer/device-owned extents. The
 * service accepts only canonical cached apertures, never MMIO or uncached
 * aliases. The policy is copied by Prepare and need not outlive it.
 * SRAM_HIGH additionally checks the live L2 carve-out during preparation.
 * Its exact owned bound still comes from the Data Memory MemHeader; L2 sizing
 * must remain unchanged for the lifetime of the prepared service.
 *
 * max_attempts bounds the number of single-attempt spinlock backend calls;
 * max_cycles independently bounds each acquire/release phase using mcycle.
 * max_cycles must be in 1..INT32_MAX so unsigned wraparound is unambiguous.
 */
struct p4_soft_atomic_policy
{
    struct p4_soft_atomic_region regions[P4_SOFT_ATOMIC_MAX_REGIONS];
    uint32_t region_count;
    uint32_t max_attempts;
    uint32_t max_cycles;
};

enum p4_soft_atomic_operation
{
    P4_SOFT_ATOMIC_LOAD = 0,
    P4_SOFT_ATOMIC_STORE,
    P4_SOFT_ATOMIC_ADD,
    P4_SOFT_ATOMIC_AND,
    P4_SOFT_ATOMIC_OR
};

enum p4_soft_atomic_result
{
    P4_SOFT_ATOMIC_OK = 0,
    P4_SOFT_ATOMIC_BAD_ARGUMENT,
    P4_SOFT_ATOMIC_BAD_POLICY,
    P4_SOFT_ATOMIC_BAD_CONTEXT,
    P4_SOFT_ATOMIC_ALREADY_PREPARED,
    P4_SOFT_ATOMIC_NOT_PREPARED,
    P4_SOFT_ATOMIC_BAD_OPERATION,
    P4_SOFT_ATOMIC_BAD_WIDTH,
    P4_SOFT_ATOMIC_BAD_ALIGNMENT,
    P4_SOFT_ATOMIC_BAD_ADDRESS,
    P4_SOFT_ATOMIC_TIMEOUT,
    P4_SOFT_ATOMIC_LOCK_ERROR
};

/*
 * Call exactly once on hart 0, with MIE clear and hart 1 still held in reset.
 * The implementation checks hart ID and local MIE state; reset-held status is
 * an explicit caller precondition. A second call never reinitializes the live
 * lock. Returns BAD_CONTEXT or BAD_POLICY without publishing partial state.
 */
enum p4_soft_atomic_result krnP4SoftAtomicPrepare(
    const struct p4_soft_atomic_policy *policy);

/*
 * Execute one bounded, globally ordered operation on an admitted naturally
 * aligned 8-, 16-, or 32-bit target. All operations, including STORE, return
 * the target's value before the operation through old_value. STORE/ADD/AND/OR
 * truncate operands to the selected width; ADD wraps modulo that width.
 *
 * old_value must be a valid, naturally aligned, private writable 32-bit
 * caller object; it must not overlap the target operand. It is changed only
 * on success. The target itself must be accessed exclusively through this API
 * while concurrent users exist. The service makes no claim for mixed plain
 * accesses or direct PSRAM atomics.
 */
enum p4_soft_atomic_result krnP4SoftAtomicOperate(
    volatile void *address, uint32_t width,
    enum p4_soft_atomic_operation operation, uint32_t operand,
    uint32_t *old_value);

/*
 * Compare and exchange one admitted naturally aligned 8-, 16-, or 32-bit
 * target. expected and desired are truncated to width. A value mismatch is
 * still P4_SOFT_ATOMIC_OK: old_value receives the observed value, and the
 * target is left unchanged. old_value follows the same placement and
 * unchanged-on-error rules as krnP4SoftAtomicOperate().
 */
enum p4_soft_atomic_result krnP4SoftAtomicCompareExchange(
    volatile void *address, uint32_t width, uint32_t expected,
    uint32_t desired, uint32_t *old_value);

/* Numeric check only; it never dereferences address. */
int krnP4SoftAtomicAddressAllowed(const volatile void *address,
                                 uint32_t width);

#endif /* P4_SOFT_ATOMIC_H */
