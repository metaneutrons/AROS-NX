/* Host fixture for the production ESP32-P4 shared-clock engine.
 * tools/test-clock-engine.py inserts the real clock.c function bodies. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_plan.h"
#include "registers.h"

typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define RADIO_IRQ_BITS ((1UL << 16) | (1UL << 17))
#define WRITE_LOG_CAPACITY 128U
#define ACCESS_LOG_CAPACITY 512U

struct Task
{
    unsigned id;
};

struct SignalSemaphore
{
    struct Task *ss_Owner;
};

struct P4SDMMCHost
{
    struct SignalSemaphore lock;
    ULONG (*now_us)(void);
    unsigned host_div;
    ULONG slot_clock[2];
    void (*invalidate)(struct P4SDMMCHost *);
};

struct write_event
{
    IPTR address;
    ULONG value;
};

struct access_event
{
    IPTR address;
    ULONG value;
    BOOL is_write;
};

static struct Task task_a = { 1U };
static struct Task task_b = { 2U };
static struct Task *active_task;
static struct P4SDMMCHost clock_host;
static ULONG sd_registers[256];
static ULONG hp_registers[64];
static ULONG lp_registers[32];
static struct write_event write_log[WRITE_LOG_CAPACITY];
static struct access_event access_log[ACCESS_LOG_CAPACITY];
static unsigned write_count;
static unsigned access_count;
static unsigned read_count;
static unsigned cmd_write_count;
static unsigned invalidate_count;
static unsigned test_cases;
static unsigned checks;
static ULONG timer_value;
static ULONG timer_step;
static ULONG now_call_count;
static ULONG fault_cmd_timeout_index;
static ULONG fault_hle_index;

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression)                                                   \
    do                                                                      \
    {                                                                       \
        ++checks;                                                           \
        if (!(expression))                                                  \
            fail_check(#expression, __FILE__, __LINE__);                   \
    } while (0)

static struct Task *FindTask(const char *name)
{
    (void)name;
    return active_task;
}

static ULONG mock_now_us(void)
{
    ULONG result = timer_value;
    ++now_call_count;
    timer_value += timer_step;
    return result;
}

static void mock_invalidate(struct P4SDMMCHost *target)
{
    CHECK(target == &clock_host);
    ++invalidate_count;
}

static void record_access(IPTR address, ULONG value, BOOL is_write)
{
    if (access_count == ACCESS_LOG_CAPACITY)
        return;
    access_log[access_count].address = address;
    access_log[access_count].value = value;
    access_log[access_count].is_write = is_write;
    ++access_count;
}

static ULONG *mock_register(IPTR address)
{
    if (address >= P4_SDMMC_BASE &&
        address < P4_SDMMC_BASE + sizeof(sd_registers))
        return &sd_registers[(address - P4_SDMMC_BASE) / sizeof(ULONG)];
    if (address >= P4_HP_SYS_CLKRST_BASE &&
        address < P4_HP_SYS_CLKRST_BASE + sizeof(hp_registers))
        return &hp_registers[(address - P4_HP_SYS_CLKRST_BASE) / sizeof(ULONG)];
    if (address >= P4_LP_CLKRST_BASE &&
        address < P4_LP_CLKRST_BASE + sizeof(lp_registers))
        return &lp_registers[(address - P4_LP_CLKRST_BASE) / sizeof(ULONG)];
    fail_check("MMIO address is in a modeled register window", __FILE__, __LINE__);
    return NULL;
}

static ULONG clock_read(IPTR address)
{
    ULONG value;
    ++read_count;
    value = *mock_register(address);
    record_access(address, value, FALSE);
    return value;
}

static void clock_write(IPTR address, ULONG value)
{
    ULONG *reg = mock_register(address);
    if (write_count == WRITE_LOG_CAPACITY)
        fail_check("write log has capacity", __FILE__, __LINE__);
    write_log[write_count].address = address;
    write_log[write_count].value = value;
    ++write_count;
    record_access(address, value, TRUE);

    if (address == P4_SDMMC_BASE + P4SD_RINTSTS)
    {
        /* DesignWare RINTSTS is write-one-to-clear. */
        *reg &= ~value;
    }
    else if (address == P4_SDMMC_BASE + P4SD_CMD &&
             (value & P4SD_CMD_START))
    {
        ++cmd_write_count;
        if (fault_hle_index == cmd_write_count)
            sd_registers[P4SD_RINTSTS / sizeof(ULONG)] |= P4SD_INT_HLE;
        *reg = value;
        if (fault_cmd_timeout_index != cmd_write_count)
            *reg &= ~P4SD_CMD_START;
    }
    else
    {
        *reg = value;
    }
}

