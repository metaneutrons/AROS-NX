/* Host-side fixture for the real ESP32-P4 SDMMC ownership helpers.
 * tools/test-sdmmc-host.py inserts their current definitions from production. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef uint8_t UBYTE;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define MMIO_REGISTER_COUNT 1024U
#define WRITE_LOG_CAPACITY 64U
#define RADIO_IRQ_BITS ((1UL << 16) | (1UL << 17))

struct Task
{
    unsigned id;
};

struct SignalSemaphore
{
    struct Task *ss_Owner;
    unsigned nesting;
};

struct P4SDMMCHost;

struct P4SDMMCHost
{
    struct SignalSemaphore lock;
    ULONG generation;
    unsigned host_div;
    BOOL initialization_started;
    ULONG slot_clock[2];
    UBYTE *dma_data;
    ULONG dma_length;
    BOOL dma_faulted;
    BOOL (*reset)(struct P4SDMMCHost *);
    BOOL (*set_clock)(struct P4SDMMCHost *, unsigned, ULONG, BOOL, ULONG *);
    BOOL (*acquire)(struct P4SDMMCHost *, ULONG);
    void (*release)(struct P4SDMMCHost *);
    ULONG (*now_us)(void);
};

struct p4sd_private
{
    ULONG clock_hz;
    BOOL powered;
    BOOL host_ready;
    struct P4SDMMCHost *host;
    ULONG host_generation;
};

struct sdcard_Bus
{
    IPTR sdcb_Private;
    ULONG sdcb_BusNum;
};

#define FNAME_P4SDBUS(name) P4SDBUS_##name
#define P4SD_PRIV(bus) \
    ((struct p4sd_private *)(IPTR)(bus)->sdcb_Private)

static struct P4SDMMCHost shared_host;
static struct P4SDMMCHost sd_host;
static struct p4sd_private sd_private;
static struct sdcard_Bus sd_bus;
static struct Task task_a = { 1U };
static struct Task task_b = { 2U };
static struct Task *active_task;

static ULONG clock_value;
static ULONG clock_step;
static unsigned irq_disable_depth;
static unsigned forbid_depth;
static unsigned attempt_calls;
static unsigned release_calls;
static unsigned reschedule_calls;
static unsigned now_calls;
static unsigned bug_calls;
static unsigned test_cases;
static unsigned checks;
static unsigned release_on_reschedule;
static struct SignalSemaphore *scheduled_semaphore;
static struct Task *scheduled_owner;
static BOOL sd_acquire_result;
static unsigned sd_acquire_calls;
static unsigned sd_release_calls;
static ULONG sd_last_timeout;
static IPTR last_write_address;
static ULONG last_write_value;
struct write_event
{
    IPTR address;
    ULONG value;
};
static struct write_event write_log[WRITE_LOG_CAPACITY];
static ULONG sd_registers[MMIO_REGISTER_COUNT];
static unsigned write_calls;
static unsigned read_calls;
static unsigned gpio_calls;
static unsigned delay_calls;
static unsigned fifo_reset_calls;
static unsigned module_reset_calls;
static unsigned clock_set_calls;
static unsigned reset_callback_calls;
static unsigned reset_generation;
static unsigned clock_set_slot;
static ULONG clock_set_requested;
static BOOL clock_set_low_power;
static ULONG clock_set_result_hz;
static BOOL reset_callback_result;
static BOOL clock_set_result;

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression)                                                  \
    do                                                                      \
    {                                                                       \
        ++checks;                                                           \
        if (!(expression))                                                  \
            fail_check(#expression, __FILE__, __LINE__);                   \
    } while (0)

#define Disable() (++irq_disable_depth)
#define Enable()                                                            \
    do                                                                      \
    {                                                                       \
        if (!irq_disable_depth)                                             \
            fail_check("Enable without Disable", __FILE__, __LINE__);      \
        --irq_disable_depth;                                                \
    } while (0)
#define Forbid() (++forbid_depth)
#define Permit()                                                            \
    do                                                                      \
    {                                                                       \
        if (!forbid_depth)                                                  \
            fail_check("Permit without Forbid", __FILE__, __LINE__);       \
        --forbid_depth;                                                     \
    } while (0)

static void check_wait_context(void)
{
    CHECK(irq_disable_depth == 0U);
    CHECK(forbid_depth == 0U);
}

static ULONG mock_now_us(void)
{
    ULONG result;
    check_wait_context();
    ++now_calls;
    result = clock_value;
    clock_value += clock_step;
    return result;
}

static struct Task *FindTask(const char *name)
{
    (void)name;
    return active_task;
}

static BOOL AttemptSemaphore(struct SignalSemaphore *semaphore)
{
    ++attempt_calls;
    if (!semaphore->ss_Owner)
    {
        semaphore->ss_Owner = active_task;
        semaphore->nesting = 1U;
        return TRUE;
    }
    if (semaphore->ss_Owner == active_task)
    {
        ++semaphore->nesting;
        return TRUE;
    }
    return FALSE;
}

static void ReleaseSemaphore(struct SignalSemaphore *semaphore)
{
    CHECK(semaphore->ss_Owner == active_task);
    CHECK(semaphore->nesting != 0U);
    ++release_calls;
    if (--semaphore->nesting == 0U)
        semaphore->ss_Owner = NULL;
}

static void Reschedule(void)
{
    check_wait_context();
    ++reschedule_calls;
    if (release_on_reschedule &&
        reschedule_calls == release_on_reschedule)
    {
        struct Task *saved_task = active_task;
        CHECK(scheduled_semaphore != NULL);
        CHECK(scheduled_owner != NULL);
        active_task = scheduled_owner;
        ReleaseSemaphore(scheduled_semaphore);
        active_task = saved_task;
    }
}

static void bug(const char *message, ...)
{
    CHECK(message != NULL);
    ++bug_calls;
}

static ULONG p4sd_read(IPTR address);
static void p4sd_write(IPTR address, ULONG value);

static void p4sd_gpio_output(unsigned gpio, BOOL level)
{
    (void)gpio;
    (void)level;
    ++gpio_calls;
}

static void sdcard_Udelay(ULONG usec)
{
    (void)usec;
    ++delay_calls;
}

static BOOL p4sd_reset_fifo(struct sdcard_Bus *bus)
{
    (void)bus;
    ++fifo_reset_calls;
    return TRUE;
}

static BOOL mock_host_reset(struct P4SDMMCHost *host)
{
    CHECK(host == &sd_host);
    ++reset_callback_calls;
    ++module_reset_calls;
    host->initialization_started = TRUE;
    if (!reset_callback_result)
        return FALSE;
    host->host_div = 10U;
    host->generation = reset_generation;
    host->slot_clock[0] = 0UL;
    host->slot_clock[1] = 0UL;
    host->dma_data = NULL;
    host->dma_length = 0UL;
    host->dma_faulted = FALSE;
    return TRUE;
}

static BOOL mock_host_set_clock(struct P4SDMMCHost *host, unsigned slot,
                                ULONG requested, BOOL low_power,
                                ULONG *actual)
{
    CHECK(host == &sd_host);
    CHECK(actual != NULL);
    ++clock_set_calls;
    clock_set_slot = slot;
    clock_set_requested = requested;
    clock_set_low_power = low_power;
    if (!clock_set_result)
        return FALSE;
    *actual = clock_set_result_hz;
    host->slot_clock[slot] = *actual;
    return TRUE;
}

static BOOL mock_sd_acquire(struct P4SDMMCHost *host, ULONG timeout_us)
{
    CHECK(host == &sd_host);
    ++sd_acquire_calls;
    sd_last_timeout = timeout_us;
    return sd_acquire_result;
}

static void mock_sd_release(struct P4SDMMCHost *host)
{
    CHECK(host == &sd_host);
    ++sd_release_calls;
}

static void reset_fixture(void)
{
    memset(&shared_host, 0, sizeof(shared_host));
    memset(&sd_host, 0, sizeof(sd_host));
    memset(&sd_private, 0, sizeof(sd_private));
    memset(&sd_bus, 0, sizeof(sd_bus));
    active_task = &task_b;
    clock_value = 100U;
    clock_step = 1U;
    irq_disable_depth = 0U;
    forbid_depth = 0U;
    attempt_calls = 0U;
    release_calls = 0U;
    reschedule_calls = 0U;
    now_calls = 0U;
    bug_calls = 0U;
    release_on_reschedule = 0U;
    scheduled_semaphore = NULL;
    scheduled_owner = NULL;
    sd_acquire_result = TRUE;
    sd_acquire_calls = 0U;
    sd_release_calls = 0U;
    sd_last_timeout = 0U;
    last_write_address = 0U;
    last_write_value = 0U;
    memset(write_log, 0, sizeof(write_log));
    memset(sd_registers, 0, sizeof(sd_registers));
    write_calls = 0U;
    read_calls = 0U;
    gpio_calls = 0U;
    delay_calls = 0U;
    fifo_reset_calls = 0U;
    module_reset_calls = 0U;
    clock_set_calls = 0U;
    reset_callback_calls = 0U;
    reset_generation = 8U;
    clock_set_slot = 0U;
    clock_set_requested = 0UL;
    clock_set_low_power = FALSE;
    clock_set_result_hz = 400000UL;
    reset_callback_result = TRUE;
    clock_set_result = TRUE;
    shared_host.now_us = mock_now_us;
    sd_host.now_us = mock_now_us;
    sd_host.reset = mock_host_reset;
    sd_host.set_clock = mock_host_set_clock;
}

static void run_test(const char *name, void (*test)(void))
{
    test();
    ++test_cases;
    printf("ok - %s\n", name);
}

/* This file is a small simulator, not another copy of the implementation. */
/* INSERT_ACTUAL_SOURCE */

