/* Template for test-wifi-reset-status.py's extracted production-source model. */
#include <hardware/mmc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0
#define P4_WIFI_SDIO_RESET_QUERY 0
#define P4_WIFI_SDIO_SETUP 1

/* INSERT_ACTUAL_CONSTANTS */
/* INSERT_ACTUAL_COMMAND_TYPES */

struct P4SDMMCHost;
typedef BOOL (*host_command_fn)(struct P4SDMMCHost *,
    const struct P4HostCommand *, struct P4HostCommandResult *);

/* Only fields read by the extracted runtime callbacks are modeled here. */
struct P4SDMMCHost
{
    ULONG generation;
    BOOL dma_faulted;
    ULONG (*now_us)(void);
    ULONG budget_start;
    ULONG budget_us;
    host_command_fn command;
};

/* INSERT_ACTUAL_BUDGET_HELPER */
/* INSERT_ACTUAL_CMD52 */

struct wifi_probe_context
{
    struct P4SDMMCHost *host;
    ULONG generation;
    struct P4HostCommandResult last;
};

static unsigned checks;
static unsigned cases;
static unsigned command_calls;
static unsigned zero_calls;
static ULONG clock_us;
static BOOL command_completed;
static BOOL increment_generation_on_command;
static BOOL expire_budget_on_command;
static BOOL fault_dma_on_command;
static struct P4HostCommand captured_request;
static struct P4HostCommandResult next_result;
static struct P4SDMMCHost host;
static struct wifi_probe_context test_context;

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

static void p4host_zero(void *destination, size_t length)
{
    unsigned char *bytes = (unsigned char *)destination;
    size_t index;
    ++zero_calls;
    for (index = 0U; index < length; ++index)
        bytes[index] = 0U;
}

static BOOL mock_host_command(struct P4SDMMCHost *target,
    const struct P4HostCommand *request,
    struct P4HostCommandResult *result)
{
    ++command_calls;
    captured_request = *request;
    *result = next_result;
    if (increment_generation_on_command)
        ++target->generation;
    if (expire_budget_on_command)
        clock_us = target->budget_start + target->budget_us;
    if (fault_dma_on_command)
        target->dma_faulted = TRUE;
    return command_completed;
}

/* INSERT_ACTUAL_WIFI_BUDGET */
/* INSERT_ACTUAL_WIFI_COMMAND_FAILURE */
/* INSERT_ACTUAL_WIFI_COMMAND */

static int register_command_return;
static uint32_t register_command_response;
static unsigned register_command_calls;
static unsigned register_command_index;
static uint32_t register_command_argument;
static enum p4_sdio_response_type register_command_response_type;
static uint32_t register_command_error_mask;

static int wifi_command_for_register_test(void *opaque, unsigned index,
    uint32_t argument, enum p4_sdio_response_type response,
    uint32_t error_mask, uint32_t *word)
{
    (void)opaque;
    ++register_command_calls;
    register_command_index = index;
    register_command_argument = argument;
    register_command_response_type = response;
    register_command_error_mask = error_mask;
    *word = register_command_response;
    return register_command_return;
}

#define wifi_command wifi_command_for_register_test
/* INSERT_ACTUAL_WIFI_REGISTER_ADAPTERS */
#undef wifi_command

static void begin_case(void)
{
    ++cases;
    memset(&host, 0, sizeof(host));
    memset(&test_context, 0, sizeof(test_context));
    memset(&captured_request, 0xa5, sizeof(captured_request));
    memset(&next_result, 0, sizeof(next_result));
    host.generation = UINT32_C(0x5a17c0de);
    host.dma_faulted = FALSE;
    host.now_us = mock_now_us;
    host.budget_start = 100U;
    host.budget_us = 100U;
    host.command = mock_host_command;
    test_context.host = &host;
    test_context.generation = host.generation;
    clock_us = host.budget_start;
    command_calls = 0U;
    zero_calls = 0U;
    command_completed = FALSE;
    increment_generation_on_command = FALSE;
    expire_budget_on_command = FALSE;
    fault_dma_on_command = FALSE;
    next_result.status = P4HOST_COMMAND_ERROR;
    next_result.started = TRUE;
    next_result.raw = P4SD_INT_CMD_DONE | P4SD_INT_RTO;
    next_result.response[0] = UINT32_C(0xdeadc0de);
    next_result.response[1] = UINT32_C(0xdead0001);
    next_result.response[2] = UINT32_C(0xdead0002);
    next_result.response[3] = UINT32_C(0xdead0003);
}