/* INSERT_ACTUAL_CLOCK_SOURCE */

static void begin_case(void)
{
    ++test_cases;
}

static void setup_host(unsigned host_div, ULONG slot0_clock, ULONG slot1_clock)
{
    memset(&clock_host, 0, sizeof(clock_host));
    memset(sd_registers, 0, sizeof(sd_registers));
    memset(hp_registers, 0, sizeof(hp_registers));
    memset(lp_registers, 0, sizeof(lp_registers));
    memset(write_log, 0, sizeof(write_log));
    memset(access_log, 0, sizeof(access_log));
    write_count = 0U;
    access_count = 0U;
    read_count = 0U;
    cmd_write_count = 0U;
    invalidate_count = 0U;
    timer_value = 0U;
    timer_step = 1U;
    now_call_count = 0U;
    fault_cmd_timeout_index = 0U;
    fault_hle_index = 0U;
    active_task = &task_a;
    clock_host.lock.ss_Owner = &task_a;
    clock_host.now_us = mock_now_us;
    clock_host.host_div = host_div;
    clock_host.slot_clock[0] = slot0_clock;
    clock_host.slot_clock[1] = slot1_clock;
    clock_host.invalidate = mock_invalidate;

    sd_registers[P4SD_CLKENA / sizeof(ULONG)] = 0x00030003UL;
    sd_registers[P4SD_CLKDIV / sizeof(ULONG)] = 0xA1B2C3D4UL;
    sd_registers[P4SD_CLKSRC / sizeof(ULONG)] = 0x9ABCDEFFUL;
    sd_registers[P4SD_RINTSTS / sizeof(ULONG)] = RADIO_IRQ_BITS | 0x000000A5UL;
    hp_registers[P4_HP_SOC_CLK_CTRL1 / sizeof(ULONG)] = 0x80001234UL;
    hp_registers[P4_HP_REF_CLK_CTRL2 / sizeof(ULONG)] = 0x00000123UL;
    hp_registers[P4_HP_PERI_CLK_CTRL01 / sizeof(ULONG)] =
        0xA5000000UL | P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC;
    hp_registers[P4_HP_PERI_CLK_CTRL02 / sizeof(ULONG)] = 0xC0000055UL;
    lp_registers[P4_LP_SDMMC_RST_CTRL / sizeof(ULONG)] = 0x00543210UL;
}

static ULONG read_sd(ULONG offset)
{
    return sd_registers[offset / sizeof(ULONG)];
}

static ULONG read_hp(ULONG offset)
{
    return hp_registers[offset / sizeof(ULONG)];
}

static ULONG read_lp(ULONG offset)
{
    return lp_registers[offset / sizeof(ULONG)];
}

static unsigned writes_to(IPTR address)
{
    unsigned index;
    unsigned count = 0U;
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == address)
            ++count;
    return count;
}

static unsigned writes_in_range(IPTR base, IPTR length)
{
    unsigned index;
    unsigned count = 0U;
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address >= base &&
            write_log[index].address < base + length)
            ++count;
    return count;
}

