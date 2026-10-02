/* Host tests exercise the production private software-atomic state machine. */
#include <pthread.h>
#include <setjmp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define P4_SPINLOCK_TEST
#define P4_SOFT_ATOMIC_TEST
static uint32_t test_high_cache_config = 2u;
#define P4_SOFT_ATOMIC_HIGH_CONFIG_READ() test_high_cache_config

#include "../p4_soft_atomic.h"

/* Minimal RV32 public lock fixture: word/bit layout and owner follow the ABI. */
#define _AROS_TYPES_SPINLOCK_S_H_
typedef struct
{
    union
    {
        volatile struct
        {
            unsigned int readcount : 24;
            unsigned int _pad2 : 3;
            unsigned int write : 1;
            unsigned int _pad1 : 3;
            unsigned int updating : 1;
        } slock;
        volatile unsigned char block[4];
        volatile unsigned int lock;
    };
    void *s_Owner;
} spinlock_t;
#define SPINLOCK_UNLOCKED 0u
#define SPINLOCKB_WRITE 27u
#define SPINLOCKB_UPDATING 31u
#define SPINLOCKF_WRITE (1u << SPINLOCKB_WRITE)
#define SPINLOCKF_UPDATING (1u << SPINLOCKB_UPDATING)
#define SPINLOCK_MODE_READ 0u
#define SPINLOCK_MODE_WRITE 1u

#include "../p4_spinlock.h"

static int test_spin_address_valid(const spinlock_t *lock,
                                   const struct p4_spinlock_policy *policy);
static int test_region_admitted(const struct p4_soft_atomic_region *region);
static int test_test_fatal(enum p4_soft_atomic_result reason)
    __attribute__((noreturn));
static uint32_t test_spin_load(volatile uint32_t *word);
static uint32_t test_spin_lr(volatile uint32_t *word);
static int test_spin_cas_once(volatile uint32_t *word,
                              uint32_t expected, uint32_t desired);
static uint32_t test_irq_save(void);
static void test_irq_restore(uint32_t saved_mie);
static uint32_t test_hart_id(void);
static uint32_t test_cycle_now(void);
static void test_full_fence(void);

#define P4_SPINLOCK_LOAD(word) test_spin_load(word)
#define P4_SPINLOCK_LR(word) test_spin_lr(word)
#define P4_SPINLOCK_CAS_ONCE(word, expected, desired) \
    test_spin_cas_once((word), (expected), (desired))
#define P4_SPINLOCK_ADDRESS_VALID(lock, policy) \
    test_spin_address_valid((lock), (policy))

#define P4_SOFT_ATOMIC_REGION_ADMITTED(region) \
    test_region_admitted((region))
#define P4_SOFT_ATOMIC_IRQ_SAVE() test_irq_save()
#define P4_SOFT_ATOMIC_IRQ_RESTORE(saved) test_irq_restore(saved)
#define P4_SOFT_ATOMIC_HART_ID() test_hart_id()
#define P4_SOFT_ATOMIC_CYCLE_NOW() test_cycle_now()
#define P4_SOFT_ATOMIC_FULL_FENCE() test_full_fence()
#define P4_SOFT_ATOMIC_FATAL(reason) test_test_fatal(reason)

#include "../p4_spinlock.c"
#include "../p4_soft_atomic.c"

#define TEST_STORAGE_BYTES 128u
#define TEST_WRITERS 3u
#define TEST_READERS 3u
#define TEST_WRITER_ITERATIONS 800u
#define TEST_READER_ITERATIONS 1200u
#define TEST_CAS_WRITERS 4u
#define TEST_CAS_ITERATIONS 800u
#define TEST_TOTAL_WRITES (TEST_WRITERS * TEST_WRITER_ITERATIONS)
#define TEST_TOTAL_OPERATIONS (TEST_TOTAL_WRITES + \
                               TEST_READERS * TEST_READER_ITERATIONS)
#define TEST_TOTAL_CAS_WRITES (TEST_CAS_WRITERS * TEST_CAS_ITERATIONS)

static union
{
    uint8_t bytes[TEST_STORAGE_BYTES];
    uint16_t halves[TEST_STORAGE_BYTES / sizeof(uint16_t)];
    uint32_t words[TEST_STORAGE_BYTES / sizeof(uint32_t)];
} test_storage __attribute__((aligned(4)));