static uint32_t expected_response_flags(enum p4_sdio_response_type response)
{
    static const ULONG flags[] = {
        MMC_RSP_NONE, MMC_RSP_R4, MMC_RSP_R6, MMC_RSP_R1, MMC_RSP_R5
    };
    return flags[response];
}

static void check_captured_request(unsigned index, uint32_t argument,
    enum p4_sdio_response_type response, uint32_t error_mask)
{
    CHECK(captured_request.slot == P4_BOARD_WIFI_SLOT);
    CHECK(captured_request.generation == test_context.generation);
    CHECK(captured_request.index == index);
    CHECK(captured_request.argument == argument);
    CHECK(captured_request.response_flags == expected_response_flags(response));
    CHECK(captured_request.response_error_mask == error_mask);
    CHECK(captured_request.data == NULL);
    CHECK(captured_request.length == 0U);
    CHECK(captured_request.block_size == 0U);
    CHECK(captured_request.writing == FALSE);
    CHECK(captured_request.early_cmd18 == FALSE);
    CHECK(captured_request.diagnostic_fault == P4HOST_FAULT_NONE);
}

static void expect_host_failure(unsigned index, uint32_t argument,
    enum p4_sdio_response_type response, uint32_t error_mask,
    enum P4HostCommandStatus status, BOOL started, ULONG raw, ULONG idstatus,
    enum P4HostCommandFault injected_fault, int expected)
{
    uint32_t word = UINT32_C(0xa55aa55a);
    int returned;
    begin_case();
    next_result.status = status;
    next_result.started = started;
    next_result.raw = raw;
    next_result.idstatus = idstatus;
    next_result.injected_fault = injected_fault;
    returned = wifi_command(&test_context, index, argument, response, error_mask,
                            &word);
    CHECK(returned == expected);
    CHECK(word == UINT32_C(0xa55aa55a));
    CHECK(command_calls == 1U);
    check_captured_request(index, argument, response, error_mask);
}

static void test_exact_reset_command_classifies_only_timeout_or_crc(void)
{
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE,
        P4_SDIO_COMMAND_RESPONSE_TIMEOUT);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RCRC, 0U, P4HOST_FAULT_NONE,
        P4_SDIO_COMMAND_RESPONSE_CRC);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_TIMEOUT, TRUE, 0U, 0U,
        P4HOST_FAULT_NONE, P4_SDIO_COMMAND_RESPONSE_TIMEOUT);
}

static void test_generic_commands_never_classify_transport_failures(void)
{
    expect_host_failure(5U, 0U, P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c09), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R1,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK ^ UINT32_C(1), P4HOST_COMMAND_ERROR,
        TRUE, P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5, 0U,
        P4HOST_COMMAND_ERROR, TRUE, P4SD_INT_CMD_DONE | P4SD_INT_RCRC, 0U,
        P4HOST_FAULT_NONE, 0);
}