static ULONG nth_write_to(IPTR address, unsigned wanted)
{
    unsigned index;
    unsigned found = 0U;
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == address && ++found == wanted)
            return write_log[index].value;
    fail_check("requested MMIO write exists", __FILE__, __LINE__);
    return 0U;
}

static unsigned first_write_setting(IPTR address, ULONG mask)
{
    unsigned index;
    for (index = 0U; index < access_count; ++index)
        if (access_log[index].is_write &&
            access_log[index].address == address &&
            (access_log[index].value & mask) == mask)
            return index;
    fail_check("matching MMIO write exists", __FILE__, __LINE__);
    return 0U;
}

static unsigned first_sdmmc_access(void)
{
    unsigned index;
    for (index = 0U; index < access_count; ++index)
        if (access_log[index].address >= P4_SDMMC_BASE &&
            access_log[index].address < P4_SDMMC_BASE + sizeof(sd_registers))
            return index;
    fail_check("SDMMC MMIO access exists", __FILE__, __LINE__);
    return 0U;
}

static ULONG expected_source_fields(unsigned divider, ULONG old_value)
{
    ULONG value = old_value & ~P4_HP_SDIO_FIELDS_M;
    if (divider > 1U)
        value |= ((ULONG)(divider - 1U) << P4_HP_SDIO_EDGE_L_S) |
                 ((ULONG)(divider / 2U - 1U) << P4_HP_SDIO_EDGE_H_S) |
                 ((ULONG)(divider - 1U) << P4_HP_SDIO_EDGE_N_S);
    value |= (1UL << P4_HP_SDIO_DRV_EDGE_S) | P4_HP_SDIO_SLF_EN |
             P4_HP_SDIO_DRV_EN | P4_HP_SDIO_SAM_EN;
    return value;
}

static void check_failed_transition(ULONG actual)
{
    CHECK((read_sd(P4SD_CLKENA) & 3UL) == 0UL);
    CHECK(clock_host.slot_clock[0] == 0UL);
    CHECK(clock_host.slot_clock[1] == 0UL);
    CHECK(clock_host.host_div == 0U);
    CHECK(invalidate_count == 1U);
    CHECK(actual == 0x13579BDFUL);
    CHECK((read_sd(P4SD_RINTSTS) & RADIO_IRQ_BITS) == RADIO_IRQ_BITS);
}

static void test_invalid_calls_have_no_mmio(void)
{
    ULONG actual = 0x13579BDFUL;
    begin_case();
    setup_host(10U, 0UL, 0UL);
    active_task = &task_b;
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(read_count == 0U && write_count == 0U);
    CHECK(actual == 0x13579BDFUL);

    setup_host(10U, 0UL, 0UL);
    CHECK(!p4host_set_clock(NULL, 0U, 25000000UL, FALSE, &actual));
    CHECK(read_count == 0U && write_count == 0U);
    CHECK(!p4host_set_clock(&clock_host, 2U, 25000000UL, FALSE, &actual));
    CHECK(read_count == 0U && write_count == 0U);
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, 2, &actual));
    CHECK(read_count == 0U && write_count == 0U);
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, NULL));
    CHECK(read_count == 0U && write_count == 0U);
    CHECK(actual == 0x13579BDFUL);

    clock_host.host_div = 0U;
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(read_count == 0U && write_count == 0U);
    clock_host.host_div = 17U;
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(read_count == 0U && write_count == 0U);

    setup_host(10U, 0UL, 0UL);
    CHECK(!p4host_reset(NULL));
    CHECK(read_count == 0U && write_count == 0U);

    setup_host(10U, 0UL, 0UL);
    active_task = &task_b;
    CHECK(!p4host_reset(&clock_host));
    CHECK(read_count == 0U && write_count == 0U);
}

static void test_source_divider_bounds(void)
{
    begin_case();
    setup_host(10U, 0UL, 0UL);
    CHECK(!clock_source(&clock_host, 0U));
    CHECK(!clock_source(&clock_host, 17U));
    CHECK(read_count == 0U && write_count == 0U);
}