static struct p4_soft_atomic_policy test_policy;
static _Thread_local uint32_t test_irq_mie;
static _Thread_local uint32_t test_hart;
static _Thread_local uint32_t test_cycles;
static _Thread_local uint32_t test_cycle_step = 1u;
static _Thread_local uint32_t test_fail_acquire_cas;
static _Thread_local uint32_t test_fail_unlock_cas;
static _Thread_local uint32_t test_spin_load_count;
static _Thread_local uint32_t test_irq_save_count;
static _Thread_local uint32_t test_irq_restore_count;
static _Thread_local uint32_t test_bad_irq_restore;
static uint32_t test_spin_busy_count;
static uint32_t test_spin_cas_count;
static uint32_t test_policy_admission_enabled = 1;
static uint32_t test_check_count;
static jmp_buf test_fatal_jump;
static volatile uint32_t test_fatal_reason;
static volatile uint32_t test_fatal_mie;
static uint32_t test_fatal_old_value;

#define CHECK(condition) do { \
    ++test_check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static int test_spin_address_valid(
    const spinlock_t *lock, const struct p4_spinlock_policy *policy)
{
    return lock == &p4_soft_atomic_lock &&
           policy == &p4_soft_atomic_lock_policy &&
           policy->region_count == 1u;
}

static int test_region_admitted(const struct p4_soft_atomic_region *region)
{
    uintptr_t begin = (uintptr_t)test_storage.bytes;
    uintptr_t end = begin + TEST_STORAGE_BYTES - 1u;

    if (test_policy_admission_enabled && region != NULL &&
        region->kind == P4_SOFT_ATOMIC_REGION_SRAM_HIGH)
        return p4_soft_atomic_region_canonical(region);

    return test_policy_admission_enabled && region != NULL &&
           region->kind == P4_SOFT_ATOMIC_REGION_PSRAM &&
           region->begin >= begin && region->end <= end &&
           region->begin < region->end;
}

static uint32_t test_spin_load(volatile uint32_t *word)
{
    uint32_t value;

    ++test_spin_load_count;
    value = __atomic_load_n((uint32_t *)word, __ATOMIC_RELAXED);
    if (value != SPINLOCK_UNLOCKED)
        __atomic_add_fetch(&test_spin_busy_count, 1u, __ATOMIC_RELAXED);
    return value;
}

static uint32_t test_spin_lr(volatile uint32_t *word)
{
    return __atomic_load_n((uint32_t *)word, __ATOMIC_ACQUIRE);
}

static int test_spin_cas_once(volatile uint32_t *word,
                              uint32_t expected, uint32_t desired)
{
    uint32_t observed;
    uint32_t *plain_word = (uint32_t *)word;

    __atomic_add_fetch(&test_spin_cas_count, 1u, __ATOMIC_RELAXED);
    observed = __atomic_load_n(plain_word, __ATOMIC_ACQUIRE);
    if (observed != expected)
        return 0;

    if (expected == SPINLOCK_UNLOCKED && desired == SPINLOCKF_WRITE &&
        test_fail_acquire_cas != 0)
    {
        --test_fail_acquire_cas;
        return 0;
    }
    if (expected == SPINLOCKF_WRITE && desired == SPINLOCK_UNLOCKED &&
        test_fail_unlock_cas != 0)
    {
        --test_fail_unlock_cas;
        return 0;
    }

    return __atomic_compare_exchange_n(plain_word, &expected, desired, 0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_RELAXED);
}

static uint32_t test_irq_save(void)
{
    uint32_t saved = test_irq_mie;

    ++test_irq_save_count;
    test_irq_mie = 0;
    return saved;
}

static void test_irq_restore(uint32_t saved_mie)
{
    if (test_irq_mie != 0)
        ++test_bad_irq_restore;
    ++test_irq_restore_count;
    test_irq_mie = saved_mie & 8u;
}

static uint32_t test_hart_id(void)
{
    return test_hart;
}

static uint32_t test_cycle_now(void)
{
    uint32_t now = test_cycles;

    test_cycles += test_cycle_step;
    return now;
}

static void test_full_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int test_test_fatal(enum p4_soft_atomic_result reason)
{
    test_fatal_reason = (uint32_t)reason;
    test_fatal_mie = test_irq_mie;
    longjmp(test_fatal_jump, 1);
}

static void prepare_policy(void)
{
    uintptr_t begin = (uintptr_t)test_storage.bytes;

    memset(&test_policy, 0, sizeof(test_policy));
    test_policy.regions[0].begin = begin;
    /* Leave the final byte outside the exact admitted extent. */
    test_policy.regions[0].end = begin + TEST_STORAGE_BYTES - 1u;
    test_policy.regions[0].kind = P4_SOFT_ATOMIC_REGION_PSRAM;
    test_policy.region_count = 1u;
    test_policy.max_attempts = 20000u;
    test_policy.max_cycles = 1000000u;
}