static void test_combined_status_bits_and_nonresponse_failures_are_rejected(void)
{
    static const ULONG extra_bits[] = {
        P4SD_INT_RESP_ERR, P4SD_INT_HLE, P4SD_INT_RCRC,
        P4SD_INT_DATA_OVER, P4SD_INT_RXDR, P4SD_INT_DCRC, P4SD_INT_DTO,
        P4SD_INT_HTO, P4SD_INT_FRUN, P4SD_INT_SBE, P4SD_INT_EBE
    };
    static const enum P4HostCommandFault injected_faults[] = {
        P4HOST_FAULT_CMD_CRC, P4HOST_FAULT_DATA_CRC, P4HOST_FAULT_TIMEOUT
    };
    unsigned index;
    for (index = 0U; index < sizeof(extra_bits) / sizeof(extra_bits[0]);
         ++index)
    {
        expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
            P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
            P4SD_INT_CMD_DONE | P4SD_INT_RTO | extra_bits[index], 0U,
            P4HOST_FAULT_NONE, 0);
    }

    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO | P4SD_INT_DCRC | P4SD_INT_DTO |
            P4SD_INT_HTO | P4SD_INT_FRUN | P4SD_INT_SBE | P4SD_INT_EBE,
        0U,
        P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, P4SD_IDSTS_FBE,
        P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_CARD_ERROR, TRUE,
        P4SD_INT_CMD_DONE, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_DMA_ERROR, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, P4SD_IDSTS_FBE,
        P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_REFUSED, TRUE,
        P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_TIMEOUT, FALSE, 0U, 0U,
        P4HOST_FAULT_NONE, 0);
    expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
        P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_TIMEOUT, TRUE,
        P4SD_INT_RTO, 0U, P4HOST_FAULT_NONE, 0);

    for (index = 0U;
         index < sizeof(injected_faults) / sizeof(injected_faults[0]);
         ++index)
    {
        expect_host_failure(52U, UINT32_C(0x80000c08), P4_SDIO_RESP_R5,
            P4_SDIO_PROBE_R5_ERROR_MASK, P4HOST_COMMAND_ERROR, TRUE,
            P4SD_INT_CMD_DONE | P4SD_INT_RTO, 0U, injected_faults[index], 0);
    }
}

static void test_generation_and_budget_are_checked_before_and_after_host(void)
{
    uint32_t word;
    int returned;

    begin_case();
    ++host.generation;
    word = UINT32_C(0xa55aa55a);
    returned = wifi_command(&test_context, 52U, UINT32_C(0x80000c08),
        P4_SDIO_RESP_R5, P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 0);
    CHECK(command_calls == 0U);
    CHECK(word == UINT32_C(0xa55aa55a));

    begin_case();
    clock_us = host.budget_start + host.budget_us;
    word = UINT32_C(0xa55aa55a);
    returned = wifi_command(&test_context, 52U, UINT32_C(0x80000c08),
        P4_SDIO_RESP_R5, P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 0);
    CHECK(command_calls == 0U);
    CHECK(word == UINT32_C(0xa55aa55a));

    begin_case();
    increment_generation_on_command = TRUE;
    word = UINT32_C(0xa55aa55a);
    returned = wifi_command(&test_context, 52U, UINT32_C(0x80000c08),
        P4_SDIO_RESP_R5, P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 0);
    CHECK(command_calls == 1U);
    CHECK(word == UINT32_C(0xa55aa55a));

    begin_case();
    expire_budget_on_command = TRUE;
    word = UINT32_C(0xa55aa55a);
    returned = wifi_command(&test_context, 52U, UINT32_C(0x80000c08),
        P4_SDIO_RESP_R5, P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 0);
    CHECK(command_calls == 1U);
    CHECK(word == UINT32_C(0xa55aa55a));

    begin_case();
    fault_dma_on_command = TRUE;
    word = UINT32_C(0xa55aa55a);
    returned = wifi_command(&test_context, 52U, UINT32_C(0x80000c08),
        P4_SDIO_RESP_R5, P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 0);
    CHECK(command_calls == 1U);
    CHECK(word == UINT32_C(0xa55aa55a));
}

