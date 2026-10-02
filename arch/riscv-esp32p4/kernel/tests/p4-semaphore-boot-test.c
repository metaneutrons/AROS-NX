/* Composes actual boot reservation, arena initialization and ledger sources.
 * Exec type layouts are host shims; the real RV32 layout is compiled separately. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader
{
    struct { unsigned int ln_Type; } mh_Node;
    unsigned int mh_Attributes;
    struct MemChunk *mh_First;
    void *mh_Lower, *mh_Upper;
    uintptr_t mh_Free;
};
struct SemaphoreRequest
{
    struct { void *next, *previous; } link;
    void *waiter;
    struct { uint32_t word, owner; } lock;
};
#define MEMCHUNK_TOTAL 16u
#define MEMHEADER_TOTAL ((sizeof(struct MemHeader) + 15u) & ~(size_t)15u)
#define MEMF_MANAGED (1u << 15)
#define NT_MEMORY 10u
#define P4_BOOT_RESERVE_TEST 1
#define P4_SEMAPHORE_BOOT_TEST 1
#define P4_SEMAPHORE_OPERATION_CAPACITY 3u
#define P4_SEMAPHORE_ENTRY_CAPACITY 7u
#include "../p4_boot_reserve.c"
#include "../p4_semaphore_ledger.h"
#define p4_semaphore_ledger_init boot_test_actual_ledger_init
#include "../p4_semaphore_ledger.c"
#undef p4_semaphore_ledger_init

static int injected_init_failure;
int p4_semaphore_ledger_init(struct p4_semaphore_ledger *ledger,
    struct p4_semaphore_ledger_operation *operations, uint32_t op_capacity,
    struct p4_semaphore_ledger_entry *entries, uint32_t entry_capacity)
{
    if (injected_init_failure)
        return -1;
    return boot_test_actual_ledger_init(ledger, operations, op_capacity,
                                        entries, entry_capacity);
}
#include "../p4_semaphore_boot.c"

static _Alignas(64) unsigned char memory[4096];
static unsigned char before[4096];
static unsigned int checks, failures;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expression); } } while (0)

static struct MemHeader *prepare(void)
{
    struct MemHeader *pool = (struct MemHeader *)(void *)memory;
    memset(memory, 0, sizeof(memory));
    pool->mh_Node.ln_Type = NT_MEMORY;
    pool->mh_Lower = memory;
    pool->mh_Upper = memory + sizeof(memory);
    pool->mh_First = (struct MemChunk *)(void *)(memory + MEMHEADER_TOTAL);
    pool->mh_First->mc_Bytes = sizeof(memory) - MEMHEADER_TOTAL;
    pool->mh_Free = pool->mh_First->mc_Bytes;
    /* Test-only fresh boot, not a runtime reset mechanism. */
    p4_semaphore_boot_ledger = (struct p4_semaphore_ledger){0};
    p4_semaphore_boot_requests = NULL;
    p4_semaphore_boot_bytes = 0;
    p4_semaphore_boot_attempted = 0;
    p4_semaphore_boot_prepared = 0;
    injected_init_failure = 0;
    return pool;
}