static void test_slot0_identification_clock(void)
{
    ULONG actual = 0UL;
    ULONG original_clkdiv;
    ULONG original_clksrc;
    ULONG original_clkctrl01;
    ULONG original_clkctrl02;
    ULONG expected_clkctrl01;
    unsigned index;
    struct p4sdmmc_clock_plan plan;
    begin_case();
    setup_host(10U, 0UL, 0UL);
    original_clkdiv = read_sd(P4SD_CLKDIV);
    original_clksrc = read_sd(P4SD_CLKSRC);
    original_clkctrl01 = read_hp(P4_HP_PERI_CLK_CTRL01);
    original_clkctrl02 = read_hp(P4_HP_PERI_CLK_CTRL02);
    read_count = 0U;

    CHECK(p4sdmmc_clock_plan(400000UL, 10U, 0, &plan));
    CHECK(plan.host_div == 10U && plan.card_div == 20U);
    CHECK(p4host_set_clock(&clock_host, 0U, 400000UL, FALSE, &actual));
    CHECK(actual == plan.actual_hz && actual == 400000UL);
    CHECK(read_sd(P4SD_CLKDIV) ==
          ((original_clkdiv & ~0xFFUL) | (ULONG)plan.card_div));
    CHECK(read_sd(P4SD_CLKSRC) == (original_clksrc & ~3UL));
    CHECK(read_sd(P4SD_CLKENA) == 0x00020003UL);
    CHECK(clock_host.host_div == 10U);
    CHECK(clock_host.slot_clock[0] == 400000UL &&
          clock_host.slot_clock[1] == 0UL);
    CHECK(invalidate_count == 0U);
    CHECK(read_hp(P4_HP_SOC_CLK_CTRL1) ==
          (0x80001234UL | P4_HP_SDMMC_CLK_EN));
    CHECK(read_hp(P4_HP_REF_CLK_CTRL2) ==
          (0x00000123UL | P4_HP_REF_160M_CLK_EN));
    expected_clkctrl01 = (original_clkctrl01 &
                          ~(P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC)) |
                         P4_HP_SDIO_CLK_EN;
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL01) == expected_clkctrl01);
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL02) ==
          expected_source_fields(10U, original_clkctrl02));
    CHECK(writes_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02) == 2U);
    CHECK((nth_write_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, 1U) &
           P4_HP_SDIO_UPDATE) != 0UL);
    CHECK((nth_write_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, 2U) &
           P4_HP_SDIO_UPDATE) == 0UL);
    CHECK(cmd_write_count == 3U);
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == P4_SDMMC_BASE + P4SD_CMD)
            CHECK(write_log[index].value ==
                  (P4SD_CMD_UPDATE_CLK | P4SD_CMD_WAIT_DATA |
                   P4SD_CMD_USE_HOLD | P4SD_CMD_START));
    CHECK((read_sd(P4SD_RINTSTS) & RADIO_IRQ_BITS) == RADIO_IRQ_BITS);
    CHECK((read_sd(P4SD_RINTSTS) & P4SD_INT_CMD_DATA_MASK) == 0UL);
    CHECK(writes_to(P4_SDMMC_BASE + P4SD_RINTSTS) == 6U);
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == P4_SDMMC_BASE + P4SD_RINTSTS)
            CHECK(write_log[index].value == P4SD_INT_CMD_DATA_MASK);
}

