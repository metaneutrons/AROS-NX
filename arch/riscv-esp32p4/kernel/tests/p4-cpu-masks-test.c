/*
 * Host test for the actual P4 CPU-mask helper implementation.  The allocator,
 * hart CSR and P4 soft-atomic service are mocked at their existing boundaries.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

#include "../p4_cpu_masks.h"
#include "../p4_soft_atomic.h"

#define TEST_MASK_COUNT 4u

static uint32_t test_masks[TEST_MASK_COUNT];
static unsigned char test_mask_used[TEST_MASK_COUNT];
static size_t alloc_count;
static size_t alloc_size;
static uint32_t alloc_flags;
static size_t free_count;
static size_t free_size;
static void *freed_mask;
static int alloc_fail;
static unsigned int atomic_count;
static uint32_t atomic_width;
static enum p4_soft_atomic_operation atomic_operation;
static enum p4_soft_atomic_result next_atomic_result = P4_SOFT_ATOMIC_OK;
static int mocked_hart;
static unsigned int failures;
static jmp_buf fatal_jump;
static int fatal_expected;
static uint32_t fatal_count;
static enum p4_soft_atomic_result fatal_reason;
static int mocked_mie = 1;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

void *p4CpuMasksTestAllocMem(size_t size, uint32_t flags)
{
    unsigned int i;

    ++alloc_count;
    alloc_size = size;
    alloc_flags = flags;
    if (alloc_fail)
        return NULL;

    for (i = 0; i < TEST_MASK_COUNT; ++i)
    {
        if (!test_mask_used[i])
        {
            test_mask_used[i] = 1u;
            if ((flags & 0x00010000u) != 0u)
                test_masks[i] = 0u;
            return &test_masks[i];
        }
    }

    return NULL;
}

void p4CpuMasksTestFreeMem(void *memory, size_t size)
{
    unsigned int i;

    ++free_count;
    free_size = size;
    freed_mask = memory;

    for (i = 0; i < TEST_MASK_COUNT; ++i)
    {
        if (memory == &test_masks[i])
        {
            test_mask_used[i] = 0u;
            return;
        }
    }
}

int p4CpuMasksTestGetCPUNumber(void)
{
    return mocked_hart;
}

void p4CpuMasksTestDisableInterrupts(void)
{
    mocked_mie = 0;
}

void p4CpuMasksTestFatal(enum p4_soft_atomic_result reason)
{
    ++fatal_count;
    fatal_reason = reason;
    if (!fatal_expected)
    {
        fprintf(stderr, "unexpected CPU-mask fatal service error %d\n",
                (int)reason);
        abort();
    }

    longjmp(fatal_jump, 1);
}

enum p4_soft_atomic_result krnP4SoftAtomicOperate(
    volatile void *address, uint32_t width,
    enum p4_soft_atomic_operation operation, uint32_t operand,
    uint32_t *old_value)
{
    unsigned int i;
    volatile uint32_t *word = NULL;
    enum p4_soft_atomic_result result = next_atomic_result;

    ++atomic_count;
    atomic_width = width;
    atomic_operation = operation;
    next_atomic_result = P4_SOFT_ATOMIC_OK;

    if (result != P4_SOFT_ATOMIC_OK)
        return result;

    if (width != 4u || old_value == NULL || address == NULL)
        return P4_SOFT_ATOMIC_BAD_WIDTH;

    for (i = 0; i < TEST_MASK_COUNT; ++i)
    {
        if (address == (volatile void *)&test_masks[i])
        {
            word = &test_masks[i];
            break;
        }
    }
    if (word == NULL)
        return P4_SOFT_ATOMIC_BAD_ADDRESS;

    *old_value = *word;
    if (operation == P4_SOFT_ATOMIC_STORE)
        *word = operand;
    else if (operation != P4_SOFT_ATOMIC_LOAD)
        return P4_SOFT_ATOMIC_BAD_OPERATION;

    return P4_SOFT_ATOMIC_OK;
}

static void test_allocation_and_mask_operations(void)
{
    void *mask = krnP4AllocCPUMask();
    unsigned int calls;
    uint32_t *word = (uint32_t *)mask;

    CHECK(mask != NULL);
    CHECK(alloc_count == 1u);
    CHECK(alloc_size == sizeof(uint32_t));
    CHECK(alloc_flags == (0x00000001u | 0x00010000u));
    CHECK(word != NULL && *word == 0u);
    CHECK(atomic_count == 0u);

    krnP4GetCPUMask(0u, mask);
    CHECK(*word == 1u);
    CHECK(atomic_operation == P4_SOFT_ATOMIC_STORE);
    CHECK(atomic_width == 4u);
    CHECK(krnP4CPUInMask(0u, mask));
    CHECK(!krnP4CPUInMask(1u, mask));
    CHECK(atomic_operation == P4_SOFT_ATOMIC_LOAD);
    CHECK(atomic_width == 4u);

    /* GetCPUMask follows the existing API's SET semantics. */
    krnP4GetCPUMask(1u, mask);
    CHECK(*word == 2u);
    CHECK(!krnP4CPUInMask(0u, mask));
    CHECK(krnP4CPUInMask(1u, mask));

    krnP4ClearCPUMask(mask);
    CHECK(*word == 0u);
    CHECK(atomic_operation == P4_SOFT_ATOMIC_STORE);
    CHECK(atomic_width == 4u);

    calls = atomic_count;
    krnP4GetCPUMask(UINT32_MAX, mask);
    krnP4GetCPUMask(2u, mask);
    krnP4GetCPUMask(32u, mask);
    CHECK(atomic_count == calls);
    CHECK(*word == 0u);
    CHECK(!krnP4CPUInMask(UINT32_MAX, mask));
    CHECK(!krnP4CPUInMask(2u, mask));
    CHECK(!krnP4CPUInMask(32u, mask));

    calls = atomic_count;
    krnP4ClearCPUMask(NULL);
    krnP4GetCPUMask(0u, NULL);
    CHECK(atomic_count == calls);

    krnP4FreeCPUMask(mask);
    CHECK(free_count == 1u);
    CHECK(free_size == sizeof(uint32_t));
    CHECK(freed_mask == mask);

    alloc_fail = 1;
    CHECK(krnP4AllocCPUMask() == NULL);
    CHECK(alloc_count == 2u);
    CHECK(alloc_size == sizeof(uint32_t));
    CHECK(alloc_flags == (0x00000001u | 0x00010000u));
    alloc_fail = 0;
}

