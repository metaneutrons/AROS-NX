/* Template for test-wifi-transfer.py's extracted production-source model. */
#include <hardware/mmc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_fifo.h"
#include "hosted_tx.h"
#include "sdio_commands.h"

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef int BOOL;
static int owner_task;
static void *FindTask(void *unused)
{
    (void)unused;
    return &owner_task;
}

#define TRUE 1
#define FALSE 0

/* INSERT_ACTUAL_CONSTANTS */
/* INSERT_ACTUAL_COMMAND_TYPES */

struct P4SDMMCHost;
typedef BOOL (*host_command_fn)(struct P4SDMMCHost *,
    const struct P4HostCommand *, struct P4HostCommandResult *);

/* Only fields read by the extracted production budget/transfer adapters. */
struct P4SDMMCHost
{
    ULONG generation;
    BOOL dma_faulted;
    struct { void *ss_Owner; } lock;
    ULONG slot_clock[2];
    ULONG (*now_us)(void);
    ULONG budget_start;
    ULONG budget_us;
    host_command_fn command;
};

/* INSERT_ACTUAL_BUDGET_HELPERS */

struct wifi_probe_context
{
    struct P4SDMMCHost *host;
    ULONG generation;
    ULONG peer_clock;
    struct P4HostCommandResult last;
};

/* INSERT_ACTUAL_TRANSFER_FUNCTIONS */

static unsigned checks;
static unsigned cases;
static unsigned command_calls;
static ULONG clock_us;
static BOOL command_completed;
static BOOL expire_budget_on_command;
static BOOL change_generation_on_command;
static BOOL fault_dma_on_command;
static UBYTE response_bytes[4];
static UBYTE captured_data[4];
static struct P4HostCommand captured_request;
static struct P4HostCommandResult next_result;
static struct P4SDMMCHost host;
static struct wifi_probe_context test_context;

#define WIFI_TEST_ADDRESS UINT32_C(0x1234)
#define WIFI_TEST_READ_WORD UINT32_C(0x89abcdef)
#define WIFI_TEST_WRITE_WORD UINT32_C(0x12345678)
#define WIFI_TEST_SENTINEL UINT32_C(0xa55aa55a)

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static ULONG mock_now_us(void)
{
    return clock_us;
}

static BOOL mock_host_command(struct P4SDMMCHost *target,
    const struct P4HostCommand *request,
    struct P4HostCommandResult *result)
{
    unsigned index;

    ++command_calls;
    memcpy(&captured_request, request, sizeof(captured_request));
    if (request->data && request->length == 4U)
    {
        if (request->writing)
        {
            for (index = 0U; index < 4U; ++index)
                captured_data[index] = request->data[index];
        }
        else if (command_completed)
        {
            for (index = 0U; index < 4U; ++index)
                request->data[index] = response_bytes[index];
            for (index = 0U; index < 4U; ++index)
                captured_data[index] = request->data[index];
        }
    }

    *result = next_result;
    if (expire_budget_on_command)
        clock_us = target->budget_start + target->budget_us;
    if (change_generation_on_command)
        ++target->generation;
    if (fault_dma_on_command)
        target->dma_faulted = TRUE;
    return command_completed;
}

static uint32_t expected_cmd53_argument(BOOL writing)
{
    uint32_t argument = (UINT32_C(1) << 28) |
                        (UINT32_C(1) << 26) |
                        (WIFI_TEST_ADDRESS << 9) | UINT32_C(4);
    if (writing)
        argument |= UINT32_C(1) << 31;
    return argument;
}

static uint32_t expected_fifo_argument(
    const struct p4_hosted_fifo_plan *plan)
{
    return (UINT32_C(1) << 28) | (UINT32_C(1) << 27) |
           (UINT32_C(1) << 26) |
           ((P4_HOSTED_FIFO_END - plan->logical_bytes) << 9) |
           (plan->bus_bytes / 512U);
}