static ULONG p4sd_read(IPTR address)
{
    CHECK(address >= (IPTR)P4_SDMMC_BASE);
    CHECK(address < (IPTR)P4_SDMMC_BASE + sizeof(sd_registers));
    ++read_calls;
    return sd_registers[(address - (IPTR)P4_SDMMC_BASE) / sizeof(ULONG)];
}

static void p4sd_write(IPTR address, ULONG value)
{
    CHECK(address >= (IPTR)P4_SDMMC_BASE);
    CHECK(address < (IPTR)P4_SDMMC_BASE + sizeof(sd_registers));
    CHECK(write_calls < WRITE_LOG_CAPACITY);
    ++write_calls;
    last_write_address = address;
    last_write_value = value;
    write_log[write_calls - 1U].address = address;
    write_log[write_calls - 1U].value = value;
    if (address == (IPTR)(P4_SDMMC_BASE + P4SD_RINTSTS))
        sd_registers[P4SD_RINTSTS / sizeof(ULONG)] &= ~value;
    else
        sd_registers[(address - (IPTR)P4_SDMMC_BASE) / sizeof(ULONG)] = value;
}

static void test_immediate_and_nonblocking_contention(void)
{
    reset_fixture();
    CHECK(p4host_acquire(&shared_host, 0U) == TRUE);
    CHECK(shared_host.lock.ss_Owner == &task_b);
    CHECK(shared_host.lock.nesting == 1U);
    CHECK(attempt_calls == 1U);
    CHECK(now_calls == 0U);
    CHECK(reschedule_calls == 0U);

    p4host_release(&shared_host);
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 1U;
    active_task = &task_b;
    CHECK(p4host_acquire(&shared_host, 0U) == FALSE);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 1U);
    CHECK(now_calls == 0U);
    CHECK(reschedule_calls == 0U);
}