static void test_shorthands(void)
{
    void *any = (void *)(uintptr_t)UINTPTR_MAX;
    void *all_but_self = (void *)(uintptr_t)(UINTPTR_MAX - 1u);
    unsigned int calls = atomic_count;
    size_t frees = free_count;

    CHECK(krnP4CPUInMask(0u, NULL));
    CHECK(!krnP4CPUInMask(1u, NULL));
    CHECK(krnP4CPUInMask(0u, any));
    CHECK(krnP4CPUInMask(1u, any));
    CHECK(!krnP4CPUInMask(UINT32_MAX, any));

    mocked_hart = 0;
    CHECK(!krnP4CPUInMask(0u, all_but_self));
    CHECK(krnP4CPUInMask(1u, all_but_self));
    mocked_hart = 1;
    CHECK(krnP4CPUInMask(0u, all_but_self));
    CHECK(!krnP4CPUInMask(1u, all_but_self));
    mocked_hart = 2;
    CHECK(!krnP4CPUInMask(0u, all_but_self));
    mocked_hart = 0;

    krnP4GetCPUMask(0u, any);
    krnP4GetCPUMask(1u, all_but_self);
    krnP4ClearCPUMask(any);
    krnP4ClearCPUMask(all_but_self);
    krnP4FreeCPUMask(NULL);
    krnP4FreeCPUMask(any);
    krnP4FreeCPUMask(all_but_self);

    CHECK(atomic_count == calls);
    CHECK(free_count == frees);
}

static void invoke_failed_get(void *mask)
{
    krnP4GetCPUMask(0u, mask);
}

static void invoke_failed_clear(void *mask)
{
    krnP4ClearCPUMask(mask);
}

