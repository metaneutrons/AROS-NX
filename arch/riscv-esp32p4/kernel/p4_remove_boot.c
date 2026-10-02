/* Boot-only permanent prefix storage for task-independent removal tickets. */
#if !defined(P4_REMOVE_BOOT_TEST)
#include <exec/memory.h>
#endif

#include "p4_remove_boot.h"
#include "p4_boot_reserve.h"
#include "p4_task_registry.h"
#include "../exec/p4_remove_bank.h"

#ifndef P4_REMOVE_BOOT_CAPACITY
#define P4_REMOVE_BOOT_CAPACITY P4_TASK_REGISTRY_LIMIT
#endif

#define P4_REMOVE_BOOT_ALIGN 16u
#define P4_REMOVE_BOOT_ROUND(size) \
    (((size) + P4_REMOVE_BOOT_ALIGN - 1u) & \
     ~(size_t)(P4_REMOVE_BOOT_ALIGN - 1u))

_Static_assert(P4_REMOVE_BOOT_CAPACITY > 0u &&
    P4_REMOVE_BOOT_CAPACITY <= P4_REMOVE_BANK_MAX_SLOTS,
    "invalid removal-bank boot capacity");
#if !defined(P4_REMOVE_BOOT_TEST)
_Static_assert(P4_REMOVE_BOOT_CAPACITY >= P4_TASK_REGISTRY_LIMIT,
    "production removal bank must cover the Task registry limit");
#endif
_Static_assert((P4_REMOVE_BOOT_ALIGN & (P4_REMOVE_BOOT_ALIGN - 1u)) == 0u,
    "removal-bank boot alignment must be a power of two");
_Static_assert(_Alignof(struct p4_remove_bank_slot) <= P4_REMOVE_BOOT_ALIGN,
    "removal-bank slot alignment exceeds prefix alignment");
_Static_assert(sizeof(struct p4_remove_bank_slot) <=
    (SIZE_MAX - (P4_REMOVE_BOOT_ALIGN - 1u)) /
        (P4_REMOVE_BOOT_CAPACITY != 0u ? P4_REMOVE_BOOT_CAPACITY : 1u),
    "removal-bank boot storage size overflows");

static struct p4_remove_bank p4_remove_boot_bank;
static size_t p4_remove_boot_bytes;
static int p4_remove_boot_attempted;
static int p4_remove_boot_prepared;

#if !defined(P4_REMOVE_BOOT_TEST) || !defined(P4_REMOVE_BOOT_BANK_INIT)
#define P4_REMOVE_BOOT_BANK_INIT p4_remove_bank_init
#endif

int krnP4RemoveBootPrepare(struct MemHeader *pool,
                           uintptr_t region_begin,
                           uintptr_t region_end)
{
    const size_t bytes = P4_REMOVE_BOOT_ROUND(
        sizeof(struct p4_remove_bank_slot) * P4_REMOVE_BOOT_CAPACITY);
    void *storage = NULL;
    volatile unsigned char *clear;
    struct p4_remove_bank_slot *slots;
    size_t index;

    if (p4_remove_boot_attempted)
        return 0;

    /* Mark the sole boot attempt before validating caller input. A failed
     * prefix reservation or bank init is terminal; no runtime reset exists. */
    p4_remove_boot_attempted = 1;
    if (!krnP4BootReservePrefix(pool, region_begin, region_end, bytes,
                                P4_REMOVE_BOOT_ALIGN, &storage))
        return 0;

    /* Clear the reserved prefix explicitly before creating typed slot
     * objects. Volatile stores avoid a pre-Exec runtime memset dependency. */
    clear = (volatile unsigned char *)storage;
    for (index = 0u; index < bytes; ++index)
        clear[index] = 0u;

    slots = (struct p4_remove_bank_slot *)storage;
    if (P4_REMOVE_BOOT_BANK_INIT(&p4_remove_boot_bank, slots,
                                 P4_REMOVE_BOOT_CAPACITY) != 1)
        return 0; /* Reservation remains withheld; nothing is published. */

    p4_remove_boot_bytes = bytes;
    p4_remove_boot_prepared = 1;
    return 1;
}

struct p4_remove_bank *krnP4RemoveBootBank(void)
{
    return p4_remove_boot_prepared ? &p4_remove_boot_bank : NULL;
}

size_t krnP4RemoveBootBytes(void)
{
    return p4_remove_boot_prepared ? p4_remove_boot_bytes : 0u;
}

uint32_t krnP4RemoveBootCapacity(void)
{
    return p4_remove_boot_prepared ? P4_REMOVE_BOOT_CAPACITY : 0u;
}
