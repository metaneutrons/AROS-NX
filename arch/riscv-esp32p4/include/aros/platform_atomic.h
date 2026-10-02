#ifndef AROS_PLATFORM_ATOMIC_H
#define AROS_PLATFORM_ATOMIC_H

#include <aros/config.h>
#if defined(__AROSEXEC_SMP__)
#include <aros/atomic_ops.h>

#if defined(P4_ATOMIC_CORE)
/* Exec needs this path before resource vectors exist. Never resolve the
 * resource via OpenResource while initializing Exec's own resource lock. */
extern uint32_t krnP4AtomicCheckedOperate(volatile void *, uint32_t,
                                       uint32_t, uint32_t);
#define P4_ATOMIC_OPERATE(address, width, operation, operand) \
    krnP4AtomicCheckedOperate((address), (width), (operation), (operand))
#else
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <proto/exec.h>

static inline __attribute__((noreturn)) void p4_atomic_missing_service(void)
{
    /* Missing/mismatched kernels must not silently use local Disable(). */
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    for (;;) __asm__ volatile("wfi" ::: "memory");
}

static inline const struct KrnAtomicOps *
p4_atomic_public_service(struct ExecBase *exec_base)
{
    /* RV32 pointer load/store + fences only, never an RMW on this cache.
     * Concurrent first callers may publish the same immutable table. */
    static const struct KrnAtomicOps *cached;
    const struct KrnAtomicOps *ops =
        __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (!ops)
    {
        APTR kernel = AROS_LC1(APTR, OpenResource,
            AROS_LCA(CONST_STRPTR, (CONST_STRPTR)"kernel.resource", A1),
            struct ExecBase *, exec_base, 83, Exec);
        intptr_t value;
        if (!kernel) p4_atomic_missing_service();
        value = AROS_LC1(intptr_t, KrnGetSystemAttr,
            AROS_LCA(uint32_t, KATTR_AtomicOps, D0),
            APTR, kernel, 29, Kernel);
        if (value == 0 || value == -1) p4_atomic_missing_service();
        ops = (const struct KrnAtomicOps *)value;
        if (ops->version != KRN_ATOMIC_OPS_VERSION ||
            ops->size < sizeof(*ops) || !ops->operate ||
            !ops->compare_exchange)
            p4_atomic_missing_service();
        __atomic_store_n(&cached, ops, __ATOMIC_RELEASE);
    }
    return ops;
}

#define P4_ATOMIC_OPERATE(address, width, operation, operand) \
    (p4_atomic_public_service(__aros_getbase_SysBase())->operate( \
        (address), (width), (operation), (operand)))
#endif

#define P4_ATOMIC_UPDATE(var, operation, operand) \
    ((void)P4_ATOMIC_OPERATE(&(var), sizeof(var), (operation), \
                           (uint32_t)(operand)))
#define __AROS_ATOMIC_INC_B(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, 1)
#define __AROS_ATOMIC_INC_W(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, 1)
#define __AROS_ATOMIC_INC_L(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, 1)
#define __AROS_ATOMIC_DEC_B(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, UINT32_MAX)
#define __AROS_ATOMIC_DEC_W(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, UINT32_MAX)
#define __AROS_ATOMIC_DEC_L(var) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_ADD, UINT32_MAX)
#define __AROS_ATOMIC_AND_B(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_AND, mask)
#define __AROS_ATOMIC_AND_W(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_AND, mask)
#define __AROS_ATOMIC_AND_L(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_AND, mask)
#define __AROS_ATOMIC_OR_B(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_OR, mask)
#define __AROS_ATOMIC_OR_W(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_OR, mask)
#define __AROS_ATOMIC_OR_L(var, mask) P4_ATOMIC_UPDATE(var, KRN_ATOMIC_OP_OR, mask)

#endif /* Exec SMP; normal P4 retains its legacy atomic selection */
#endif
