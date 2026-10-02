#include <assert.h>
#include <stdio.h>
#include <string.h>
#define P4_SECONDARY_HOST_TEST 1
#include "../secondary_probe.c"
#ifdef P4_E2_MAILBOX
struct p4_mailbox __p4_secondary_mailbox;
unsigned char __p4_secondary_worker_start[4], __p4_secondary_worker_end[4];
static struct p4_mail_state worker;
#endif

unsigned char __p4_secondary_entry[4], __p4_secondary_entry_end[4];
uint32_t __p4_secondary_report[16], __p4_secondary_stack_end[4];
uint32_t __p4_secondary_guard_low[16], __p4_secondary_guard_high[16];
uint32_t __p4_secondary_boot_snapshot[8];
static uint32_t registers[4], tick;
static unsigned int mode, reads;
#ifdef P4_E2_PRIMITIVES
#define PRIMITIVE_LOG_CAPACITY 4u
static unsigned int primitive_prepare_calls, primitive_run_calls;
static unsigned int primitive_teardown_calls;
static uint32_t primitive_prepare_nonce[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_run_nonce[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_prepare_reset_held[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_prepare_clock_off[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_prepare_boot_zero[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_teardown_reset_held[PRIMITIVE_LOG_CAPACITY];
static uint32_t primitive_teardown_clock_off[PRIMITIVE_LOG_CAPACITY];
static unsigned int primitive_check_count;
#define PRIMITIVE_CHECK(condition) do { \
    ++primitive_check_count; \
    assert(condition); \
} while (0)
#endif

static unsigned int index_of(uint32_t address)
{
    switch (address)
    {
        case P4_SECONDARY_CLOCK_REG: return 0;
        case P4_SECONDARY_RESET_REG: return 1;
        case P4_SECONDARY_STALL_REG: return 2;
        case P4_SECONDARY_BOOT_REG: return 3;
        default: assert(0); return 0;
    }
}
static uint32_t p4_r32(uint32_t address)
{ return registers[index_of(address)]; }
static void p4_w32(uint32_t address, uint32_t value)
{
    if (mode == 7 && address == P4_SECONDARY_RESET_REG)
        value &= ~P4_SECONDARY_RESET_BIT;
    registers[index_of(address)] = value;
#ifdef P4_E2_MAILBOX
    if (address == P4_SECONDARY_RESET_REG && (value & P4_SECONDARY_RESET_BIT))
        memset(&worker, 0, sizeof worker);
#endif
}
static uint32_t mock_cycles(void)
{
    ++reads;
    if (registers[3] && mode != 5 && mode != 6)
    {
        assert(registers[0] & P4_SECONDARY_CLOCK_BIT);
        assert(!(registers[1] & P4_SECONDARY_RESET_BIT));
        assert((registers[2] & P4_SECONDARY_STALL_MASK) == P4_SECONDARY_UNSTALL);
        __p4_secondary_report[0] = mode == 1 ? 0 : 1;
        __p4_secondary_report[1] = mode == 3 ? 2 : 1;
        __p4_secondary_report[5] = __p4_secondary_report[4] + (mode == 2);
        __p4_secondary_report[6] =
            (uint32_t)(uintptr_t)__p4_secondary_stack_end + (mode == 8 ? 16 : 0);
        if (mode == 4) __p4_secondary_guard_low[0] = 0;
#ifdef P4_E2_MAILBOX
        if (mode != 9 && p4_mail_step(&__p4_secondary_mailbox, &worker,
                                    __p4_secondary_report[4]))
        {
            if (mode == 10) __p4_secondary_mailbox.response[3] ^= 1;
            if (mode == 11) __p4_secondary_mailbox.response[2] += 1;
        }
#endif
    }
    if (mode != 6) tick += 1000000;
    return tick;
}
static void initialize(unsigned int next_mode)
{
    mode = next_mode; reads = 0; tick = 0xffff0000; /* wrap counter-probe */
    registers[0] = 0xa5a50008; /* core0 clock stays on */
    registers[1] = 0x5a5a0000;
    registers[2] = 0x12ab3456; /* core0 stall field must survive */
    registers[3] = 0;
    memset(__p4_secondary_boot_snapshot, 0, sizeof __p4_secondary_boot_snapshot);
    __p4_secondary_boot_snapshot[2] = P4_SECONDARY_RESET_BIT;
    memset(__p4_secondary_report, 0xff, sizeof __p4_secondary_report);
#ifdef P4_E2_PRIMITIVES
    primitive_prepare_calls = 0;
    primitive_run_calls = 0;
    primitive_teardown_calls = 0;
    memset(primitive_prepare_nonce, 0, sizeof primitive_prepare_nonce);
    memset(primitive_run_nonce, 0, sizeof primitive_run_nonce);
    memset(primitive_prepare_reset_held, 0,
           sizeof primitive_prepare_reset_held);
    memset(primitive_prepare_clock_off, 0,
           sizeof primitive_prepare_clock_off);
    memset(primitive_prepare_boot_zero, 0,
           sizeof primitive_prepare_boot_zero);
    memset(primitive_teardown_reset_held, 0,
           sizeof primitive_teardown_reset_held);
    memset(primitive_teardown_clock_off, 0,
           sizeof primitive_teardown_clock_off);
#endif
}
static void assert_stopped(void)
{
    assert(registers[0] == 0xa5a50008);
    assert(registers[1] == (0x5a5a0000 | P4_SECONDARY_RESET_BIT));
    assert(registers[2] == ((0x12ab3456 & ~P4_SECONDARY_STALL_MASK) |
                          P4_SECONDARY_STALL));
    assert(registers[3] == 0);
}
#ifdef P4_E2_PRIMITIVES
int krnP4E2Prepare(void)
{
    unsigned int index = primitive_prepare_calls++;

    if (index < PRIMITIVE_LOG_CAPACITY)
    {
        primitive_prepare_nonce[index] = __p4_secondary_report[4];
        primitive_prepare_reset_held[index] =
            (registers[1] & P4_SECONDARY_RESET_BIT) != 0;
        primitive_prepare_clock_off[index] =
            (registers[0] & P4_SECONDARY_CLOCK_BIT) == 0;
        primitive_prepare_boot_zero[index] = registers[3] == 0;
    }

    return index < PRIMITIVE_LOG_CAPACITY &&
           primitive_prepare_reset_held[index] &&
           primitive_prepare_clock_off[index] &&
           primitive_prepare_boot_zero[index];
}

int krnP4E2Run(unsigned int epoch)
{
    unsigned int index = primitive_run_calls++;

    if (index < PRIMITIVE_LOG_CAPACITY)
        primitive_run_nonce[index] = epoch;

    return index < PRIMITIVE_LOG_CAPACITY &&
           epoch == __p4_secondary_report[4] &&
           __p4_secondary_report[8] == P4_FB_BASE - 4096u &&
           (registers[0] & P4_SECONDARY_CLOCK_BIT) != 0 &&
           (registers[1] & P4_SECONDARY_RESET_BIT) == 0 &&
           registers[3] == (uint32_t)(uintptr_t)__p4_secondary_entry;
}

void krnP4E2Teardown(void)
{
    unsigned int index = primitive_teardown_calls++;

    if (index < PRIMITIVE_LOG_CAPACITY)
    {
        primitive_teardown_reset_held[index] =
            (registers[1] & P4_SECONDARY_RESET_BIT) != 0;
        primitive_teardown_clock_off[index] =
            (registers[0] & P4_SECONDARY_CLOCK_BIT) == 0;
    }
}
#endif
int main(void)
{
    unsigned int i;
    initialize(0);
    assert(attempt(0, 10, 0) == 0); assert_stopped();
#ifdef P4_E2_PRIMITIVES
    PRIMITIVE_CHECK(primitive_prepare_calls == 0);
    PRIMITIVE_CHECK(primitive_run_calls == 0);
    PRIMITIVE_CHECK(primitive_teardown_calls == 0);
#endif
    assert(attempt(1, 11, 0) == 1); assert_stopped();
#ifdef P4_E2_PRIMITIVES
    PRIMITIVE_CHECK(primitive_prepare_calls == 1);
    PRIMITIVE_CHECK(primitive_run_calls == 1);
    PRIMITIVE_CHECK(primitive_teardown_calls == 1);
    PRIMITIVE_CHECK(primitive_prepare_nonce[0] == 11);
    PRIMITIVE_CHECK(primitive_run_nonce[0] == 11);
    PRIMITIVE_CHECK(primitive_prepare_reset_held[0]);
    PRIMITIVE_CHECK(primitive_prepare_clock_off[0]);
    PRIMITIVE_CHECK(primitive_prepare_boot_zero[0]);
    PRIMITIVE_CHECK(primitive_teardown_reset_held[0]);
    PRIMITIVE_CHECK(primitive_teardown_clock_off[0]);
#endif
    for (i = 1; i <= 4; ++i)
    {
        initialize(i); assert(attempt(1, 12, 1) == -1); assert_stopped();
    }
    initialize(5); assert(attempt(1, 13, 1) == 0); assert_stopped();
    initialize(6); assert(attempt(1, 14, 1) == 0); assert_stopped();
    assert(reads <= 1000001); /* independent iteration bound */
    initialize(7); assert(attempt(1, 15, 1) == -2);
    initialize(8); assert(attempt(1, 16, 1) == -1); assert_stopped();
    initialize(0); assert(attempt(0, 17, 1) == 0); assert_stopped();
    initialize(0); assert(attempt(1, 18, 1) == 1);
    assert(registers[0] == (0xa5a50008 | P4_SECONDARY_CLOCK_BIT));
    assert(registers[1] == (0x5a5a0000 & ~P4_SECONDARY_RESET_BIT));
    assert((registers[2] & P4_SECONDARY_STALL_MASK) == P4_SECONDARY_UNSTALL);
    assert(registers[3] == (uint32_t)(uintptr_t)__p4_secondary_entry);
    assert(quiesce()); assert_stopped();
    initialize(0);
#ifdef P4_SECONDARY_RETAIN
    assert(krnP4SecondaryProbe() == 2);
    assert(quiesce()); assert_stopped();
#else
#ifdef P4_E2_MAILBOX
    assert(krnP4SecondaryProbe() == 3); assert_stopped();
#ifdef P4_E2_PRIMITIVES
    PRIMITIVE_CHECK(primitive_prepare_calls == 2);
    PRIMITIVE_CHECK(primitive_run_calls == 2);
    PRIMITIVE_CHECK(primitive_teardown_calls == 2);
    PRIMITIVE_CHECK(primitive_prepare_nonce[0] == 0xe1000002u);
    PRIMITIVE_CHECK(primitive_prepare_nonce[1] == 0xe2000003u);
    PRIMITIVE_CHECK(primitive_run_nonce[0] == 0xe1000002u);
    PRIMITIVE_CHECK(primitive_run_nonce[1] == 0xe2000003u);
    for (i = 0; i < 2; ++i)
    {
        PRIMITIVE_CHECK(primitive_prepare_reset_held[i]);
        PRIMITIVE_CHECK(primitive_prepare_clock_off[i]);
        PRIMITIVE_CHECK(primitive_prepare_boot_zero[i]);
        PRIMITIVE_CHECK(primitive_teardown_reset_held[i]);
        PRIMITIVE_CHECK(primitive_teardown_clock_off[i]);
    }
    printf("secondary E2 primitive regression: %u checks passed\n",
           primitive_check_count);
#endif
    for (i = 9; i <= 11; ++i)
    {
        initialize(i); assert(attempt(1, 19, 0) == -1); assert_stopped();
    }
    initialize(9); assert(krnP4SecondaryProbe() == 3); assert_stopped();
    initialize(6); assert(quiesce()); mailbox_prepare();
    __p4_secondary_mailbox.response[0] = 1; /* stale nonzero ack */
    assert(!mailbox_wait(2)); /* dead cycle counter still bounded */
    assert(reads <= 1000001); assert_stopped();
#else
    assert(krnP4SecondaryProbe() == 1); assert_stopped();
#endif
#endif
    initialize(0); __p4_secondary_boot_snapshot[4] = 1;
    assert(krnP4SecondaryProbe() == 0);
    puts("secondary probe: positive, suppressed, mismatch, trap, guards, timeout, cycle wrap/dead counter, isolation failure passed");
    return 0;
}
