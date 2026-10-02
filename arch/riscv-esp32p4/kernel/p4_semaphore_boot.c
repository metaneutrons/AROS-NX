/* No runtime allocation: boot carves permanent private ledger/node storage. */
#if !defined(P4_SEMAPHORE_BOOT_TEST)
#include <exec/memory.h>
#include <exec/semaphores.h>
#endif
#include "p4_boot_reserve.h"
#include "p4_semaphore_ledger.h"
#include "p4_semaphore_boot.h"

#ifndef P4_SEMAPHORE_OPERATION_CAPACITY
#define P4_SEMAPHORE_OPERATION_CAPACITY 1024u
#endif
#ifndef P4_SEMAPHORE_ENTRY_CAPACITY
#define P4_SEMAPHORE_ENTRY_CAPACITY 4096u
#endif
#define P4_SEMAPHORE_BOOT_ALIGN 16u
#define P4_SEMAPHORE_BOOT_ROUND(size) \
    (((size) + P4_SEMAPHORE_BOOT_ALIGN - 1u) & \
     ~(size_t)(P4_SEMAPHORE_BOOT_ALIGN - 1u))

_Static_assert(P4_SEMAPHORE_OPERATION_CAPACITY > 0 &&
    P4_SEMAPHORE_OPERATION_CAPACITY <= P4_SEMAPHORE_LEDGER_LIMIT,
    "invalid semaphore operation capacity");
_Static_assert(P4_SEMAPHORE_ENTRY_CAPACITY > 0 &&
    P4_SEMAPHORE_ENTRY_CAPACITY <= P4_SEMAPHORE_LEDGER_LIMIT,
    "invalid semaphore entry capacity");
_Static_assert(_Alignof(struct p4_semaphore_ledger_operation) <=
    P4_SEMAPHORE_BOOT_ALIGN &&
    _Alignof(struct p4_semaphore_ledger_entry) <= P4_SEMAPHORE_BOOT_ALIGN &&
    _Alignof(struct SemaphoreRequest) <= P4_SEMAPHORE_BOOT_ALIGN,
    "semaphore boot alignment is insufficient");
_Static_assert(sizeof(struct p4_semaphore_ledger_operation) <=
    (SIZE_MAX / 4u) / P4_SEMAPHORE_OPERATION_CAPACITY &&
    sizeof(struct p4_semaphore_ledger_entry) <=
    (SIZE_MAX / 4u) / P4_SEMAPHORE_ENTRY_CAPACITY &&
    sizeof(struct SemaphoreRequest) <=
    (SIZE_MAX / 4u) / P4_SEMAPHORE_ENTRY_CAPACITY,
    "semaphore boot storage size overflows");

static struct p4_semaphore_ledger p4_semaphore_boot_ledger;
static struct SemaphoreRequest *p4_semaphore_boot_requests;
static size_t p4_semaphore_boot_bytes;
static int p4_semaphore_boot_attempted;
static int p4_semaphore_boot_prepared;

int krnP4SemaphoreBootPrepare(struct MemHeader *pool,
    uintptr_t region_begin, uintptr_t region_end)
{
    const size_t operation_bytes = P4_SEMAPHORE_BOOT_ROUND(
        sizeof(struct p4_semaphore_ledger_operation) *
        P4_SEMAPHORE_OPERATION_CAPACITY);
    const size_t entry_bytes = P4_SEMAPHORE_BOOT_ROUND(
        sizeof(struct p4_semaphore_ledger_entry) * P4_SEMAPHORE_ENTRY_CAPACITY);
    const size_t request_bytes = P4_SEMAPHORE_BOOT_ROUND(
        sizeof(struct SemaphoreRequest) * P4_SEMAPHORE_ENTRY_CAPACITY);
    const size_t bytes = operation_bytes + entry_bytes + request_bytes;
    void *storage = NULL;
    volatile unsigned char *clear;
    struct p4_semaphore_ledger_operation *operations;
    struct p4_semaphore_ledger_entry *entries;
    size_t i;

    if (p4_semaphore_boot_attempted)
        return 0;
    p4_semaphore_boot_attempted = 1;
    if (!krnP4BootReservePrefix(pool, region_begin, region_end, bytes,
                               P4_SEMAPHORE_BOOT_ALIGN, &storage))
        return 0;

    /* Volatile stores prohibit lowering this pre-Exec clear into a runtime
     * memset service. All typed objects begin quiescent and unpublished. */
    clear = (volatile unsigned char *)storage;
    for (i = 0; i < bytes; ++i)
        clear[i] = 0;
    operations = (struct p4_semaphore_ledger_operation *)storage;
    entries = (struct p4_semaphore_ledger_entry *)
        ((unsigned char *)storage + operation_bytes);
    if (p4_semaphore_ledger_init(&p4_semaphore_boot_ledger, operations,
            P4_SEMAPHORE_OPERATION_CAPACITY, entries,
            P4_SEMAPHORE_ENTRY_CAPACITY) != 1)
        return 0; /* Reserved storage stays withheld on bootstrap failure. */
    p4_semaphore_boot_requests = (struct SemaphoreRequest *)
        ((unsigned char *)storage + operation_bytes + entry_bytes);
    p4_semaphore_boot_bytes = bytes;
    p4_semaphore_boot_prepared = 1;
    return 1;
}

struct p4_semaphore_ledger *krnP4SemaphoreBootLedger(void)
{
    return p4_semaphore_boot_prepared ? &p4_semaphore_boot_ledger : NULL;
}

struct SemaphoreRequest *krnP4SemaphoreBootRequest(uint32_t index)
{
    if (!p4_semaphore_boot_prepared || index >= P4_SEMAPHORE_ENTRY_CAPACITY)
        return NULL;
    return &p4_semaphore_boot_requests[index];
}

size_t krnP4SemaphoreBootBytes(void)
{
    return p4_semaphore_boot_bytes;
}

uint32_t krnP4SemaphoreBootOperationCapacity(void)
{
    return p4_semaphore_boot_prepared ? P4_SEMAPHORE_OPERATION_CAPACITY : 0;
}

uint32_t krnP4SemaphoreBootEntryCapacity(void)
{
    return p4_semaphore_boot_prepared ? P4_SEMAPHORE_ENTRY_CAPACITY : 0;
}