static void test_nested_same_task_acquisition(void)
{
    reset_fixture();
    CHECK(p4host_acquire(&shared_host, 0U) == TRUE);
    CHECK(p4host_acquire(&shared_host, 0U) == TRUE);
    CHECK(shared_host.lock.ss_Owner == &task_b);
    CHECK(shared_host.lock.nesting == 2U);
    p4host_release(&shared_host);
    CHECK(shared_host.lock.ss_Owner == &task_b);
    CHECK(shared_host.lock.nesting == 1U);
    p4host_release(&shared_host);
    CHECK(shared_host.lock.ss_Owner == NULL);
    CHECK(shared_host.lock.nesting == 0U);
}

static void test_contended_acquire_after_yield(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 1U;
    scheduled_semaphore = &shared_host.lock;
    scheduled_owner = &task_a;
    release_on_reschedule = 2U;

    CHECK(p4host_acquire(&shared_host, 100U) == TRUE);
    CHECK(reschedule_calls == 2U);
    CHECK(shared_host.lock.ss_Owner == &task_b);
    CHECK(shared_host.lock.nesting == 1U);
    CHECK(irq_disable_depth == 0U);
    CHECK(forbid_depth == 0U);
}

static void test_timeout_preserves_owner_and_nesting(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 2U;
    clock_step = 3U;

    CHECK(p4host_acquire(&shared_host, 10U) == FALSE);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 2U);
    CHECK(reschedule_calls == 4U);
    CHECK(release_calls == 0U);
}

