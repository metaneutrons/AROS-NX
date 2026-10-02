/* Public checked service bridge. Never execute an AMO on caller PSRAM. */
#include <aros/atomic_ops.h>
#include "p4_soft_atomic.h"
#include "p4_public_spinlock.h"

_Static_assert(KRN_ATOMIC_OP_LOAD == P4_SOFT_ATOMIC_LOAD, "load ABI");
_Static_assert(KRN_ATOMIC_OP_STORE == P4_SOFT_ATOMIC_STORE, "store ABI");
_Static_assert(KRN_ATOMIC_OP_ADD == P4_SOFT_ATOMIC_ADD, "add ABI");
_Static_assert(KRN_ATOMIC_OP_AND == P4_SOFT_ATOMIC_AND, "and ABI");
_Static_assert(KRN_ATOMIC_OP_OR == P4_SOFT_ATOMIC_OR, "or ABI");

uint32_t krnP4AtomicCheckedOperate(volatile void *address, uint32_t width,
                                 uint32_t operation, uint32_t operand)
{
    uint32_t old_value = 0;
    if (krnP4SoftAtomicOperate(address, width,
            (enum p4_soft_atomic_operation)operation, operand, &old_value)
            != P4_SOFT_ATOMIC_OK)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    return old_value;
}

uint32_t krnP4AtomicCheckedCompareExchange(volatile void *address,
        uint32_t width, uint32_t expected, uint32_t desired)
{
    uint32_t old_value = 0;
    if (krnP4SoftAtomicCompareExchange(address, width, expected, desired,
                                     &old_value) != P4_SOFT_ATOMIC_OK)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
    return old_value;
}

static const struct KrnAtomicOps p4_atomic_ops =
{
    KRN_ATOMIC_OPS_VERSION, sizeof(struct KrnAtomicOps),
    krnP4AtomicCheckedOperate, krnP4AtomicCheckedCompareExchange
};

const struct KrnAtomicOps *krnP4AtomicOps(void)
{
    return &p4_atomic_ops;
}