int main(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare();
    struct p4_semaphore_ledger *ledger;
    struct p4_semaphore_ledger_operation *operation = NULL;
    struct p4_semaphore_ledger_entry *entry = NULL;
    size_t expected = P4_SEMAPHORE_BOOT_ROUND(
        sizeof(struct p4_semaphore_ledger_operation) * 3u) +
        P4_SEMAPHORE_BOOT_ROUND(sizeof(struct p4_semaphore_ledger_entry) * 7u) +
        P4_SEMAPHORE_BOOT_ROUND(sizeof(struct SemaphoreRequest) * 7u);
    uintptr_t old_free = pool->mh_Free;
    uint32_t i;
    int owner = 0, semaphore = 0;

    CHECK(krnP4SemaphoreBootLedger() == NULL);
    CHECK(krnP4SemaphoreBootRequest(0) == NULL);
    CHECK(krnP4SemaphoreBootBytes() == 0);
    CHECK(krnP4SemaphoreBootOperationCapacity() == 0);
    CHECK(krnP4SemaphoreBootEntryCapacity() == 0);
    CHECK(krnP4SemaphoreBootPrepare(pool, begin, end));
    ledger = krnP4SemaphoreBootLedger();
    CHECK(ledger != NULL);
    CHECK(krnP4SemaphoreBootBytes() == expected);
    CHECK(krnP4SemaphoreBootOperationCapacity() == 3u);
    CHECK(krnP4SemaphoreBootEntryCapacity() == 7u);
    CHECK(pool->mh_Free <= old_free - expected);
    CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
    CHECK(pool->mh_First->mc_Next == NULL);
    CHECK(pool->mh_Lower == memory && pool->mh_Upper == memory + sizeof(memory));
    CHECK((uintptr_t)ledger->operations % 16u == 0);
    CHECK((uintptr_t)ledger->entries % 16u == 0);
    for (i = 0; i < 7u; ++i)
    {
        struct SemaphoreRequest *request = krnP4SemaphoreBootRequest(i);
        CHECK(request != NULL);
        CHECK(request->link.next == NULL && request->link.previous == NULL);
        CHECK(request->waiter == NULL && request->lock.word == 0u &&
              request->lock.owner == 0u);
        if (i > 0)
            CHECK(request == krnP4SemaphoreBootRequest(i - 1u) + 1);
    }
    CHECK(krnP4SemaphoreBootRequest(7u) == NULL);
    CHECK(krnP4SemaphoreBootRequest(UINT32_MAX) == NULL);
    CHECK(p4_semaphore_ledger_reserve(ledger, &owner, 1u, 1u, &operation) == 1);
    CHECK(p4_semaphore_ledger_set_entry(ledger, operation, 0u, &semaphore,
                                       krnP4SemaphoreBootRequest(0)) == 1);
    CHECK(p4_semaphore_ledger_close(ledger, operation) == 1);
    CHECK(p4_semaphore_ledger_entry_at(ledger, operation, 0u, &entry) == 1);
    CHECK(entry->request == krnP4SemaphoreBootRequest(0));
    CHECK(p4_semaphore_ledger_acquire(ledger, operation, 0u) == 1);
    CHECK(p4_semaphore_ledger_commit(ledger, operation) == 1);
    CHECK(p4_semaphore_ledger_reclaim(ledger, operation) == 1);
    memcpy(before, memory, sizeof(memory));
    CHECK(!krnP4SemaphoreBootPrepare(pool, begin, end));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4SemaphoreBootLedger() == ledger);

    pool = prepare();
    memcpy(before, memory, sizeof(memory));
    CHECK(!krnP4SemaphoreBootPrepare(NULL, begin, end));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4SemaphoreBootLedger() == NULL);
    CHECK(!krnP4SemaphoreBootPrepare(pool, begin, end));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);

    pool = prepare();
    pool->mh_Attributes = MEMF_MANAGED;
    memcpy(before, memory, sizeof(memory));
    CHECK(!krnP4SemaphoreBootPrepare(pool, begin, end));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4SemaphoreBootLedger() == NULL);

    pool = prepare();
    old_free = pool->mh_Free;
    injected_init_failure = 1;
    CHECK(!krnP4SemaphoreBootPrepare(pool, begin, end));
    CHECK(pool->mh_Free < old_free); /* Space remains withheld after failure. */
    CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
    CHECK(krnP4SemaphoreBootLedger() == NULL);
    CHECK(krnP4SemaphoreBootRequest(0) == NULL);
    CHECK(krnP4SemaphoreBootBytes() == 0);
    memcpy(before, memory, sizeof(memory));
    CHECK(!krnP4SemaphoreBootPrepare(pool, begin, end));
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);

    printf("P4 semaphore boot checks: %u passed, %u failed\n", checks, failures);
    return failures != 0u;
}