static void test_timeout_across_clock_wrap(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 1U;
    clock_value = UINT32_MAX - 3U;
    clock_step = 2U;

    CHECK(p4host_acquire(&shared_host, 4U) == FALSE);
    CHECK(reschedule_calls == 2U);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 1U);
}

static void test_invalid_arguments_do_not_touch_semaphore_or_clock(void)
{
    reset_fixture();
    CHECK(p4host_acquire(&shared_host, P4_SDMMC_HOST_WAIT_US + 1U) == FALSE);
    CHECK(p4host_acquire(NULL, 1U) == FALSE);
    CHECK(attempt_calls == 0U);
    CHECK(now_calls == 0U);
    CHECK(reschedule_calls == 0U);
}

static void test_release_and_invalidate_reject_nonowner(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 2U;
    shared_host.generation = 9U;

    p4host_release(&shared_host);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 2U);
    CHECK(release_calls == 0U);
    CHECK(bug_calls == 1U);

    p4host_invalidate(&shared_host);
    CHECK(shared_host.generation == 9U);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 2U);
    CHECK(bug_calls == 2U);
}

static void test_generation_increment_skips_zero_on_wrap(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_b;
    shared_host.lock.nesting = 1U;
    shared_host.generation = UINT32_MAX;

    p4host_invalidate(&shared_host);
    CHECK(shared_host.generation == 1U);
    p4host_invalidate(&shared_host);
    CHECK(shared_host.generation == 2U);
    CHECK(shared_host.lock.ss_Owner == &task_b);
    CHECK(shared_host.lock.nesting == 1U);
}

static void test_stalled_clock_has_finite_refusal_bound(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 1U;
    clock_step = 0U;

    CHECK(p4host_acquire(&shared_host, 100U) == FALSE);
    CHECK(reschedule_calls == 100000U);
    CHECK(shared_host.lock.ss_Owner == &task_a);
    CHECK(shared_host.lock.nesting == 1U);
    CHECK(irq_disable_depth == 0U);
    CHECK(forbid_depth == 0U);
}

static void test_wait_and_yield_are_not_irq_disabled(void)
{
    reset_fixture();
    shared_host.lock.ss_Owner = &task_a;
    shared_host.lock.nesting = 1U;
    clock_step = 10U;

    CHECK(p4host_acquire(&shared_host, 20U) == FALSE);
    CHECK(now_calls > 1U);
    CHECK(reschedule_calls > 0U);
    CHECK(irq_disable_depth == 0U);
    CHECK(forbid_depth == 0U);
}

static void setup_sd_bus(BOOL ready, ULONG client_generation,
                         ULONG host_generation)
{
    sd_host.generation = host_generation;
    sd_host.initialization_started = FALSE;
    sd_host.acquire = mock_sd_acquire;
    sd_host.release = mock_sd_release;
    sd_private.host_ready = ready;
    sd_private.host = &sd_host;
    sd_private.host_generation = client_generation;
    sd_bus.sdcb_Private = (IPTR)&sd_private;
}

