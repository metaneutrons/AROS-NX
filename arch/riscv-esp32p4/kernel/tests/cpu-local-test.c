/*
 * Host regression for the production ESP32-P4 CPU-local slot implementation.
 * The mock only replaces the mhartid CSR read; ownership and reset helpers
 * are compiled directly from ../cpu_local.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t test_hart_id;

static uint32_t test_read_mhartid(void)
{
    return test_hart_id;
}

#define P4_CPU_LOCAL_READ_MHARTID test_read_mhartid
#include "../cpu_local.h"
#if defined(__APPLE__)
/* The production section names are ELF syntax; Mach-O rejects them. */
#define section(name) used
#endif
#include "../cpu_local.c"
#if defined(__APPLE__)
#undef section
#endif

static unsigned long check_count;

#define CHECK(condition) do { \
    ++check_count; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s (mock hart %u)\n", \
                __LINE__, #condition, test_hart_id); \
        return 0; \
    } \
} while (0)

static int check_defaults(struct p4_cpu_local *local, uint32_t hart_id)
{
    CHECK(local != NULL);
    CHECK(local->hart_id == hart_id);
    CHECK(local->current_task == NULL);
    CHECK(local->sys_base == NULL);
    CHECK(local->kernel_base == NULL);
    CHECK(local->trap_depth == 0);
    CHECK(local->schedule_flags == 0);
    CHECK(local->id_nest == -1);
    CHECK(local->td_nest == -1);
    CHECK(local->idle_count == 0);
    CHECK(local->ticks == 0);
    CHECK(local->fpu_owner == NULL);
    return 1;
}

static int check_rejected_reset_unchanged(struct p4_cpu_local *local,
                                          uint32_t hart_id)
{
    unsigned char before[sizeof(*local)];

    if (local == NULL)
    {
        CHECK(!p4_cpu_local_reset(local, hart_id));
        return 1;
    }

    memcpy(before, local, sizeof(before));
    CHECK(!p4_cpu_local_reset(local, hart_id));
    CHECK(memcmp(before, local, sizeof(before)) == 0);
    return 1;
}

static int check_rejected_trap_unchanged(struct p4_cpu_local *local,
                                         uint32_t hart_id, int enter)
{
    unsigned char before[sizeof(*local)];
    int result;

    if (local == NULL)
    {
        result = enter ? p4_cpu_local_trap_enter(local, hart_id)
                       : p4_cpu_local_trap_leave(local, hart_id);
        CHECK(!result);
        return 1;
    }

    memcpy(before, local, sizeof(before));
    result = enter ? p4_cpu_local_trap_enter(local, hart_id)
                   : p4_cpu_local_trap_leave(local, hart_id);
    CHECK(!result);
    CHECK(memcmp(before, local, sizeof(before)) == 0);
    return 1;
}

static int test_bounds_and_initialization(void)
{
    struct p4_cpu_local dummy = {0};
    struct p4_cpu_local *local0 = p4_cpu_local_for_hart(0);
    struct p4_cpu_local *local1 = p4_cpu_local_for_hart(1);

    CHECK(local0 == &__p4_cpu_locals[0]);
    CHECK(local1 == &__p4_cpu_locals[1]);
    CHECK(local0 != local1);
    CHECK(p4_cpu_local_for_hart(2) == NULL);
    CHECK(p4_cpu_local_for_hart(UINT32_MAX) == NULL);

    test_hart_id = 0;
    CHECK(p4_cpu_local_current() == local0);
    CHECK(krnP4CpuLocalPrepare());
    CHECK(check_defaults(local0, 0));
    CHECK(check_defaults(local1, 1));

    test_hart_id = 1;
    CHECK(p4_cpu_local_current() == local1);
    test_hart_id = 2;
    CHECK(p4_cpu_local_current() == NULL);
    test_hart_id = UINT32_MAX;
    CHECK(p4_cpu_local_current() == NULL);
    test_hart_id = 0;

    CHECK(check_rejected_reset_unchanged(NULL, 0));
    CHECK(check_rejected_reset_unchanged(&dummy, 0));
    CHECK(check_rejected_reset_unchanged(local0, 2));
    CHECK(check_rejected_reset_unchanged(local0, UINT32_MAX));
    CHECK(check_rejected_reset_unchanged(local0, 1));

    /* Reset and prepare are primary-only, even when targeting hart 1. */
    test_hart_id = 1;
    CHECK(check_rejected_reset_unchanged(local1, 1));
    {
        unsigned char before0[sizeof(*local0)];
        unsigned char before1[sizeof(*local1)];

        memcpy(before0, local0, sizeof(before0));
        memcpy(before1, local1, sizeof(before1));
        CHECK(!krnP4CpuLocalPrepare());
        CHECK(memcmp(before0, local0, sizeof(before0)) == 0);
        CHECK(memcmp(before1, local1, sizeof(before1)) == 0);
    }
    test_hart_id = 0;
    return 1;
}

