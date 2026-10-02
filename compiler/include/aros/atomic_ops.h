#ifndef AROS_ATOMIC_OPS_H
#define AROS_ATOMIC_OPS_H

/* Optional, immutable kernel atomic service behind KATTR_AtomicOps.
 * All simultaneous accesses to an operand must use the same service.
 * No compatibility with mixed ordinary accesses or hardware AMOs is implied.
 * Operand widths are bytes (1, 2, 4); naturally aligned cached memory only.
 * Calls return the previous value. Invalid operands or bounded service failure
 * are fatal: returning an invented value would corrupt caller synchronization.
 * The table and its function addresses remain valid for the kernel lifetime.
 */
#include <stdint.h>

#define KRN_ATOMIC_OPS_VERSION 1u
#define KRN_ATOMIC_OP_LOAD 0u
#define KRN_ATOMIC_OP_STORE 1u
#define KRN_ATOMIC_OP_ADD 2u
#define KRN_ATOMIC_OP_AND 3u
#define KRN_ATOMIC_OP_OR 4u

struct KrnAtomicOps
{
    uint32_t version;
    uint32_t size;
    uint32_t (*operate)(volatile void *address, uint32_t width,
                        uint32_t operation, uint32_t operand);
    uint32_t (*compare_exchange)(volatile void *address, uint32_t width,
                                 uint32_t expected, uint32_t desired);
};

#endif