static void test_begin_mismatched_generation_invalidates_without_adopting(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 7U, 8U);

    CHECK(P4SDBUS_BeginTransaction(&sd_bus, TRUE) == TRUE);
    CHECK(sd_acquire_calls == 1U);
    CHECK(sd_last_timeout == P4_SDMMC_HOST_WAIT_US);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(sd_private.host_generation == 7U);
    P4SDBUS_EndTransaction(&sd_bus);
    CHECK(sd_release_calls == 1U);
}

static void test_begin_matching_generation_preserves_ready_state(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 8U, 8U);
    CHECK(P4SDBUS_BeginTransaction(&sd_bus, TRUE) == TRUE);
    CHECK(sd_last_timeout == P4_SDMMC_HOST_WAIT_US);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == 8U);
    P4SDBUS_EndTransaction(&sd_bus);
    CHECK(sd_release_calls == 1U);

    reset_fixture();
    setup_sd_bus(FALSE, 8U, 8U);
    CHECK(P4SDBUS_BeginTransaction(&sd_bus, FALSE) == TRUE);
    CHECK(sd_last_timeout == 0U);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(sd_private.host_generation == 8U);
    P4SDBUS_EndTransaction(&sd_bus);
}

static void test_begin_acquire_failure_preserves_private_state(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 7U, 8U);
    sd_acquire_result = FALSE;

    CHECK(P4SDBUS_BeginTransaction(&sd_bus, FALSE) == FALSE);
    CHECK(sd_acquire_calls == 1U);
    CHECK(sd_last_timeout == 0U);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == 7U);
    CHECK(sd_release_calls == 0U);
}

static void test_begin_without_host_preserves_private_state(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 7U, 8U);
    sd_private.host = NULL;

    CHECK(P4SDBUS_BeginTransaction(&sd_bus, FALSE) == FALSE);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == 7U);
    CHECK(sd_acquire_calls == 0U);
}

static void test_nonblocking_host_contention_leaves_state_unchanged(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 7U, 8U);
    sd_host.acquire = p4host_acquire;
    sd_host.release = p4host_release;
    sd_host.lock.ss_Owner = &task_a;
    sd_host.lock.nesting = 2U;

    CHECK(P4SDBUS_BeginTransaction(&sd_bus, FALSE) == FALSE);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == 7U);
    CHECK(sd_host.lock.ss_Owner == &task_a);
    CHECK(sd_host.lock.nesting == 2U);
    CHECK(attempt_calls == 1U);
    CHECK(now_calls == 0U);
    CHECK(reschedule_calls == 0U);
    CHECK(sd_release_calls == 0U);
}

static void check_stale_soft_reset_has_no_hardware_access(UBYTE mask)
{
    reset_fixture();
    setup_sd_bus(FALSE, 7U, 8U);
    sd_bus.sdcb_BusNum = 0U;

    p4sd_soft_reset_locked(mask, &sd_bus);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);
    CHECK(gpio_calls == 0U);
    CHECK(delay_calls == 0U);
    CHECK(fifo_reset_calls == 0U);
}

static void test_stale_cmd_data_reset_has_no_hardware_access(void)
{
    check_stale_soft_reset_has_no_hardware_access(SDHCI_RESET_CMD);
    check_stale_soft_reset_has_no_hardware_access(SDHCI_RESET_DATA);
}

static void test_full_reset_remains_explicit_reinitialization_exception(void)
{
    reset_fixture();
    setup_sd_bus(FALSE, 7U, 8U);
    sd_bus.sdcb_BusNum = 0U;

    p4sd_soft_reset_locked(SDHCI_RESET_ALL, &sd_bus);
    CHECK(module_reset_calls == 1U);
    CHECK(reset_callback_calls == 1U);
    CHECK(clock_set_calls == 1U);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(read_calls == 2U);
    CHECK(write_calls == 2U);
    CHECK(sd_host.initialization_started == TRUE);
    CHECK(sd_host.host_div == 10U);
    CHECK(sd_registers[P4SD_RST_N / sizeof(ULONG)] == 1UL);
    CHECK(sd_registers[P4SD_CTYPE / sizeof(ULONG)] == 0UL);
}