static void test_slot1_peer_preserves_shared_source(void)
{
    ULONG actual = 0UL;
    ULONG original_clkdiv;
    ULONG original_clksrc;
    ULONG original_clkctrl01;
    ULONG original_clkctrl02;
    ULONG original_soc;
    ULONG original_ref;
    ULONG expected_clkdiv;
    ULONG expected_clksrc;
    ULONG expected_clkena;
    unsigned index;
    struct p4sdmmc_clock_plan plan;
    begin_case();
    setup_host(10U, 12000000UL, 0UL);
    original_clkdiv = read_sd(P4SD_CLKDIV);
    original_clksrc = read_sd(P4SD_CLKSRC);
    original_clkctrl01 = read_hp(P4_HP_PERI_CLK_CTRL01);
    original_clkctrl02 = read_hp(P4_HP_PERI_CLK_CTRL02);
    original_soc = read_hp(P4_HP_SOC_CLK_CTRL1);
    original_ref = read_hp(P4_HP_REF_CLK_CTRL2);
    read_count = 0U;

    CHECK(p4sdmmc_clock_plan(4000000UL, 10U, 1, &plan));
    CHECK(plan.host_div == 10U && plan.card_div == 2U);
    CHECK(p4host_set_clock(&clock_host, 1U, 4000000UL, TRUE, &actual));
    CHECK(actual == plan.actual_hz && actual == 4000000UL);
    expected_clkdiv = (original_clkdiv & ~(0xFFUL << 8)) |
                      ((ULONG)plan.card_div << 8);
    expected_clksrc = (original_clksrc & ~(3UL << 2)) | (1UL << 2);
    expected_clkena = 0x00030003UL;
    CHECK(read_sd(P4SD_CLKDIV) == expected_clkdiv);
    CHECK(read_sd(P4SD_CLKSRC) == expected_clksrc);
    CHECK(read_sd(P4SD_CLKENA) == expected_clkena);
    CHECK((expected_clkena & 3UL) == 3UL);
    CHECK((read_sd(P4SD_CLKENA) & (3UL << 16)) == (3UL << 16));
    CHECK((expected_clkena & (3UL << 16)) == (3UL << 16));
    CHECK(clock_host.host_div == 10U);
    CHECK(clock_host.slot_clock[0] == 12000000UL &&
          clock_host.slot_clock[1] == 4000000UL);
    CHECK(invalidate_count == 0U);
    CHECK(read_hp(P4_HP_SOC_CLK_CTRL1) == original_soc);
    CHECK(read_hp(P4_HP_REF_CLK_CTRL2) == original_ref);
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL01) == original_clkctrl01);
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL02) == original_clkctrl02);
    CHECK(writes_in_range(P4_HP_SYS_CLKRST_BASE, sizeof(hp_registers)) == 0U);
    CHECK(cmd_write_count == 3U);
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == P4_SDMMC_BASE + P4SD_CMD)
            CHECK(write_log[index].value ==
                  (P4SD_CMD_UPDATE_CLK | P4SD_CMD_WAIT_DATA |
                   P4SD_CMD_USE_HOLD | P4SD_CMD_START |
                   (1UL << P4SD_CMD_SLOT_S)));
    CHECK((read_sd(P4SD_RINTSTS) & RADIO_IRQ_BITS) == RADIO_IRQ_BITS);
    CHECK(writes_to(P4_SDMMC_BASE + P4SD_RINTSTS) == 6U);
}

static void test_data_busy_fails_closed(void)
{
    ULONG actual = 0x13579BDFUL;
    begin_case();
    setup_host(10U, 12000000UL, 7000000UL);
    timer_step = P4HOST_CLOCK_TIMEOUT_US;
    sd_registers[P4SD_STATUS / sizeof(ULONG)] = P4SD_STATUS_DATA_BUSY;
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(cmd_write_count == 0U);
    check_failed_transition(actual);
}