static void begin_case(void)
{
    ++cases;
    memset(&host, 0, sizeof(host));
    memset(&test_context, 0, sizeof(test_context));
    memset(&captured_request, 0xa5, sizeof(captured_request));
    memset(captured_data, 0xa5, sizeof(captured_data));
    memset(&next_result, 0, sizeof(next_result));
    host.generation = UINT32_C(0x5a17c0de);
    host.now_us = mock_now_us;
    host.budget_start = 100U;
    host.budget_us = 100U;
    host.command = mock_host_command;
    test_context.host = &host;
    test_context.generation = host.generation;
    test_context.peer_clock = 25000000U;
    host.slot_clock[0] = test_context.peer_clock;
    host.lock.ss_Owner = FindTask(NULL);
    clock_us = host.budget_start;
    command_calls = 0U;
    command_completed = TRUE;
    expire_budget_on_command = FALSE;
    change_generation_on_command = FALSE;
    fault_dma_on_command = FALSE;
    response_bytes[0] = 0xefU;
    response_bytes[1] = 0xcdU;
    response_bytes[2] = 0xabU;
    response_bytes[3] = 0x89U;
    next_result.status = P4HOST_COMMAND_OK;
    next_result.started = TRUE;
}

static void check_exact_request(BOOL writing)
{
    struct P4HostCommand expected;

    p4host_zero(&expected, sizeof(expected));
    expected.slot = P4_BOARD_WIFI_SLOT;
    expected.generation = test_context.generation;
    expected.index = 53U;
    expected.argument = expected_cmd53_argument(writing);
    expected.response_flags = MMC_RSP_R5;
    expected.response_error_mask = P4_SDIO_PROBE_R5_ERROR_MASK;
    expected.data = captured_request.data;
    expected.length = 4U;
    expected.block_size = 4U;
    expected.writing = writing;

    CHECK(command_calls == 1U);
    CHECK(captured_request.slot == P4_BOARD_WIFI_SLOT);
    CHECK(captured_request.generation == test_context.generation);
    CHECK(captured_request.index == 53U);
    CHECK(captured_request.argument == expected_cmd53_argument(writing));
    CHECK(captured_request.response_flags == MMC_RSP_R5);
    CHECK(captured_request.response_error_mask ==
          P4_SDIO_PROBE_R5_ERROR_MASK);
    CHECK(captured_request.data != NULL);
    CHECK(captured_request.length == 4U);
    CHECK(captured_request.block_size == 4U);
    CHECK(captured_request.writing == writing);
    CHECK(memcmp(&expected, &captured_request, sizeof(expected)) == 0);
}

static void check_exact_fifo_request(
    const struct p4_hosted_fifo_plan *plan)
{
    struct P4HostCommand expected;

    p4host_zero(&expected, sizeof(expected));
    expected.slot = P4_BOARD_WIFI_SLOT;
    expected.generation = test_context.generation;
    expected.index = 53U;
    expected.argument = expected_fifo_argument(plan);
    expected.response_flags = MMC_RSP_R5;
    expected.response_error_mask = P4_SDIO_PROBE_R5_ERROR_MASK;
    expected.data = captured_request.data;
    expected.length = plan->bus_bytes;
    expected.block_size = 512U;
    expected.writing = FALSE;

    CHECK(command_calls == 1U);
    CHECK(captured_request.slot == P4_BOARD_WIFI_SLOT);
    CHECK(captured_request.generation == test_context.generation);
    CHECK(captured_request.index == 53U);
    CHECK(captured_request.argument == expected_fifo_argument(plan));
    CHECK(captured_request.argument == plan->argument);
    CHECK(captured_request.response_flags == MMC_RSP_R5);
    CHECK(captured_request.response_error_mask ==
          P4_SDIO_PROBE_R5_ERROR_MASK);
    CHECK(captured_request.data != NULL);
    CHECK(captured_request.length == plan->bus_bytes);
    CHECK(captured_request.block_size == 512U);
    CHECK(captured_request.writing == FALSE);
    CHECK(memcmp(&expected, &captured_request, sizeof(expected)) == 0);
}

static void test_fn1_byte_mode_word_read_is_little_endian(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 1);
    CHECK(word == WIFI_TEST_READ_WORD);
    check_exact_request(FALSE);
    CHECK(captured_data[0] == 0xefU);
    CHECK(captured_data[1] == 0xcdU);
    CHECK(captured_data[2] == 0xabU);
    CHECK(captured_data[3] == 0x89U);
}

static void test_fn1_byte_mode_word_write_is_little_endian(void)
{
    begin_case();

    CHECK(wifi_word_write(&test_context, WIFI_TEST_ADDRESS,
                          WIFI_TEST_WRITE_WORD) == 1);
    check_exact_request(TRUE);
    CHECK(captured_data[0] == 0x78U);
    CHECK(captured_data[1] == 0x56U);
    CHECK(captured_data[2] == 0x34U);
    CHECK(captured_data[3] == 0x12U);
}