static unsigned writes_to(IPTR address)
{
    unsigned index;
    unsigned count = 0U;
    for (index = 0U; index < write_calls; ++index)
        if (write_log[index].address == address)
            ++count;
    return count;
}

static void seed_host_init_registers(void)
{
    sd_registers[P4SD_RST_N / sizeof(ULONG)] = 0xA4UL;
    sd_registers[P4SD_CTYPE / sizeof(ULONG)] =
        P4SD_CTYPE_4BIT_SLOT0 | P4SD_CTYPE_8BIT_SLOT0 | 0x90000002UL;
    sd_registers[P4SD_CTRL / sizeof(ULONG)] = 0x5A5AA5A5UL;
    sd_registers[P4SD_TMOUT / sizeof(ULONG)] = 0x12345678UL;
    sd_registers[P4SD_FIFOTH / sizeof(ULONG)] = 0x87654321UL;
}

static void test_extracted_host_init_first_owner_resets_then_adopts_generation(void)
{
    reset_fixture();
    setup_sd_bus(FALSE, 1U, 1U);
    seed_host_init_registers();

    CHECK(p4sd_host_init(&sd_bus) == TRUE);
    CHECK(reset_callback_calls == 1U);
    CHECK(module_reset_calls == 1U);
    CHECK(sd_host.initialization_started == TRUE);
    CHECK(clock_set_calls == 1U);
    CHECK(clock_set_slot == 0U);
    CHECK(clock_set_requested == P4SD_CLOCK_MIN_HZ);
    CHECK(clock_set_low_power == TRUE);
    CHECK(clock_set_result_hz == P4SD_CLOCK_MIN_HZ);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == reset_generation);
    CHECK(sd_registers[P4SD_RST_N / sizeof(ULONG)] == 0xA5UL);
    CHECK(sd_registers[P4SD_CTYPE / sizeof(ULONG)] == 0x90000002UL);
    CHECK(sd_registers[P4SD_CTRL / sizeof(ULONG)] == 0x5A5AA5A5UL);
    CHECK(sd_registers[P4SD_TMOUT / sizeof(ULONG)] == 0x12345678UL);
    CHECK(sd_registers[P4SD_FIFOTH / sizeof(ULONG)] == 0x87654321UL);
    CHECK(write_calls == 2U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_RST_N)) == 1U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_CTYPE)) == 1U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_CTRL)) == 0U);
}

static void test_extracted_host_init_join_preserves_initialized_peer(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 5U, 9U);
    sd_host.host_div = 10U;
    sd_host.initialization_started = TRUE;
    sd_host.slot_clock[1] = 12000000UL;
    sd_host.slot_clock[0] = 400000UL;
    seed_host_init_registers();
    sd_registers[P4SD_RST_N / sizeof(ULONG)] = 0xB6UL;
    sd_registers[P4SD_CTYPE / sizeof(ULONG)] =
        P4SD_CTYPE_4BIT_SLOT0 | P4SD_CTYPE_8BIT_SLOT0 | 0x90000002UL;

    CHECK(p4sd_host_init(&sd_bus) == TRUE);
    CHECK(reset_callback_calls == 0U);
    CHECK(module_reset_calls == 0U);
    CHECK(clock_set_calls == 1U);
    CHECK(sd_host.host_div == 10U);
    CHECK(sd_host.slot_clock[1] == 12000000UL);
    CHECK(sd_private.host_ready == TRUE);
    CHECK(sd_private.host_generation == 9U);
    CHECK(sd_registers[P4SD_RST_N / sizeof(ULONG)] == 0xB7UL);
    CHECK(sd_registers[P4SD_CTYPE / sizeof(ULONG)] == 0x90000002UL);
    CHECK(sd_registers[P4SD_CTRL / sizeof(ULONG)] == 0x5A5AA5A5UL);
    CHECK(sd_registers[P4SD_TMOUT / sizeof(ULONG)] == 0x12345678UL);
    CHECK(sd_registers[P4SD_FIFOTH / sizeof(ULONG)] == 0x87654321UL);
    CHECK(write_calls == 2U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_RST_N)) == 1U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_CTYPE)) == 1U);
    CHECK(writes_to((IPTR)(P4_SDMMC_BASE + P4SD_CTRL)) == 0U);
}