static void test_slot0_peer_preserves_slot1_source_and_low_power(void)
{
    ULONG actual = 0UL;
    ULONG original_clkdiv;
    ULONG original_clksrc;
    ULONG original_clkctrl01;
    ULONG original_clkctrl02;
    ULONG original_soc;
    ULONG original_ref;
    unsigned index;
    struct p4sdmmc_clock_plan plan;
    begin_case();
    setup_host(10U, 0UL, 7000000UL);
    sd_registers[P4SD_CLKENA / sizeof(ULONG)] = 0x00020002UL;
    original_clkdiv = read_sd(P4SD_CLKDIV);
    original_clksrc = read_sd(P4SD_CLKSRC);
    original_clkctrl01 = read_hp(P4_HP_PERI_CLK_CTRL01);
    original_clkctrl02 = read_hp(P4_HP_PERI_CLK_CTRL02);
    original_soc = read_hp(P4_HP_SOC_CLK_CTRL1);
    original_ref = read_hp(P4_HP_REF_CLK_CTRL2);

    CHECK(p4sdmmc_clock_plan(25000000UL, 10U, 1, &plan));
    CHECK(plan.host_div == 10U && plan.card_div == 0U);
    CHECK(p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(actual == plan.actual_hz && actual == 16000000UL);
    CHECK(read_sd(P4SD_CLKDIV) == (original_clkdiv & ~0xFFUL));
    CHECK(read_sd(P4SD_CLKSRC) == (original_clksrc & ~3UL));
    CHECK(read_sd(P4SD_CLKENA) == 0x00020003UL);
    CHECK(clock_host.host_div == 10U);
    CHECK(clock_host.slot_clock[0] == 16000000UL &&
          clock_host.slot_clock[1] == 7000000UL);
    CHECK(invalidate_count == 0U);
    CHECK(read_hp(P4_HP_SOC_CLK_CTRL1) == original_soc);
    CHECK(read_hp(P4_HP_REF_CLK_CTRL2) == original_ref);
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL01) == original_clkctrl01);
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL02) == original_clkctrl02);
    CHECK(writes_in_range(P4_HP_SYS_CLKRST_BASE, sizeof(hp_registers)) == 0U);
    CHECK(cmd_write_count == 3U);
    for (index = 0U; index < write_count; ++index)
        if (write_log[index].address == P4_SDMMC_BASE + P4SD_CMD)
            CHECK(write_log[index].value ==
                  (P4SD_CMD_UPDATE_CLK | P4SD_CMD_WAIT_DATA |
                   P4SD_CMD_USE_HOLD | P4SD_CMD_START));
    CHECK((read_sd(P4SD_RINTSTS) & RADIO_IRQ_BITS) == RADIO_IRQ_BITS);
}

static void test_update_failures(void)
{
    ULONG actual;
    ULONG fault_index;
    for (fault_index = 1U; fault_index <= 3U; ++fault_index)
    {
        begin_case();
        setup_host(10U, 12000000UL, 7000000UL);
        timer_step = P4HOST_CLOCK_TIMEOUT_US;
        fault_cmd_timeout_index = fault_index;
        actual = 0x13579BDFUL;
        CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
        CHECK(cmd_write_count == fault_index);
        check_failed_transition(actual);
    }
    for (fault_index = 1U; fault_index <= 3U; ++fault_index)
    {
        begin_case();
        setup_host(10U, 12000000UL, 7000000UL);
        fault_hle_index = fault_index;
        actual = 0x13579BDFUL;
        CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
        CHECK(cmd_write_count == fault_index);
        CHECK((read_sd(P4SD_RINTSTS) & P4SD_INT_HLE) != 0UL);
        check_failed_transition(actual);
    }
}

