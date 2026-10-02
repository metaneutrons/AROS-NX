/* Actual boot-reserve, remove-bank, completion and boot-binding bodies.
 * Exec MemHeader and memory constants are host-layout shims. Each boot
 * scenario runs in a child process so the production one-shot state is never
 * reset by the fixture. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct MemChunk
{
    struct MemChunk *mc_Next;
    uintptr_t mc_Bytes;
};

struct MemHeader
{
    struct { unsigned int ln_Type; } mh_Node;
    unsigned int mh_Attributes;
    struct MemChunk *mh_First;
    void *mh_Lower;
    void *mh_Upper;
    uintptr_t mh_Free;
};

struct p4_remove_bank;
struct p4_remove_bank_slot;

#define MEMCHUNK_TOTAL 16u
#define MEMHEADER_TOTAL \
    ((sizeof(struct MemHeader) + 15u) & ~(size_t)15u)
#define MEMF_MANAGED (1u << 15)
#define NT_MEMORY 10u
#define P4_BOOT_RESERVE_TEST 1
#define P4_REMOVE_BOOT_TEST 1

#include "../../exec/p4_remove_bank.c"
#include "../../exec/p4_remove_completion.c"
#include "../p4_boot_reserve.c"

static int injected_bank_init_failure;

static int boot_test_bank_init(struct p4_remove_bank *bank,
                               struct p4_remove_bank_slot *slots,
                               uint32_t capacity);

#define P4_REMOVE_BOOT_BANK_INIT boot_test_bank_init
#include "../p4_remove_boot.c"

static int boot_test_bank_init(struct p4_remove_bank *bank,
                               struct p4_remove_bank_slot *slots,
                               uint32_t capacity)
{
    if (injected_bank_init_failure)
        return -1;
    return p4_remove_bank_init(bank, slots, capacity);
}

/* Exercise the actual default capacity, not a reduced test-only bank. Keep
 * sufficient host-layout space beyond the permanent prefix for MemChunk. */
static _Alignas(64) unsigned char memory[
    sizeof(struct p4_remove_bank_slot) * P4_REMOVE_BOOT_CAPACITY + 131072u];
static unsigned int checks;
static unsigned int failures;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
    } \
} while (0)

struct scenario_result
{
    unsigned int checks;
    unsigned int failures;
};

typedef void (*scenario_fn)(void);

static struct MemHeader *prepare_pool(unsigned int fill,
                                      unsigned int attributes)
{
    struct MemHeader *pool;
    struct MemChunk *first;

    memset(memory, (int)fill, sizeof(memory));
    pool = (struct MemHeader *)(void *)memory;
    memset(pool, 0, sizeof(*pool));
    pool->mh_Node.ln_Type = NT_MEMORY;
    pool->mh_Attributes = attributes;
    pool->mh_Lower = memory;
    pool->mh_Upper = memory + sizeof(memory);
    first = (struct MemChunk *)(void *)(memory + MEMHEADER_TOTAL);
    first->mc_Next = NULL;
    first->mc_Bytes = sizeof(memory) - MEMHEADER_TOTAL;
    pool->mh_First = first;
    pool->mh_Free = first->mc_Bytes;
    return pool;
}

static size_t bank_bytes(void)
{
    size_t bytes = sizeof(struct p4_remove_bank_slot) *
                   (size_t)P4_REMOVE_BOOT_CAPACITY;
    return (bytes + 15u) & ~(size_t)15u;
}

static uintptr_t round16(uintptr_t value)
{
    return (value + 15u) & ~(uintptr_t)15u;
}

static void scenario_success_and_repeat(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare_pool(0xa5u, 0u);
    struct p4_remove_bank *bank;
    struct p4_remove_handle handle = {0u, 0u};
    struct p4_remove_handle ignored = {77u, 88u};
    size_t reserved_bytes = bank_bytes();
    uintptr_t expected_storage = round16((uintptr_t)pool->mh_First);
    uintptr_t old_free = pool->mh_Free;
    uintptr_t free_before_retry;
    unsigned char before[sizeof(memory)];
    size_t i;
    int ready = -1;
    uintptr_t notify = (uintptr_t)0x777u;
    int reclaim = -1;
    uint32_t count = UINT32_MAX;

    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
    CHECK(krnP4RemoveBootCapacity() == 0u);
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 1);
    bank = krnP4RemoveBootBank();
    CHECK(bank != NULL);
    CHECK(krnP4RemoveBootBytes() == reserved_bytes);
    CHECK(krnP4RemoveBootCapacity() == P4_REMOVE_BOOT_CAPACITY);
    CHECK(pool->mh_Free == old_free - reserved_bytes);
    CHECK(pool->mh_First->mc_Next == NULL);
    CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
    CHECK(pool->mh_Lower == memory && pool->mh_Upper == memory + sizeof(memory));
    CHECK((uintptr_t)bank->slots == expected_storage);
    CHECK(((uintptr_t)bank->slots & 15u) == 0u);
    CHECK(expected_storage >= begin + MEMHEADER_TOTAL);
    CHECK(expected_storage + reserved_bytes <= end);
    for (i = 0u; i < reserved_bytes; ++i)
        CHECK(((const volatile unsigned char *)expected_storage)[i] == 0u);

    /* The actual returned bank is initialized and can perform the full leaf
     * request/ACK/drop lifecycle; identities remain opaque integers. */
    CHECK(p4_remove_bank_count_target(bank, (uintptr_t)0x900u, &count) == 1);
    CHECK(count == 0u);
    CHECK(p4_remove_bank_reserve(bank, (uintptr_t)0x101u,
                                 (uintptr_t)0x900u, &handle) == 1);
    CHECK(p4_remove_bank_ready(bank, handle, (uintptr_t)0x101u, &ready) == 1);
    CHECK(ready == 0);
    CHECK(p4_remove_bank_ack(bank, handle, (uintptr_t)0x900u,
                             &notify, &reclaim) == 1);
    CHECK(notify == (uintptr_t)0x101u && reclaim == 0);
    CHECK(p4_remove_bank_drop(bank, handle, (uintptr_t)0x101u, 0,
                              &reclaim) == 1);
    CHECK(reclaim == 1);

    memcpy(before, memory, sizeof(memory));
    free_before_retry = pool->mh_Free;
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(pool->mh_Free == free_before_retry);
    CHECK(krnP4RemoveBootBank() == bank);
    CHECK(krnP4RemoveBootBytes() == reserved_bytes);
    CHECK(krnP4RemoveBootCapacity() == P4_REMOVE_BOOT_CAPACITY);
    CHECK(ignored.index == 77u && ignored.generation == 88u);
}