static void check_extracted_host_init_refusal(BOOL dma_faulted,
                                               ULONG dma_length)
{
    reset_fixture();
    setup_sd_bus(TRUE, 5U, 9U);
    sd_host.host_div = 10U;
    sd_host.initialization_started = TRUE;
    sd_host.slot_clock[1] = 12000000UL;
    sd_host.dma_faulted = dma_faulted;
    sd_host.dma_length = dma_length;
    seed_host_init_registers();

    CHECK(p4sd_host_init(&sd_bus) == FALSE);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(sd_private.host_generation == 5U);
    CHECK(reset_callback_calls == 0U);
    CHECK(module_reset_calls == 0U);
    CHECK(clock_set_calls == 0U);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);
    CHECK(sd_host.host_div == 10U);
    CHECK(sd_host.slot_clock[1] == 12000000UL);
    CHECK(sd_host.dma_faulted == dma_faulted);
    CHECK(sd_host.dma_length == dma_length);
}

static void test_extracted_host_init_refuses_faulted_or_active_dma(void)
{
    check_extracted_host_init_refusal(TRUE, 0UL);
    check_extracted_host_init_refusal(FALSE, 512UL);
}

static void test_extracted_host_init_failed_reset_does_no_slot_writes(void)
{
    reset_fixture();
    setup_sd_bus(TRUE, 5U, 9U);
    reset_callback_result = FALSE;
    seed_host_init_registers();

    CHECK(p4sd_host_init(&sd_bus) == FALSE);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(reset_callback_calls == 1U);
    CHECK(module_reset_calls == 1U);
    CHECK(sd_host.initialization_started == TRUE);
    CHECK(clock_set_calls == 0U);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);

    /* The sticky first-attempt marker prevents a later implicit reset from
     * treating a failed shared reset as a cold uninitialized host. */
    CHECK(p4sd_host_init(&sd_bus) == FALSE);
    CHECK(reset_callback_calls == 1U);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);
}

static void check_extracted_host_init_divider_refusal(unsigned host_div)
{
    reset_fixture();
    setup_sd_bus(TRUE, 5U, 9U);
    sd_host.host_div = host_div;
    sd_host.initialization_started = TRUE;
    seed_host_init_registers();

    CHECK(p4sd_host_init(&sd_bus) == FALSE);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(reset_callback_calls == 0U);
    CHECK(module_reset_calls == 0U);
    CHECK(clock_set_calls == 0U);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);
}

static void test_extracted_host_init_refuses_invalid_shared_divider(void)
{
    check_extracted_host_init_divider_refusal(0U);
    check_extracted_host_init_divider_refusal(17U);
}

static void test_stale_power_and_width_setters_have_no_hardware_access(void)
{
    reset_fixture();
    setup_sd_bus(FALSE, 7U, 8U);
    sd_bus.sdcb_BusNum = 0U;

    p4sd_set_power_locked(0U, FALSE, &sd_bus);
    p4sd_set_width_locked(4U, &sd_bus);
    CHECK(sd_private.host_ready == FALSE);
    CHECK(read_calls == 0U);
    CHECK(write_calls == 0U);
    CHECK(gpio_calls == 0U);
    CHECK(delay_calls == 0U);
}