static void test_expired_budget_refuses_before_command(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    clock_us = host.budget_start + host.budget_us;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 0U);
}

static void test_post_command_budget_expiry_preserves_read_output(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    expire_budget_on_command = TRUE;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 1U);
    check_exact_request(FALSE);
}

static void test_generation_change_preserves_read_output(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    change_generation_on_command = TRUE;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 1U);
    CHECK(host.generation != test_context.generation);
    check_exact_request(FALSE);
}

static void test_motor_refusal_preserves_read_output(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    command_completed = FALSE;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 1U);
    check_exact_request(FALSE);
}

static void test_dma_fault_after_motor_success_preserves_read_output(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    fault_dma_on_command = TRUE;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 1U);
    CHECK(host.dma_faulted == TRUE);
    check_exact_request(FALSE);
}

static void test_existing_dma_fault_rejects_read(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    host.dma_faulted = TRUE;

    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(command_calls == 1U);
    check_exact_request(FALSE);
}

static void test_motor_refusal_rejects_write(void)
{
    begin_case();
    command_completed = FALSE;

    CHECK(wifi_word_write(&test_context, WIFI_TEST_ADDRESS,
                          WIFI_TEST_WRITE_WORD) == 0);
    CHECK(command_calls == 1U);
    check_exact_request(TRUE);
    CHECK(captured_data[0] == 0x78U);
    CHECK(captured_data[1] == 0x56U);
    CHECK(captured_data[2] == 0x34U);
    CHECK(captured_data[3] == 0x12U);
}

static void test_fifo_read_uses_fn1_incrementing_block_cmd53(void)
{
    struct p4_hosted_fifo_plan plan;
    UBYTE buffer[1024];
    begin_case();
    plan.logical_bytes = 513U;
    plan.bus_bytes = 1024U;
    plan.argument = expected_fifo_argument(&plan);
    plan.credits = 7U;

    CHECK(wifi_fifo_read(&test_context, &plan, buffer) == 1);
    check_exact_fifo_request(&plan);
}

static void expect_invalid_fifo_plan(struct p4_hosted_fifo_plan plan)
{
    UBYTE buffer[1024];
    begin_case();
    CHECK(wifi_fifo_read(&test_context, &plan, buffer) == 0);
    CHECK(command_calls == 0U);
}

static void test_fifo_read_rejects_forged_command_argument(void)
{
    struct p4_hosted_fifo_plan plan;
    plan.logical_bytes = 513U;
    plan.bus_bytes = 1024U;
    plan.argument = expected_fifo_argument(&plan) ^ (UINT32_C(1) << 28);
    plan.credits = 1U;
    expect_invalid_fifo_plan(plan);
}

static void test_fifo_read_rejects_zero_logical_length(void)
{
    struct p4_hosted_fifo_plan plan;
    plan.logical_bytes = 0U;
    plan.bus_bytes = 0U;
    plan.argument = 0U;
    plan.credits = 1U;
    expect_invalid_fifo_plan(plan);
}

static void test_fifo_read_rejects_length_over_bus_limit(void)
{
    struct p4_hosted_fifo_plan plan;
    plan.logical_bytes = P4_HOSTED_FIFO_BUS_MAX + 1U;
    plan.bus_bytes = P4_HOSTED_FIFO_BUS_MAX + 512U;
    plan.argument = 0U;
    plan.credits = 1U;
    expect_invalid_fifo_plan(plan);
}

static void test_fifo_read_rejects_padding_mismatch(void)
{
    struct p4_hosted_fifo_plan plan;
    plan.logical_bytes = 513U;
    plan.bus_bytes = 512U;
    plan.argument = expected_fifo_argument(&plan);
    plan.credits = 1U;
    expect_invalid_fifo_plan(plan);
}

static void test_invalid_inputs_do_not_issue_commands(void)
{
    uint32_t word = WIFI_TEST_SENTINEL;
    begin_case();
    CHECK(wifi_word_read(&test_context, WIFI_TEST_ADDRESS, NULL) == 0);
    CHECK(wifi_word_read(&test_context, UINT32_C(0x20000), &word) == 0);
    CHECK(word == WIFI_TEST_SENTINEL);
    CHECK(wifi_word_write(&test_context, UINT32_C(0x20000),
                          WIFI_TEST_WRITE_WORD) == 0);
    CHECK(command_calls == 0U);
}