static int test_canonical_region_boundaries(void)
{
    struct p4_soft_atomic_region region;

    region.kind = P4_SOFT_ATOMIC_REGION_SRAM;
    region.begin = P4_SOFT_ATOMIC_SRAM_BASE;
    region.end = P4_SOFT_ATOMIC_SRAM_END;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.begin = P4_SOFT_ATOMIC_SRAM_END - 4u;
    region.end = P4_SOFT_ATOMIC_SRAM_END;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.end = P4_SOFT_ATOMIC_SRAM_END + 4u;
    CHECK(!p4_soft_atomic_region_canonical(&region));

    region.kind = P4_SOFT_ATOMIC_REGION_PSRAM;
    region.begin = P4_SOFT_ATOMIC_PSRAM_BASE;
    region.end = P4_SOFT_ATOMIC_PSRAM_END;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.begin = P4_SOFT_ATOMIC_PSRAM_END - 16u;
    region.end = P4_SOFT_ATOMIC_PSRAM_END;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.end = P4_SOFT_ATOMIC_PSRAM_END + 1u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    region.begin = (uintptr_t)0x88000000u;
    region.end = region.begin + 64u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    region.begin = (uintptr_t)0x4ff40000u;
    region.end = region.begin + 64u;
    region.kind = P4_SOFT_ATOMIC_REGION_SRAM;
    CHECK(!p4_soft_atomic_region_canonical(&region));

    region.kind = P4_SOFT_ATOMIC_REGION_SRAM_HIGH;
    test_high_cache_config = 0u;
    region.end = (uintptr_t)0x4ffa0000u;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.end++;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    region.begin = (uintptr_t)0x4ff3fffcu;
    region.end = (uintptr_t)0x4ff40004u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    test_high_cache_config = 1u;
    region.begin = (uintptr_t)0x4ff40000u;
    region.end = (uintptr_t)0x4ff80000u;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.begin = region.end - 4u;
    CHECK(p4_soft_atomic_region_canonical(&region));
    region.end++;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    test_high_cache_config = 2u;
    region.begin = (uintptr_t)0x4ff40000u;
    region.end = region.begin + 4u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    test_high_cache_config = 3u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    test_high_cache_config = 0u;
    region.begin = (uintptr_t)0x8ff40000u;
    region.end = region.begin + 4u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    test_high_cache_config = 2u;
    region.kind = 99u;
    CHECK(!p4_soft_atomic_region_canonical(&region));
    return 1;
}