static void test_p4sd_set_acknowledges_only_low_w1c_bits(void)
{
    reset_fixture();
    sd_registers[P4SD_RINTSTS / sizeof(ULONG)] =
        RADIO_IRQ_BITS | 0x0000FFFFUL;
    p4sd_set(P4SD_RINTSTS, UINT32_MAX);
    CHECK(write_calls == 1U);
    CHECK(last_write_address == (IPTR)(P4_SDMMC_BASE + P4SD_RINTSTS));
    CHECK(last_write_value == P4SD_INT_CMD_DATA_MASK);
    CHECK(sd_registers[P4SD_RINTSTS / sizeof(ULONG)] == RADIO_IRQ_BITS);

    sd_registers[P4SD_RINTSTS / sizeof(ULONG)] =
        RADIO_IRQ_BITS | 0x0000FFFFUL;
    p4sd_set(P4SD_RINTSTS, 0xE5FF0000UL | 0xA55AUL);
    CHECK(write_calls == 2U);
    CHECK(last_write_value == 0xA55AUL);
    CHECK(sd_registers[P4SD_RINTSTS / sizeof(ULONG)] ==
          (RADIO_IRQ_BITS | (0xFFFFUL & ~0xA55AUL)));
}

static void test_p4sd_set_preserves_unrelated_register_write(void)
{
    reset_fixture();
    p4sd_set(P4SD_CMDARG, 0xFEDCBA98UL);
    CHECK(write_calls == 1U);
    CHECK(last_write_address == (IPTR)(P4_SDMMC_BASE + P4SD_CMDARG));
    CHECK(last_write_value == 0xFEDCBA98UL);
}

int main(void)
{
    run_test("immediate and nonblocking contention", test_immediate_and_nonblocking_contention);
    run_test("nested same-task acquisition", test_nested_same_task_acquisition);
    run_test("contended acquisition after yield", test_contended_acquire_after_yield);
    run_test("timeout preserves owner and nesting", test_timeout_preserves_owner_and_nesting);
    run_test("timeout across clock wrap", test_timeout_across_clock_wrap);
    run_test("invalid arguments avoid semaphore and clock", test_invalid_arguments_do_not_touch_semaphore_or_clock);
    run_test("non-owner release and invalidation", test_release_and_invalidate_reject_nonowner);
    run_test("generation wrap skips zero", test_generation_increment_skips_zero_on_wrap);
    run_test("stalled clock refusal is bounded", test_stalled_clock_has_finite_refusal_bound);
    run_test("wait and yield keep interrupts enabled", test_wait_and_yield_are_not_irq_disabled);
    run_test("generation mismatch invalidates without adoption", test_begin_mismatched_generation_invalidates_without_adopting);
    run_test("matching generation preserves ready state", test_begin_matching_generation_preserves_ready_state);
    run_test("acquire failure preserves SD private state", test_begin_acquire_failure_preserves_private_state);
    run_test("missing host preserves SD private state", test_begin_without_host_preserves_private_state);
    run_test("nonblocking host contention preserves state", test_nonblocking_host_contention_leaves_state_unchanged);
    run_test("stale CMD/DATA reset has no hardware access", test_stale_cmd_data_reset_has_no_hardware_access);
    run_test("full reset remains an explicit reinit exception", test_full_reset_remains_explicit_reinitialization_exception);
    run_test("extracted first-owner init resets and adopts generation", test_extracted_host_init_first_owner_resets_then_adopts_generation);
    run_test("extracted join preserves initialized peer", test_extracted_host_init_join_preserves_initialized_peer);
    run_test("extracted init refuses faulted or active DMA", test_extracted_host_init_refuses_faulted_or_active_dma);
    run_test("extracted init reset failure avoids slot writes", test_extracted_host_init_failed_reset_does_no_slot_writes);
    run_test("extracted init refuses invalid shared divider", test_extracted_host_init_refuses_invalid_shared_divider);
    run_test("stale power/width setters have no hardware access", test_stale_power_and_width_setters_have_no_hardware_access);
    run_test("W1C writes preserve only low command/data bits", test_p4sd_set_acknowledges_only_low_w1c_bits);
    run_test("unrelated register write is unchanged", test_p4sd_set_preserves_unrelated_register_write);
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