static int test_owner_and_failure_paths(void)
{
    struct p4_cpu_local *local0 = p4_cpu_local_for_hart(0);
    struct p4_cpu_local *local1 = p4_cpu_local_for_hart(1);
    struct p4_cpu_local dummy = {0};

    test_hart_id = 0;
    CHECK(check_rejected_trap_unchanged(local0, 0, 0)); /* underflow */
    CHECK(check_rejected_trap_unchanged(local0, 1, 1)); /* wrong argument */
    CHECK(check_rejected_trap_unchanged(NULL, 0, 1));
    CHECK(check_rejected_trap_unchanged(&dummy, 0, 1)); /* wrong slot */

    /* Correct argument, wrong executing hart: ownership must still reject. */
    test_hart_id = 1;
    CHECK(check_rejected_trap_unchanged(local0, 0, 1));

    /* The recorded owner is part of the contract, not just an index hint. */
    test_hart_id = 0;
    local0->hart_id = 1;
    CHECK(check_rejected_trap_unchanged(local0, 0, 1));
    local0->hart_id = 0;

    /* Overflow and underflow refusals preserve the complete slot. */
    local0->trap_depth = UINT32_MAX;
    CHECK(check_rejected_trap_unchanged(local0, 0, 1));
    local0->trap_depth = 0;
    CHECK(check_rejected_trap_unchanged(local0, 0, 0));

    /* Hart 1 may own slot 1 after primary initialization, but may not reset it. */
    test_hart_id = 1;
    CHECK(p4_cpu_local_trap_enter(local1, 1));
    CHECK(local1->trap_depth == 1);
    CHECK(check_rejected_reset_unchanged(local1, 1));
    CHECK(local1->trap_depth == 1);
    CHECK(p4_cpu_local_trap_leave(local1, 1));
    CHECK(local1->trap_depth == 0);

    test_hart_id = 0;
    return 1;
}

static int test_owner_local_mutations(void)
{
    const uint32_t iterations = 1000;
    struct p4_cpu_local *local0 = p4_cpu_local_for_hart(0);
    struct p4_cpu_local *local1 = p4_cpu_local_for_hart(1);
    uint32_t i;

    test_hart_id = 0;
    CHECK(krnP4CpuLocalPrepare());

    for (i = 0; i < iterations; ++i)
    {
        struct p4_cpu_local *local;
        uint32_t hart_id;
        int depth;

        /* Alternate owner harts so each slot is mutated only by its owner. */
        hart_id = 0;
        test_hart_id = hart_id;
        local = p4_cpu_local_current();
        CHECK(local == local0);
        local->schedule_flags = 0x51000000u | (hart_id << 16) | i;
        ++local->ticks;
        ++local->idle_count;
        for (depth = 1; depth <= 4; ++depth)
        {
            CHECK(p4_cpu_local_trap_enter(local, hart_id));
            CHECK(local->trap_depth == (uint32_t)depth);
        }
        for (depth = 3; depth >= 0; --depth)
        {
            CHECK(p4_cpu_local_trap_leave(local, hart_id));
            CHECK(local->trap_depth == (uint32_t)depth);
        }

        hart_id = 1;
        test_hart_id = hart_id;
        local = p4_cpu_local_current();
        CHECK(local == local1);
        local->schedule_flags = 0x51000000u | (hart_id << 16) | i;
        ++local->ticks;
        ++local->idle_count;
        for (depth = 1; depth <= 4; ++depth)
        {
            CHECK(p4_cpu_local_trap_enter(local, hart_id));
            CHECK(local->trap_depth == (uint32_t)depth);
        }
        for (depth = 3; depth >= 0; --depth)
        {
            CHECK(p4_cpu_local_trap_leave(local, hart_id));
            CHECK(local->trap_depth == (uint32_t)depth);
        }
    }

    CHECK(local0->ticks == iterations);
    CHECK(local0->idle_count == iterations);
    CHECK(local0->schedule_flags == (0x51000000u | (iterations - 1)));
    CHECK(local0->trap_depth == 0);
    CHECK(local1->ticks == iterations);
    CHECK(local1->idle_count == iterations);
    CHECK(local1->schedule_flags == (0x51000000u | (1u << 16) |
                                      (iterations - 1)));
    CHECK(local1->trap_depth == 0);

    /* A quiescent primary reset deterministically clears both slots. */
    test_hart_id = 0;
    CHECK(krnP4CpuLocalPrepare());
    CHECK(check_defaults(local0, 0));
    CHECK(check_defaults(local1, 1));
    return 1;
}

int main(void)
{
    if (!test_bounds_and_initialization() ||
        !test_owner_and_failure_paths() ||
        !test_owner_local_mutations())
        return 1;

    printf("PASS cpu-local host test (%lu checks)\n", check_count);
    return 0;
}