static void test_fifo_write_motor_facts(void)
{
    struct p4_hosted_fifo_plan plan = {0, 29, 512, 1};
    struct p4_hosted_tx_completion completion;
    UBYTE buffer[512] = {0};
    unsigned variation;
    plan.argument = expected_fifo_argument(&plan) | (UINT32_C(1) << 31);
    begin_case();
    CHECK(wifi_fifo_write(&test_context, &plan, buffer, &completion) == 1);
    CHECK(command_calls == 1);
    CHECK(captured_request.index == 53);
    CHECK(captured_request.slot == 1);
    CHECK(captured_request.argument == plan.argument);
    CHECK(captured_request.length == 512);
    CHECK(captured_request.block_size == 512);
    CHECK(captured_request.writing == TRUE);
    CHECK(captured_request.data == buffer);
    CHECK(completion.started == 1 && completion.success == 1);
    CHECK(completion.completed_bytes == 512);
    for (variation = 0; variation < 8; ++variation)
    {
        begin_case();
        if (variation == 0) clock_us += host.budget_us;
        if (variation == 1) host.lock.ss_Owner = NULL;
        if (variation == 2) ++host.generation;
        if (variation == 3) ++host.slot_clock[0];
        if (variation == 4) host.dma_faulted = TRUE;
        if (variation == 5) expire_budget_on_command = TRUE;
        if (variation == 6) change_generation_on_command = TRUE;
        if (variation == 7) fault_dma_on_command = TRUE;
        /* Deliberately stale successful previous request is not reused. */
        test_context.last.started = TRUE;
        test_context.last.status = P4HOST_COMMAND_OK;
        CHECK(wifi_fifo_write(&test_context, &plan, buffer, &completion) == 1);
        CHECK(command_calls == (variation < 5 ? 0U : 1U));
        CHECK(completion.started == (variation < 5 ? 0U : 1U));
        CHECK(completion.success == 0 && completion.completed_bytes == 0);
    }
    for (variation = 0; variation < 5; ++variation)
    {
        begin_case();
        command_completed = FALSE;
        next_result.status = P4HOST_COMMAND_ERROR;
        if (variation == 0) next_result.started = FALSE;
        if (variation == 2) next_result.started = 2;
        if (variation == 3) command_completed = 2;
        if (variation == 4) command_completed = TRUE;
        CHECK(wifi_fifo_write(&test_context, &plan, buffer, &completion) ==
              (variation < 2 ? 1 : 0));
        CHECK(command_calls == 1);
        if (variation < 2)
        {
            CHECK(completion.started == variation);
            CHECK(completion.success == 0 && completion.completed_bytes == 0);
        }
    }
    for (variation = 0; variation < 6; ++variation)
    {
        struct p4_hosted_fifo_plan invalid = plan;
        begin_case();
        if (variation == 0) invalid.logical_bytes = 0;
        if (variation == 1) invalid.logical_bytes = 1537;
        if (variation == 2) invalid.bus_bytes = 1024;
        if (variation == 3) invalid.credits = 2;
        if (variation == 4) invalid.argument &= ~(UINT32_C(1) << 31);
        if (variation == 5) invalid.argument ^= (UINT32_C(1) << 28);
        CHECK(wifi_fifo_write(&test_context, &invalid, buffer, &completion) == 0);
        CHECK(command_calls == 0);
    }
    begin_case();
    CHECK(wifi_fifo_write(&test_context, NULL, buffer, &completion) == 0);
    CHECK(wifi_fifo_write(&test_context, &plan, NULL, &completion) == 0);
    CHECK(wifi_fifo_write(&test_context, &plan, buffer, NULL) == 0);
    CHECK(command_calls == 0);
}

int main(void)
{
    test_fn1_byte_mode_word_read_is_little_endian();
    test_fn1_byte_mode_word_write_is_little_endian();
    test_expired_budget_refuses_before_command();
    test_post_command_budget_expiry_preserves_read_output();
    test_generation_change_preserves_read_output();
    test_motor_refusal_preserves_read_output();
    test_dma_fault_after_motor_success_preserves_read_output();
    test_existing_dma_fault_rejects_read();
    test_motor_refusal_rejects_write();
    test_fifo_read_uses_fn1_incrementing_block_cmd53();
    test_fifo_read_rejects_forged_command_argument();
    test_fifo_read_rejects_zero_logical_length();
    test_fifo_read_rejects_length_over_bus_limit();
    test_fifo_read_rejects_padding_mismatch();
    test_invalid_inputs_do_not_issue_commands();
    test_fifo_write_motor_facts();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