static void invoke_failed_membership(void *mask)
{
    (void)krnP4CPUInMask(1u, mask);
}

static void invoke_invalid_mask_pointer(void *unused)
{
    (void)unused;
    (void)krnP4CPUInMask(0u, (const void *)(uintptr_t)0x1234u);
}

static void expect_fatal(void (*operation)(void *), void *argument,
                         enum p4_soft_atomic_result reason)
{
    uint32_t prior_count = fatal_count;

    mocked_mie = 1;
    fatal_expected = 1;
    if (setjmp(fatal_jump) == 0)
    {
        operation(argument);
        CHECK(0 && "failed atomic mask operation unexpectedly returned");
    }
    fatal_expected = 0;

    CHECK(fatal_count == prior_count + 1u);
    CHECK(fatal_reason == reason);
    CHECK(mocked_mie == 0);
}

static void test_service_failures(void)
{
    void *mask = krnP4AllocCPUMask();
    uint32_t *word = (uint32_t *)mask;

    CHECK(mask != NULL);
    *word = 2u;

    next_atomic_result = P4_SOFT_ATOMIC_BAD_WIDTH;
    expect_fatal(invoke_failed_get, mask, P4_SOFT_ATOMIC_BAD_WIDTH);
    CHECK(*word == 2u);
    CHECK(atomic_operation == P4_SOFT_ATOMIC_STORE);
    CHECK(atomic_width == 4u);

    next_atomic_result = P4_SOFT_ATOMIC_BAD_WIDTH;
    expect_fatal(invoke_failed_clear, mask, P4_SOFT_ATOMIC_BAD_WIDTH);
    CHECK(*word == 2u);
    CHECK(atomic_operation == P4_SOFT_ATOMIC_STORE);
    CHECK(atomic_width == 4u);

    next_atomic_result = P4_SOFT_ATOMIC_BAD_WIDTH;
    expect_fatal(invoke_failed_membership, mask, P4_SOFT_ATOMIC_BAD_WIDTH);
    CHECK(atomic_operation == P4_SOFT_ATOMIC_LOAD);
    CHECK(atomic_width == 4u);

    expect_fatal(invoke_invalid_mask_pointer, NULL,
                 P4_SOFT_ATOMIC_BAD_ADDRESS);

    krnP4FreeCPUMask(mask);
}

static void test_online_publication(void)
{
    CHECK(p4CpuMasksTestGetOnlineMask() == 1u);
    CHECK(krnP4GetCPUCount() == 1u);
    CHECK(krnP4GetCPUCount() == 1u);
    CHECK(krnP4GetCPUCount() == 1u);

    mocked_hart = 1;
    CHECK(!krnP4PublishSecondaryOnlineAfterHandshake(1));
    CHECK(p4CpuMasksTestGetOnlineMask() == 1u);
    CHECK(krnP4GetCPUCount() == 1u);

    mocked_hart = 0;
    CHECK(!krnP4PublishSecondaryOnlineAfterHandshake(0));
    CHECK(p4CpuMasksTestGetOnlineMask() == 1u);
    CHECK(krnP4GetCPUCount() == 1u);

    CHECK(krnP4PublishSecondaryOnlineAfterHandshake(1));
    CHECK(p4CpuMasksTestGetOnlineMask() == 3u);
    CHECK(krnP4GetCPUCount() == 2u);
    CHECK(!krnP4PublishSecondaryOnlineAfterHandshake(1));
    CHECK(p4CpuMasksTestGetOnlineMask() == 3u);
    CHECK(krnP4GetCPUCount() == 2u);

    mocked_hart = 1;
    CHECK(!krnP4PublishSecondaryOnlineAfterHandshake(1));
    CHECK(p4CpuMasksTestGetOnlineMask() == 3u);
    CHECK(krnP4GetCPUCount() == 2u);
}

int main(void)
{
    test_allocation_and_mask_operations();
    test_shorthands();
    test_service_failures();
    test_online_publication();

    if (failures != 0u)
    {
        fprintf(stderr, "%u CPU-mask test(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    puts("P4 CPU-mask tests passed");
    return EXIT_SUCCESS;
}