static void scenario_null_pool_one_shot(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    unsigned char before[sizeof(memory)];
    struct MemHeader *pool;

    memset(memory, 0x6du, sizeof(memory));
    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(NULL, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
    CHECK(krnP4RemoveBootCapacity() == 0u);

    pool = prepare_pool(0x4bu, 0u);
    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
}

static void scenario_managed_pool_refusal(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare_pool(0x43u, MEMF_MANAGED);
    unsigned char before[sizeof(memory)];

    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
    CHECK(krnP4RemoveBootCapacity() == 0u);
}

static void scenario_fragmented_pool_refusal(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare_pool(0x52u, 0u);
    unsigned char before[sizeof(memory)];

    pool->mh_First->mc_Next = pool->mh_First;
    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
}

static void scenario_invalid_region_refusal(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare_pool(0x39u, 0u);
    unsigned char before[sizeof(memory)];

    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(pool, begin + 1u, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
}

static void scenario_bank_init_failure_withholds(void)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t end = begin + sizeof(memory);
    struct MemHeader *pool = prepare_pool(0xadu, 0u);
    const uintptr_t expected_storage = round16((uintptr_t)pool->mh_First);
    const size_t bytes = bank_bytes();
    const uintptr_t old_free = pool->mh_Free;
    unsigned char before[sizeof(memory)];
    uintptr_t free_after_reserve;
    size_t i;

    injected_bank_init_failure = 1;
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    free_after_reserve = pool->mh_Free;
    CHECK(free_after_reserve == old_free - bytes);
    CHECK(krnP4RemoveBootBank() == NULL);
    CHECK(krnP4RemoveBootBytes() == 0u);
    CHECK(krnP4RemoveBootCapacity() == 0u);
    CHECK(pool->mh_First->mc_Bytes == pool->mh_Free);
    CHECK(expected_storage + bytes <= end);
    for (i = 0u; i < bytes; ++i)
        CHECK(((const volatile unsigned char *)expected_storage)[i] == 0u);

    memcpy(before, memory, sizeof(memory));
    CHECK(krnP4RemoveBootPrepare(pool, begin, end) == 0);
    CHECK(memcmp(before, memory, sizeof(memory)) == 0);
    CHECK(krnP4RemoveBootBank() == NULL);
}

static int run_scenario(const char *name, scenario_fn scenario,
                        unsigned int *out_checks,
                        unsigned int *out_failures)
{
    int descriptors[2];
    pid_t child;
    int status;
    struct scenario_result result;
    size_t received = 0u;

    if (pipe(descriptors) != 0)
    {
        fprintf(stderr, "pipe failed for %s\n", name);
        return 0;
    }

    child = fork();
    if (child < 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        fprintf(stderr, "fork failed for %s\n", name);
        return 0;
    }

    if (child == 0)
    {
        ssize_t wrote;

        close(descriptors[0]);
        checks = 0u;
        failures = 0u;
        scenario();
        result.checks = checks;
        result.failures = failures;
        wrote = write(descriptors[1], &result, sizeof(result));
        close(descriptors[1]);
        _exit(wrote == (ssize_t)sizeof(result) && failures == 0u ? 0 : 1);
    }

    close(descriptors[1]);
    while (received < sizeof(result))
    {
        ssize_t amount = read(descriptors[0],
                              (unsigned char *)&result + received,
                              sizeof(result) - received);
        if (amount <= 0)
            break;
        received += (size_t)amount;
    }
    close(descriptors[0]);
    if (waitpid(child, &status, 0) != child ||
        received != sizeof(result) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
    {
        fprintf(stderr, "scenario failed: %s\n", name);
        return 0;
    }

    *out_checks += result.checks;
    *out_failures += result.failures;
    return 1;
}

int main(void)
{
    unsigned int total_checks = 0u;
    unsigned int total_failures = 0u;

    if (!run_scenario("success/repeat", scenario_success_and_repeat,
                      &total_checks, &total_failures) ||
        !run_scenario("null pool/one shot", scenario_null_pool_one_shot,
                      &total_checks, &total_failures) ||
        !run_scenario("managed pool", scenario_managed_pool_refusal,
                      &total_checks, &total_failures) ||
        !run_scenario("fragmented pool", scenario_fragmented_pool_refusal,
                      &total_checks, &total_failures) ||
        !run_scenario("invalid region", scenario_invalid_region_refusal,
                      &total_checks, &total_failures) ||
        !run_scenario("bank init failure", scenario_bank_init_failure_withholds,
                      &total_checks, &total_failures))
        return 1;

    printf("p4-remove-boot-test: PASS (%u checks, %u failures)\n",
           total_checks, total_failures);
    return total_failures != 0u;
}