static void test_reset_invalidates_and_pulses_source(void)
{
    ULONG original_clkctrl02;
    ULONG original_lp_reset;
    ULONG original_clkena;
    ULONG expected_lp_reset;
    unsigned gate_enable_access;
    unsigned first_sd_access;
    begin_case();
    setup_host(7U, 25000000UL, 18000000UL);
    original_clkctrl02 = read_hp(P4_HP_PERI_CLK_CTRL02);
    original_lp_reset = read_lp(P4_LP_SDMMC_RST_CTRL);
    original_clkena = read_sd(P4SD_CLKENA);
    CHECK(p4host_reset(&clock_host));
    gate_enable_access = first_write_setting(
        P4_HP_SYS_CLKRST_BASE + P4_HP_SOC_CLK_CTRL1,
        P4_HP_SDMMC_CLK_EN);
    first_sd_access = first_sdmmc_access();
    CHECK(gate_enable_access < first_sd_access);
    CHECK(invalidate_count == 1U);
    CHECK(clock_host.slot_clock[0] == 0UL &&
          clock_host.slot_clock[1] == 0UL);
    CHECK(clock_host.host_div == 10U);
    CHECK(read_sd(P4SD_CLKENA) == (original_clkena & ~3UL));
    CHECK(read_hp(P4_HP_SOC_CLK_CTRL1) ==
          (0x80001234UL | P4_HP_SDMMC_CLK_EN));
    CHECK(read_hp(P4_HP_REF_CLK_CTRL2) ==
          (0x00000123UL | P4_HP_REF_160M_CLK_EN));
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL01) ==
          ((0xA5000000UL & ~(P4_HP_SDIO_HS_MODE | P4_HP_SDIO_CLK_SRC)) |
           P4_HP_SDIO_CLK_EN));
    CHECK(read_hp(P4_HP_PERI_CLK_CTRL02) ==
          expected_source_fields(10U, original_clkctrl02));
    CHECK(writes_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02) == 2U);
    CHECK((nth_write_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, 1U) &
           P4_HP_SDIO_UPDATE) != 0UL);
    CHECK((nth_write_to(P4_HP_SYS_CLKRST_BASE + P4_HP_PERI_CLK_CTRL02, 2U) &
           P4_HP_SDIO_UPDATE) == 0UL);
    CHECK(writes_to(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL) == 2U);
    expected_lp_reset = original_lp_reset | P4_LP_SDMMC_RST_EN;
    CHECK(nth_write_to(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL, 1U) ==
          expected_lp_reset);
    CHECK(nth_write_to(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL, 2U) ==
          (original_lp_reset & ~P4_LP_SDMMC_RST_EN));
    CHECK(read_lp(P4_LP_SDMMC_RST_CTRL) ==
          (original_lp_reset & ~P4_LP_SDMMC_RST_EN));
}

static void test_stalled_timer_is_bounded_and_fails_closed(void)
{
    ULONG actual = 0x13579BDFUL;
    begin_case();
    setup_host(10U, 0UL, 0UL);
    timer_step = 0U;
    CHECK(!p4host_set_clock(&clock_host, 0U, 25000000UL, FALSE, &actual));
    CHECK(timer_value == 0U);
    CHECK(now_call_count >= P4HOST_POLL_BOUND + 1UL);
    CHECK(now_call_count <= P4HOST_POLL_BOUND + 10UL);
    CHECK(cmd_write_count == 1U);
    CHECK(timer_value == 0U);
    check_failed_transition(actual);

    begin_case();
    setup_host(7U, 25000000UL, 18000000UL);
    timer_step = 0U;
    CHECK(!p4host_reset(&clock_host));
    CHECK(timer_value == 0U);
    CHECK(now_call_count == P4HOST_POLL_BOUND + 1UL);
    CHECK(invalidate_count == 2U);
    CHECK(clock_host.slot_clock[0] == 0UL &&
          clock_host.slot_clock[1] == 0UL);
    CHECK(clock_host.host_div == 0U);
    CHECK((read_sd(P4SD_CLKENA) & 3UL) == 0UL);
    CHECK(writes_to(P4_LP_CLKRST_BASE + P4_LP_SDMMC_RST_CTRL) == 0U);
}

int main(void)
{
    test_invalid_calls_have_no_mmio();
    test_source_divider_bounds();
    test_slot0_identification_clock();
    test_slot1_peer_preserves_shared_source();
    test_slot0_peer_preserves_slot1_source_and_low_power();
    test_data_busy_fails_closed();
    test_update_failures();
    test_reset_invalidates_and_pulses_source();
    test_stalled_timer_is_bounded_and_fails_closed();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
