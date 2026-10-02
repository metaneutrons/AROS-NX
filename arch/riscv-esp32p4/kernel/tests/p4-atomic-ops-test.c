/* Checked public bridge contract, not physical coherence qualification. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#define P4_PUBLIC_SPINLOCK_TEST 1
#include "../p4_atomic_ops.c"

static jmp_buf fatal_jump;
static unsigned checks, calls, fatal_calls;
static enum p4_soft_atomic_result response;
static volatile void *seen_address;
static uint32_t seen_width, seen_operation, seen_operand, seen_expected;
static uint32_t observed = 0x12345678u;

enum p4_soft_atomic_result krnP4SoftAtomicOperate(volatile void *address,
    uint32_t width, enum p4_soft_atomic_operation operation,
    uint32_t operand, uint32_t *old_value)
{
    calls++;
    seen_address = address; seen_width = width;
    seen_operation = operation; seen_operand = operand;
    if (response == P4_SOFT_ATOMIC_OK) *old_value = observed;
    return response;
}

enum p4_soft_atomic_result krnP4SoftAtomicCompareExchange(
    volatile void *address, uint32_t width, uint32_t expected,
    uint32_t desired, uint32_t *old_value)
{
    seen_expected = expected;
    return krnP4SoftAtomicOperate(address, width, P4_SOFT_ATOMIC_STORE,
                                 desired, old_value);
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    assert(reason == P4_PUBLIC_SPIN_BAD_STATE);
    fatal_calls++;
    longjmp(fatal_jump, 1);
}

#define CHECK(condition) do { assert(condition); checks++; } while (0)

int main(void)
{
    uint32_t operand = 0;
    const struct KrnAtomicOps *ops = krnP4AtomicOps();
    CHECK(ops == krnP4AtomicOps());
    CHECK(ops->version == KRN_ATOMIC_OPS_VERSION);
    CHECK(ops->size == sizeof(*ops));
    for (uint32_t width = 1; width <= 4; width *= 2)
    {
        for (uint32_t operation = 0; operation <= KRN_ATOMIC_OP_OR;
             operation++)
        {
            CHECK(ops->operate(&operand, width, operation, 0xaabbccddu)
                  == observed);
            CHECK(seen_address == &operand && seen_width == width);
            CHECK(seen_operation == operation && seen_operand == 0xaabbccddu);
        }
        CHECK(ops->compare_exchange(&operand, width, 0x1357u, 0x2468u)
              == observed);
        CHECK(seen_expected == 0x1357u && seen_operand == 0x2468u);
    }
    CHECK(calls == 18);
    for (int error = P4_SOFT_ATOMIC_BAD_ARGUMENT;
         error <= P4_SOFT_ATOMIC_LOCK_ERROR; error++)
    {
        response = (enum p4_soft_atomic_result)error;
        if (!setjmp(fatal_jump))
        {
            (void)ops->operate(&operand, 4, KRN_ATOMIC_OP_LOAD, 0);
            assert(!"failed operation returned");
        }
        CHECK(fatal_calls == (unsigned)(error * 2 - 1));
        if (!setjmp(fatal_jump))
        {
            (void)ops->compare_exchange(&operand, 4, 0, 1);
            assert(!"failed compare/exchange returned");
        }
        CHECK(fatal_calls == (unsigned)(error * 2));
    }
    printf("p4-atomic-ops-test: %u checks passed\n", checks);
    return 0;
}