static void test_success_publishes_response_only_after_host_success(void)
{
    uint32_t word = UINT32_C(0xa55aa55a);
    int returned;
    begin_case();
    command_completed = TRUE;
    next_result.status = P4HOST_COMMAND_OK;
    next_result.raw = P4SD_INT_CMD_DONE;
    next_result.response[0] = UINT32_C(0x12345678);
    returned = wifi_command(&test_context, 5U, 0U, P4_SDIO_RESP_R4,
                            P4_SDIO_PROBE_R5_ERROR_MASK, &word);
    CHECK(returned == 1);
    CHECK(word == UINT32_C(0x12345678));
    CHECK(command_calls == 1U);
    check_captured_request(5U, 0U, P4_SDIO_RESP_R4,
                           P4_SDIO_PROBE_R5_ERROR_MASK);
}

static void test_register_adapters_reject_negative_command_outcomes(void)
{
    static const int negative_outcomes[] = {
        P4_SDIO_COMMAND_RESPONSE_TIMEOUT, P4_SDIO_COMMAND_RESPONSE_CRC
    };
    unsigned index;
    for (index = 0U;
         index < sizeof(negative_outcomes) / sizeof(negative_outcomes[0]);
         ++index)
    {
        uint8_t value = UINT8_C(0xa5);
        int returned;
        begin_case();
        register_command_calls = 0U;
        register_command_return = negative_outcomes[index];
        register_command_response = UINT32_C(0x0000005a);
        returned = wifi_register_read(&test_context, 0U, 6U, &value);
        CHECK(returned == 0);
        CHECK(value == UINT8_C(0xa5));
        CHECK(register_command_calls == 1U);
        CHECK(register_command_index == 52U);
        CHECK(register_command_argument == UINT32_C(0x00000c00));
        CHECK(register_command_response_type == P4_SDIO_RESP_R5);
        CHECK(register_command_error_mask == P4_SDIO_PROBE_R5_ERROR_MASK);

        begin_case();
        register_command_calls = 0U;
        register_command_return = negative_outcomes[index];
        register_command_response = UINT32_C(0x0000005a);
        returned = wifi_register_write(&test_context, 0U, 6U, UINT8_C(8));
        CHECK(returned == 0);
        CHECK(register_command_calls == 1U);
        CHECK(register_command_index == 52U);
        CHECK(register_command_argument == UINT32_C(0x80000c08));
        CHECK(register_command_response_type == P4_SDIO_RESP_R5);
        CHECK(register_command_error_mask == P4_SDIO_PROBE_R5_ERROR_MASK);
    }
}

static void test_register_adapters_keep_positive_response_behavior(void)
{
    uint8_t value = 0U;
    int returned;
    begin_case();
    register_command_calls = 0U;
    register_command_return = 1;
    register_command_response = UINT32_C(0x0000005a);
    returned = wifi_register_read(&test_context, 0U, 6U, &value);
    CHECK(returned == 1);
    CHECK(value == UINT8_C(0x5a));
    CHECK(register_command_calls == 1U);

    begin_case();
    register_command_calls = 0U;
    register_command_return = 1;
    register_command_response = UINT32_C(0x00000008);
    returned = wifi_register_write(&test_context, 0U, 6U, UINT8_C(8));
    CHECK(returned == 1);
    CHECK(register_command_calls == 1U);
    CHECK(register_command_argument == UINT32_C(0x80000c08));
}

int main(void)
{
    CHECK(P4_BOARD_WIFI_SLOT == 1U);
    CHECK(P4_SDIO_PROBE_R5_ERROR_MASK == UINT32_C(0x0000cb00));
    CHECK(P4_SDIO_COMMAND_RESPONSE_TIMEOUT == -1);
    CHECK(P4_SDIO_COMMAND_RESPONSE_CRC == -2);
    test_exact_reset_command_classifies_only_timeout_or_crc();
    test_generic_commands_never_classify_transport_failures();
    test_combined_status_bits_and_nonresponse_failures_are_rejected();
    test_generation_and_budget_are_checked_before_and_after_host();
    test_success_publishes_response_only_after_host_success();
    test_register_adapters_reject_negative_command_outcomes();
    test_register_adapters_keep_positive_response_behavior();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