static int test_prepare_gates_and_no_reinitialization(void)
{
    struct p4_soft_atomic_policy invalid;
    uint32_t value = 0x5a5aa5a5u;

    CHECK(krnP4SoftAtomicPrepare(NULL) == P4_SOFT_ATOMIC_BAD_ARGUMENT);
    test_storage.words[0] = 0x11223344u;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &value) == P4_SOFT_ATOMIC_NOT_PREPARED);
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[0], 4u,
                                          0x11223344u, 0x55667788u,
                                          &value) ==
          P4_SOFT_ATOMIC_NOT_PREPARED);
    CHECK(test_storage.words[0] == 0x11223344u);
    CHECK(value == 0x5a5aa5a5u);

    CHECK(krnP4SoftAtomicPrepare((const struct p4_soft_atomic_policy *)
              ((uintptr_t)&test_policy + 1u)) == P4_SOFT_ATOMIC_BAD_ARGUMENT);
    test_hart = 1u;
    test_irq_mie = 0u;
    CHECK(krnP4SoftAtomicPrepare(&test_policy) == P4_SOFT_ATOMIC_BAD_CONTEXT);
    CHECK(test_irq_mie == 0u);

    test_hart = 0u;
    test_irq_mie = 8u;
    CHECK(krnP4SoftAtomicPrepare(&test_policy) == P4_SOFT_ATOMIC_BAD_CONTEXT);
    CHECK(test_irq_mie == 8u);

    test_irq_mie = 0u;
    invalid = test_policy;
    invalid.region_count = P4_SOFT_ATOMIC_MAX_REGIONS + 1u;
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);

    invalid = test_policy;
    invalid.max_attempts = 0u;
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);
    invalid = test_policy;
    invalid.max_cycles = 0x80000000u;
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);

    invalid = test_policy;
    invalid.region_count = 2u;
    invalid.regions[1] = invalid.regions[0];
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);

    test_policy_admission_enabled = 0u;
    CHECK(krnP4SoftAtomicPrepare(&test_policy) == P4_SOFT_ATOMIC_BAD_POLICY);
    test_policy_admission_enabled = 1u;

    invalid = test_policy;
    invalid.region_count = 2u;
    invalid.regions[1].kind = P4_SOFT_ATOMIC_REGION_SRAM_HIGH;
    invalid.regions[1].begin = (uintptr_t)0x4ff40000u;
    invalid.regions[1].end = (uintptr_t)0x4ffa0001u;
    test_high_cache_config = 0u;
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);
    CHECK(p4_soft_atomic_ready == P4_SOFT_ATOMIC_READY_EMPTY);
    CHECK(p4_soft_atomic_active_policy.region_count == 0u);
    invalid.regions[1].end = (uintptr_t)0x4ff40004u;
    test_high_cache_config = 2u;
    CHECK(krnP4SoftAtomicPrepare(&invalid) == P4_SOFT_ATOMIC_BAD_POLICY);
    CHECK(p4_soft_atomic_ready == P4_SOFT_ATOMIC_READY_EMPTY);
    CHECK(p4_soft_atomic_active_policy.region_count == 0u);
    test_high_cache_config = 0u;
    test_policy.region_count = 2u;
    test_policy.regions[1] = invalid.regions[1];
    test_policy.regions[1].end = (uintptr_t)0x4ffa0000u;
    CHECK(krnP4SoftAtomicPrepare(&test_policy) == P4_SOFT_ATOMIC_OK);
    CHECK(test_irq_mie == 0u);
    CHECK(p4_soft_atomic_ready == P4_SOFT_ATOMIC_READY);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(p4_soft_atomic_active_policy.regions[1].end == 0x4ffa0000u);
    CHECK(krnP4SoftAtomicAddressAllowed((volatile void *)
                                     (uintptr_t)0x4ff9fffcu, 4u));
    CHECK(!krnP4SoftAtomicAddressAllowed((volatile void *)
                                      (uintptr_t)0x4ffa0000u, 4u));
    test_high_cache_config = 2u;

    /* Policy storage is copied, not retained by reference. */
    test_policy.regions[0].begin = 0;
    test_policy.regions[0].end = 0;
    test_storage.words[0] = 0x12345678u;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_LOAD, 0u,
                                  &value) == P4_SOFT_ATOMIC_OK);
    CHECK(value == 0x12345678u);
    test_policy.regions[0].begin = (uintptr_t)test_storage.bytes;
    test_policy.regions[0].end = (uintptr_t)test_storage.bytes +
                                 TEST_STORAGE_BYTES - 1u;

    p4_soft_atomic_lock.lock = SPINLOCKF_WRITE;
    CHECK(krnP4SoftAtomicPrepare(&test_policy) ==
          P4_SOFT_ATOMIC_ALREADY_PREPARED);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCKF_WRITE);
    p4_soft_atomic_lock.lock = SPINLOCK_UNLOCKED;
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static int test_widths_and_return_values(void)
{
    uint32_t old_value;

    memset(&test_storage, 0, sizeof(test_storage));

    test_storage.bytes[0] = 0xfeu;
    old_value = 0xdeadbeefu;
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_LOAD, 0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xfeu && test_storage.bytes[0] == 0xfeu);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_STORE, 0x123u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xfeu && test_storage.bytes[0] == 0x23u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_ADD, 2u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x23u && test_storage.bytes[0] == 0x25u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_AND, 0x0fu,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x25u && test_storage.bytes[0] == 0x05u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_OR, 0xf0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x05u && test_storage.bytes[0] == 0xf5u);
    test_storage.bytes[0] = 0xffu;
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[0], 1u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xffu && test_storage.bytes[0] == 0u);

    test_storage.halves[2] = 0xfffeu;
    CHECK(krnP4SoftAtomicOperate(&test_storage.halves[2], 2u,
                                  P4_SOFT_ATOMIC_LOAD, 0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xfffeu);
    CHECK(krnP4SoftAtomicOperate(&test_storage.halves[2], 2u,
                                  P4_SOFT_ATOMIC_STORE, 0x12345u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xfffeu && test_storage.halves[2] == 0x2345u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.halves[2], 2u,
                                  P4_SOFT_ATOMIC_ADD, 0xfffeu,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x2345u && test_storage.halves[2] == 0x2343u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.halves[2], 2u,
                                  P4_SOFT_ATOMIC_AND, 0x00f0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x2343u && test_storage.halves[2] == 0x0040u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.halves[2], 2u,
                                  P4_SOFT_ATOMIC_OR, 0xf000u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x0040u && test_storage.halves[2] == 0xf040u);

    test_storage.words[4] = UINT32_MAX;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[4], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == UINT32_MAX && test_storage.words[4] == 0u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[4], 4u,
                                  P4_SOFT_ATOMIC_OR, 0xa5a5f00fu,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0u && test_storage.words[4] == 0xa5a5f00fu);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[4], 4u,
                                  P4_SOFT_ATOMIC_AND, 0x0ff00ff0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xa5a5f00fu &&
          test_storage.words[4] == 0x05a00000u);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[4], 4u,
                                  P4_SOFT_ATOMIC_STORE, 0xfeedbeefu,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x05a00000u &&
          test_storage.words[4] == 0xfeedbeefu);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[4], 4u,
                                  P4_SOFT_ATOMIC_LOAD, 0u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xfeedbeefu);

    CHECK(test_irq_mie == 0u);
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static int test_compare_exchange_widths(void)
{
    uint32_t old_value = 0xdeadbeefu;

    test_storage.bytes[0] = 0x23u;
    test_irq_mie = 8u;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.bytes[0], 1u,
                                          0x123u, 0x456u,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x23u && test_storage.bytes[0] == 0x56u);
    CHECK(test_irq_mie == 8u);

    old_value = 0xdeadbeefu;
    test_irq_mie = 0u;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.bytes[0], 1u,
                                          0x157u, 0xaau,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x56u && test_storage.bytes[0] == 0x56u);
    CHECK(test_irq_mie == 0u);

    test_storage.halves[3] = 0xabcdu;
    old_value = 0xdeadbeefu;
    test_irq_mie = 8u;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.halves[3], 2u,
                                          0x1abcdu, 0x23456u,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xabcdu && test_storage.halves[3] == 0x3456u);
    CHECK(test_irq_mie == 8u);

    old_value = 0xdeadbeefu;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.halves[3], 2u,
                                          0x3457u, 0x9999u,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x3456u && test_storage.halves[3] == 0x3456u);
    CHECK(test_irq_mie == 8u);

    test_storage.words[5] = 0x11223344u;
    old_value = 0xdeadbeefu;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[5], 4u,
                                          0x11223344u, 0xaabbccddu,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0x11223344u &&
          test_storage.words[5] == 0xaabbccddu);

    old_value = 0xdeadbeefu;
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[5], 4u,
                                          0xaabbccdeu, 0x76543210u,
                                          &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 0xaabbccddu &&
          test_storage.words[5] == 0xaabbccddu);
    CHECK(test_irq_mie == 8u);
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static int test_invalid_input_and_placement(void)
{
    uint32_t old_value = 0xdecafbad;
    uint32_t original = 0x10203040u;
    uint32_t loads_before;
    uint8_t misaligned_before;

    test_storage.words[0] = original;
    misaligned_before = test_storage.bytes[1];
    CHECK(krnP4SoftAtomicOperate(NULL, 4u, P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  (enum p4_soft_atomic_operation)99,
                                  1u, &old_value) ==
          P4_SOFT_ATOMIC_BAD_OPERATION);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 3u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_WIDTH);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[1], 2u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) ==
          P4_SOFT_ATOMIC_BAD_ALIGNMENT);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  NULL) == P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  (uint32_t *)(void *)&test_storage.bytes[1]) ==
          P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &test_storage.words[0]) ==
          P4_SOFT_ATOMIC_BAD_ARGUMENT);

    CHECK(krnP4SoftAtomicCompareExchange(NULL, 4u, original, 0u,
                                          &old_value) ==
          P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(old_value == 0xdecafbad && test_storage.words[0] == original);
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[0], 3u,
                                          original, 0u,
                                          &old_value) ==
          P4_SOFT_ATOMIC_BAD_WIDTH);
    CHECK(old_value == 0xdecafbad && test_storage.words[0] == original);
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.bytes[1], 2u,
                                          0u, 1u,
                                          &old_value) ==
          P4_SOFT_ATOMIC_BAD_ALIGNMENT);
    CHECK(old_value == 0xdecafbad &&
          test_storage.bytes[1] == misaligned_before);
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[0], 4u,
                                          original, 0u, NULL) ==
          P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(krnP4SoftAtomicCompareExchange(&test_storage.words[0], 4u,
                                          original, 0u,
                                          &test_storage.words[0]) ==
          P4_SOFT_ATOMIC_BAD_ARGUMENT);
    CHECK(krnP4SoftAtomicCompareExchange(
              (volatile void *)(uintptr_t)0x48000000u, 4u,
              0u, 1u, &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(old_value == 0xdecafbad && test_storage.words[0] == original);
    test_storage.bytes[TEST_STORAGE_BYTES - 1u] = 0x7au;
    CHECK(krnP4SoftAtomicCompareExchange(
              &test_storage.bytes[TEST_STORAGE_BYTES - 1u], 1u,
              0x7au, 0x55u, &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(old_value == 0xdecafbad &&
          test_storage.bytes[TEST_STORAGE_BYTES - 1u] == 0x7au);

    loads_before = test_spin_load_count;
    CHECK(krnP4SoftAtomicOperate(
              (volatile void *)(uintptr_t)0x48000000u, 4u,
              P4_SOFT_ATOMIC_ADD, 1u, &old_value) ==
          P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[TEST_STORAGE_BYTES / 4u],
                                  4u, P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[
                                      TEST_STORAGE_BYTES / 4u - 1u],
                                  4u, P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(krnP4SoftAtomicOperate(&test_storage.bytes[TEST_STORAGE_BYTES - 1u],
                                  1u, P4_SOFT_ATOMIC_LOAD, 0u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(krnP4SoftAtomicOperate(
              (volatile void *)(uintptr_t)(UINTPTR_MAX - 3u), 4u,
              P4_SOFT_ATOMIC_LOAD, 0u, &old_value) ==
          P4_SOFT_ATOMIC_BAD_ADDRESS);
    CHECK(p4_soft_atomic_target_allowed(
              (uintptr_t)&test_storage.words[0], 4u));
    CHECK(!p4_soft_atomic_target_allowed(
              (uintptr_t)&test_storage.words[TEST_STORAGE_BYTES / 4u], 4u));
    CHECK(!p4_soft_atomic_target_allowed(
              (uintptr_t)&test_storage.words[0], 3u));
    CHECK(test_storage.words[0] == original);
    CHECK(old_value == 0xdecafbad);
    CHECK(test_spin_load_count == loads_before);

    p4_soft_atomic_active_policy.region_count = 0u;
    CHECK(!krnP4SoftAtomicAddressAllowed(&test_storage.words[0], 4u));
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[0], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_BAD_ADDRESS);
    p4_soft_atomic_active_policy.region_count = 1u;
    CHECK(test_storage.words[0] == original);
    CHECK(old_value == 0xdecafbad);
    return 1;
}

static int test_irq_restore_and_backend_retry(void)
{
    uint32_t old_value = 0;
    uint32_t initial;

    test_storage.words[1] = 40u;
    test_irq_mie = 8u;
    initial = test_irq_restore_count;
    test_fail_acquire_cas = 1u;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[1], 4u,
                                  P4_SOFT_ATOMIC_ADD, 2u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 40u && test_storage.words[1] == 42u);
    CHECK(test_irq_mie == 8u);
    CHECK(test_irq_restore_count == initial + 1u);

    test_irq_mie = 0u;
    test_fail_unlock_cas = 2u;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[1], 4u,
                                  P4_SOFT_ATOMIC_ADD, 3u,
                                  &old_value) == P4_SOFT_ATOMIC_OK);
    CHECK(old_value == 42u && test_storage.words[1] == 45u);
    CHECK(test_irq_mie == 0u);
    CHECK(test_fail_unlock_cas == 0u);
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static int test_retry_and_cycle_timeouts(void)
{
    uint32_t old_value = 0x87654321u;
    uint32_t before;
    uint32_t attempts = p4_soft_atomic_active_policy.max_attempts;
    uint32_t cycles = p4_soft_atomic_active_policy.max_cycles;

    test_storage.words[2] = 0x13579bdfu;
    p4_soft_atomic_lock.lock = SPINLOCKF_WRITE;
    p4_soft_atomic_active_policy.max_attempts = 7u;
    p4_soft_atomic_active_policy.max_cycles = 100000u;
    test_spin_load_count = 0;
    test_cycles = 0u;
    test_cycle_step = 1u;
    test_irq_mie = 8u;
    before = test_spin_load_count;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[2], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_TIMEOUT);
    CHECK(test_spin_load_count - before == 7u);
    CHECK(test_storage.words[2] == 0x13579bdfu);
    CHECK(old_value == 0x87654321u);
    CHECK(test_irq_mie == 8u);

    p4_soft_atomic_active_policy.max_attempts = 500u;
    p4_soft_atomic_active_policy.max_cycles = 8u;
    test_spin_load_count = 0;
    test_cycles = 0u;
    test_cycle_step = 1u;
    test_irq_mie = 0u;
    CHECK(krnP4SoftAtomicOperate(&test_storage.words[2], 4u,
                                  P4_SOFT_ATOMIC_ADD, 1u,
                                  &old_value) == P4_SOFT_ATOMIC_TIMEOUT);
    CHECK(test_spin_load_count > 0u && test_spin_load_count < 500u);
    CHECK(test_storage.words[2] == 0x13579bdfu);
    CHECK(old_value == 0x87654321u);
    CHECK(test_irq_mie == 0u);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCKF_WRITE);

    p4_soft_atomic_lock.lock = SPINLOCK_UNLOCKED;
    p4_soft_atomic_active_policy.max_attempts = attempts;
    p4_soft_atomic_active_policy.max_cycles = cycles;
    return 1;
}

struct contention_arg
{
    uint32_t writer;
    uint32_t iterations;
};

static uint32_t worker_start;
static uint32_t worker_errors;
static uint32_t seen_old_values[TEST_TOTAL_WRITES];
static uint32_t test_contention_failed_acquires;
static uint32_t cas_worker_errors;
static uint32_t cas_retry_count;
static uint32_t cas_first_loaders;

static void *contention_worker(void *opaque)
{
    const struct contention_arg *arg = opaque;
    uint32_t i, last = 0;

    test_irq_mie = 8u;
    test_cycles = 0u;
    test_cycle_step = 1u;
    while (__atomic_load_n(&worker_start, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();

    for (i = 0; i < arg->iterations; ++i)
    {
        uint32_t old_value = 0;
        enum p4_soft_atomic_result result;

        if (arg->writer)
        {
            result = krnP4SoftAtomicOperate(&test_storage.words[8], 4u,
                                            P4_SOFT_ATOMIC_ADD, 1u,
                                            &old_value);
            if (result != P4_SOFT_ATOMIC_OK || old_value >= TEST_TOTAL_WRITES)
            {
                __atomic_add_fetch(&worker_errors, 1u, __ATOMIC_RELAXED);
                return NULL;
            }

            if (__atomic_exchange_n(&seen_old_values[old_value], 1u,
                                    __ATOMIC_ACQ_REL) != 0u)
                __atomic_add_fetch(&worker_errors, 1u, __ATOMIC_RELAXED);
        }
        else
        {
            result = krnP4SoftAtomicOperate(&test_storage.words[8], 4u,
                                            P4_SOFT_ATOMIC_LOAD, 0u,
                                            &old_value);
            if (result != P4_SOFT_ATOMIC_OK ||
                old_value < last || old_value > TEST_TOTAL_WRITES)
                __atomic_add_fetch(&worker_errors, 1u, __ATOMIC_RELAXED);
            last = old_value;
        }

        if (test_irq_mie != 8u)
            __atomic_add_fetch(&worker_errors, 1u, __ATOMIC_RELAXED);
    }

    return NULL;
}

static int test_pthread_reader_writer_contention(void)
{
    pthread_t threads[TEST_WRITERS + TEST_READERS];
    struct contention_arg args[TEST_WRITERS + TEST_READERS];
    uint32_t i;

    memset(seen_old_values, 0, sizeof(seen_old_values));
    test_storage.words[8] = 0u;
    worker_start = 0u;
    worker_errors = 0u;
    test_spin_busy_count = 0u;

    for (i = 0; i < TEST_WRITERS + TEST_READERS; ++i)
    {
        args[i].writer = i < TEST_WRITERS;
        args[i].iterations = args[i].writer ? TEST_WRITER_ITERATIONS
                                             : TEST_READER_ITERATIONS;
        CHECK(pthread_create(&threads[i], NULL, contention_worker,
                             &args[i]) == 0);
    }
    __atomic_store_n(&worker_start, 1u, __ATOMIC_RELEASE);

    for (i = 0; i < TEST_WRITERS + TEST_READERS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);

    CHECK(__atomic_load_n(&worker_errors, __ATOMIC_ACQUIRE) == 0u);
    CHECK(test_storage.words[8] == TEST_TOTAL_WRITES);
    CHECK(__atomic_load_n(&test_spin_busy_count, __ATOMIC_RELAXED) >
          TEST_TOTAL_OPERATIONS);
    test_contention_failed_acquires =
        __atomic_load_n(&test_spin_busy_count, __ATOMIC_RELAXED) -
        TEST_TOTAL_OPERATIONS;
    for (i = 0; i < TEST_TOTAL_WRITES; ++i)
        CHECK(__atomic_load_n(&seen_old_values[i], __ATOMIC_ACQUIRE) == 1u);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static void *cas_increment_worker(void *opaque)
{
    const struct contention_arg *arg = opaque;
    uint32_t i;

    test_irq_mie = 8u;
    test_cycles = 0u;
    test_cycle_step = 1u;
    while (__atomic_load_n(&worker_start, __ATOMIC_ACQUIRE) == 0u)
        sched_yield();

    for (i = 0; i < arg->iterations; ++i)
    {
        uint32_t expected = 0u;
        enum p4_soft_atomic_result result;

        result = krnP4SoftAtomicOperate(&test_storage.words[10], 4u,
                                        P4_SOFT_ATOMIC_LOAD, 0u, &expected);
        if (result != P4_SOFT_ATOMIC_OK || test_irq_mie != 8u)
        {
            __atomic_add_fetch(&cas_worker_errors, 1u, __ATOMIC_RELAXED);
            return NULL;
        }

        if (i == 0u)
        {
            __atomic_add_fetch(&cas_first_loaders, 1u, __ATOMIC_ACQ_REL);
            while (__atomic_load_n(&cas_first_loaders, __ATOMIC_ACQUIRE) <
                   TEST_CAS_WRITERS)
                sched_yield();
        }

        for (;;)
        {
            uint32_t observed = UINT32_MAX;

            result = krnP4SoftAtomicCompareExchange(
                &test_storage.words[10], 4u, expected, expected + 1u,
                &observed);
            if (result != P4_SOFT_ATOMIC_OK || test_irq_mie != 8u)
            {
                __atomic_add_fetch(&cas_worker_errors, 1u,
                                   __ATOMIC_RELAXED);
                return NULL;
            }

            if (observed == expected)
                break;

            expected = observed;
            __atomic_add_fetch(&cas_retry_count, 1u, __ATOMIC_RELAXED);
        }

        if ((i & 7u) == 0u)
            sched_yield();
    }

    return NULL;
}

static int test_pthread_compare_exchange_increments(void)
{
    pthread_t threads[TEST_CAS_WRITERS];
    struct contention_arg args[TEST_CAS_WRITERS];
    uint32_t i;

    test_storage.words[10] = 0u;
    worker_start = 0u;
    cas_worker_errors = 0u;
    cas_retry_count = 0u;
    cas_first_loaders = 0u;

    for (i = 0; i < TEST_CAS_WRITERS; ++i)
    {
        args[i].writer = 1u;
        args[i].iterations = TEST_CAS_ITERATIONS;
        CHECK(pthread_create(&threads[i], NULL, cas_increment_worker,
                             &args[i]) == 0);
    }
    __atomic_store_n(&worker_start, 1u, __ATOMIC_RELEASE);

    for (i = 0; i < TEST_CAS_WRITERS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);

    CHECK(__atomic_load_n(&cas_worker_errors, __ATOMIC_ACQUIRE) == 0u);
    CHECK(test_storage.words[10] == TEST_TOTAL_CAS_WRITES);
    CHECK(__atomic_load_n(&cas_retry_count, __ATOMIC_RELAXED) > 0u);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCK_UNLOCKED);
    CHECK(test_bad_irq_restore == 0u);
    return 1;
}

static int test_release_failure_is_fail_closed(void)
{
    uint32_t restore_before = test_irq_restore_count;

    test_fatal_old_value = 0xcafebabeu;

    test_storage.words[9] = 10u;
    test_irq_mie = 8u;
    test_cycles = 0u;
    test_cycle_step = 0u;
    test_fail_unlock_cas = p4_soft_atomic_active_policy.max_attempts + 1u;
    test_fatal_reason = 0u;
    test_fatal_mie = 0xffffffffu;

    if (setjmp(test_fatal_jump) == 0)
    {
        (void)krnP4SoftAtomicOperate(&test_storage.words[9], 4u,
                                     P4_SOFT_ATOMIC_ADD, 1u,
                                     &test_fatal_old_value);
        CHECK(0 && "fatal release failure unexpectedly returned");
    }

    CHECK(test_fatal_reason == P4_SOFT_ATOMIC_LOCK_ERROR);
    CHECK(test_fatal_mie == 0u && test_irq_mie == 0u);
    CHECK(test_irq_restore_count == restore_before);
    CHECK(p4_soft_atomic_lock.lock == SPINLOCKF_WRITE);
    CHECK(test_storage.words[9] == 11u);
    CHECK(test_fatal_old_value == 0xcafebabeu);
    CHECK(test_bad_irq_restore == 0u);

    /* The test harness recovers the deliberately fail-closed fixture. */
    p4_soft_atomic_lock.lock = SPINLOCK_UNLOCKED;
    test_irq_mie = 8u;
    return 1;
}

int main(void)
{
    prepare_policy();

    if (!test_canonical_region_boundaries() ||
        !test_prepare_gates_and_no_reinitialization() ||
        !test_widths_and_return_values() ||
        !test_compare_exchange_widths() ||
        !test_invalid_input_and_placement() ||
        !test_irq_restore_and_backend_retry() ||
        !test_retry_and_cycle_timeouts() ||
        !test_pthread_reader_writer_contention() ||
        !test_pthread_compare_exchange_increments() ||
        !test_release_failure_is_fail_closed())
        return 1;

    printf("PASS P4 soft-atomic host test (%u checks; failed acquires=%u; "
           "CAS retries=%u; backend CAS=%u)\n",
           test_check_count,
           test_contention_failed_acquires,
           __atomic_load_n(&cas_retry_count, __ATOMIC_RELAXED),
           __atomic_load_n(&test_spin_cas_count, __ATOMIC_RELAXED));
    return 0;
}
