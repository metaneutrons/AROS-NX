/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t UBYTE;
typedef uint32_t ULONG;
typedef uintptr_t IPTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0

/* INSERT_ACTUAL_SD_CONSTANTS */

#define MMC_RSP_PRESENT           (1UL << 0)
#define MMC_RSP_136               (1UL << 1)
#define MMC_RSP_CRC               (1UL << 2)
#define MMC_RSP_BUSY              (1UL << 3)
#define MMC_RSP_OPCODE            (1UL << 4)
#define MMC_RSP_NONE              0UL
#define MMC_RSP_R1                (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_R1b               (MMC_RSP_R1 | MMC_RSP_BUSY)
#define MMC_RSP_R2                (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_136)

#define SDCARD_TAG_RSP 4UL

/* INSERT_ACTUAL_COMMAND_TYPES */

struct TagItem
{
    ULONG ti_Tag;
    IPTR ti_Data;
};

struct P4SDMMCHost
{
    BOOL (*command)(struct P4SDMMCHost *, const struct P4HostCommand *,
                    struct P4HostCommandResult *);
    BOOL dma_faulted;
    ULONG generation;
};

struct p4sd_private
{
    BOOL host_ready;
    struct P4SDMMCHost *host;
    ULONG host_generation;
};

struct sdcard_Bus
{
    ULONG sdcb_BusNum;
    IPTR sdcb_Private;
};

#define P4SD_PRIV(bus) ((struct p4sd_private *)(IPTR)(bus)->sdcb_Private)

static unsigned int checks;
static unsigned int cases;
static unsigned int memset_calls;
static unsigned int memcpy_calls;
static unsigned int bug_calls;
static unsigned int command_calls;
static unsigned int confirm_calls;
static unsigned int recover_calls;
static BOOL command_return;
static BOOL confirm_return;
static BOOL recover_return;
static struct P4HostCommand captured_request;
static struct P4HostCommandResult command_result;
static struct TagItem *confirm_response;
static IPTR response_data_seen_during_confirm;
static unsigned int events[8];
static unsigned int event_count;
static struct P4SDMMCHost host;
static struct p4sd_private test_priv;
static struct sdcard_Bus test_bus;
static UBYTE test_data[P4SD_MAX_DATA_LEN + 1UL];
static UBYTE expected_data[P4SD_MAX_DATA_LEN + 1UL];

#define EVENT_COMMAND 1U
#define EVENT_CONFIRM 2U

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void *mock_memset(void *destination, int value, size_t length)
{
    unsigned char *bytes = (unsigned char *)destination;
    size_t index;

    ++memset_calls;
    for (index = 0; index < length; ++index)
        bytes[index] = (unsigned char)(unsigned int)value;
    return destination;
}

static void *mock_memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    size_t index;

    ++memcpy_calls;
    for (index = 0; index < length; ++index)
        to[index] = from[index];
    return destination;
}

#define memset mock_memset
#define memcpy mock_memcpy

static void mock_bug(const char *format, ...)
{
    (void)format;
    ++bug_calls;
}

#define bug(...) mock_bug(__VA_ARGS__)

static BOOL p4sd_confirm_tran(struct sdcard_Bus *target_bus);
static BOOL p4sd_recover_multiblock(struct sdcard_Bus *target_bus);

/* INSERT_ACTUAL_SOURCE */

static BOOL mock_host_command(struct P4SDMMCHost *target_host,
                              const struct P4HostCommand *request,
                              struct P4HostCommandResult *result)
{
    (void)target_host;
    CHECK(command_calls == 0);
    ++command_calls;
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = EVENT_COMMAND;
    captured_request = *request;
    *result = command_result;
    return command_return;
}

static BOOL p4sd_confirm_tran(struct sdcard_Bus *target_bus)
{
    (void)target_bus;
    ++confirm_calls;
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = EVENT_CONFIRM;
    response_data_seen_during_confirm = confirm_response != NULL ?
        confirm_response->ti_Data : 0;
    return confirm_return;
}

static BOOL p4sd_recover_multiblock(struct sdcard_Bus *target_bus)
{
    ++recover_calls;
    if (!recover_return)
        P4SD_PRIV(target_bus)->host_ready = FALSE;
    return recover_return;
}

static void begin_case(void)
{
    ++cases;
}

static void reset_mocks(void)
{
    memset_calls = 0;
    memcpy_calls = 0;
    bug_calls = 0;
    command_calls = 0;
    confirm_calls = 0;
    recover_calls = 0;
    command_return = TRUE;
    confirm_return = TRUE;
    recover_return = TRUE;
    confirm_response = NULL;
    response_data_seen_during_confirm = 0;
    event_count = 0;
    captured_request.slot = 0;
    captured_request.generation = 0;
    captured_request.index = 0;
    captured_request.argument = 0;
    captured_request.response_flags = 0;
    captured_request.response_error_mask = 0;
    captured_request.data = NULL;
    captured_request.length = 0;
    captured_request.block_size = 0;
    captured_request.writing = FALSE;
    captured_request.early_cmd18 = FALSE;
    command_result.status = P4HOST_COMMAND_OK;
    command_result.started = TRUE;
    command_result.raw = 0;
    command_result.idstatus = 0;
    command_result.response[0] = 0x11223344UL;
    command_result.response[1] = 0x22334455UL;
    command_result.response[2] = 0x33445566UL;
    command_result.response[3] = 0x44556677UL;
}

static void prepare(void)
{
    reset_mocks();
    host.command = mock_host_command;
    host.dma_faulted = FALSE;
    host.generation = 0xA51C23D4UL;
    test_priv.host_ready = TRUE;
    test_priv.host = &host;
    test_priv.host_generation = host.generation;
    test_bus.sdcb_BusNum = 3;
    test_bus.sdcb_Private = (IPTR)&test_priv;
}

static void check_request(ULONG index, ULONG argument, ULONG response_flags)
{
    CHECK(command_calls == 1);
    CHECK(captured_request.slot == 0U);
    CHECK(captured_request.generation == test_priv.host_generation);
    CHECK(captured_request.index == index);
    CHECK(captured_request.argument == argument);
    CHECK(captured_request.response_flags == response_flags);
    CHECK(captured_request.response_error_mask == 0UL);
    CHECK(captured_request.data == NULL);
    CHECK(captured_request.length == 0UL);
    CHECK(captured_request.block_size == 0UL);
    CHECK(captured_request.writing == FALSE);
    CHECK(captured_request.early_cmd18 == FALSE);
    CHECK(memset_calls == 1U);
}

static void check_data_request(ULONG index, ULONG argument, ULONG response_flags,
                               UBYTE *data, ULONG length, ULONG block_size,
                               BOOL early_cmd18)
{
    CHECK(command_calls == 1U);
    CHECK(captured_request.slot == 0U);
    CHECK(captured_request.generation == test_priv.host_generation);
    CHECK(captured_request.index == index);
    CHECK(captured_request.argument == argument);
    CHECK(captured_request.response_flags == response_flags);
    CHECK(captured_request.response_error_mask == 0UL);
    CHECK(captured_request.data == data);
    CHECK(captured_request.length == length);
    CHECK(captured_request.block_size == block_size);
    CHECK(captured_request.writing == FALSE);
    CHECK(captured_request.early_cmd18 == early_cmd18);
    CHECK(memset_calls == 1U);
}

static void prepare_data(void)
{
    size_t index;

    for (index = 0; index < sizeof(test_data); ++index)
    {
        UBYTE value = (UBYTE)((index * 29U + 7U) & 0xFFU);
        test_data[index] = value;
        expected_data[index] = value;
    }
}

static void check_data_unchanged(void)
{
    CHECK(memcmp(test_data, expected_data, sizeof(test_data)) == 0);
}

static void check_data_refusal(ULONG index, UBYTE *data, ULONG length,
                               ULONG data_flags)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xA1B2C3D4UL};

    prepare();
    prepare_data();
    CHECK(p4sd_shared_command(&test_bus, index, 0x1234UL, MMC_RSP_R1,
                              &response, data, length, data_flags) ==
          (ULONG)-1);
    CHECK(command_calls == 0U);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    CHECK(response.ti_Data == (IPTR)0xA1B2C3D4UL);
    CHECK(memset_calls == 1U);
    check_data_unchanged();
}

static void test_r1_request_and_publication(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xDEADBEEFUL};
    ULONG actual;

    begin_case();
    prepare();
    command_result.response[0] = 0x89ABCDEFUL;
    actual = p4sd_shared_command(&test_bus, 7UL, 0x10203040UL, MMC_RSP_R1,
                                 &response, NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(7UL, 0x10203040UL, MMC_RSP_R1);
    CHECK(response.ti_Data == (IPTR)0x89ABCDEFUL);
    CHECK(memcpy_calls == 0U);
    CHECK(confirm_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    CHECK(bug_calls == 0U);
}

static void test_r2_request_and_word_publication(void)
{
    ULONG response_words[4] = {1UL, 2UL, 3UL, 4UL};
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)response_words};
    ULONG actual;

    begin_case();
    prepare();
    command_result.response[0] = 0x01020304UL;
    command_result.response[1] = 0x11121314UL;
    command_result.response[2] = 0x21222324UL;
    command_result.response[3] = 0x31323334UL;
    actual = p4sd_shared_command(&test_bus, 2UL, 0x55667788UL, MMC_RSP_R2,
                                 &response, NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(2UL, 0x55667788UL, MMC_RSP_R2);
    CHECK(response_words[0] == 0x01020304UL);
    CHECK(response_words[1] == 0x11121314UL);
    CHECK(response_words[2] == 0x21222324UL);
    CHECK(response_words[3] == 0x31323334UL);
    CHECK(memcpy_calls == 1U);
}

static void test_null_response_tag_pointer(void)
{
    ULONG actual;

    begin_case();
    prepare();
    actual = p4sd_shared_command(&test_bus, 0UL, 0UL, MMC_RSP_R1, NULL,
                                 NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(0UL, 0UL, MMC_RSP_R1);
    CHECK(memcpy_calls == 0U);
}

static void test_absent_response_is_untouched(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xAABBCCDDUL};
    ULONG actual;

    begin_case();
    prepare();
    actual = p4sd_shared_command(&test_bus, 8UL, 9UL, MMC_RSP_NONE, &response,
                                 NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(8UL, 9UL, MMC_RSP_NONE);
    CHECK(response.ti_Data == (IPTR)0xAABBCCDDUL);
    CHECK(memcpy_calls == 0U);
}

static void test_null_r2_storage_is_safe(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, 0};
    ULONG actual;

    begin_case();
    prepare();
    actual = p4sd_shared_command(&test_bus, 9UL, 10UL, MMC_RSP_R2, &response,
                                 NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(9UL, 10UL, MMC_RSP_R2);
    CHECK(response.ti_Data == 0);
    CHECK(memcpy_calls == 0U);
}

static void test_cmd17_single_block_read_mapping(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xCAFED00DUL};
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    command_result.response[0] = 0x76543210UL;
    actual = p4sd_shared_command(&test_bus, MMC_CMD_READ_SINGLE_BLOCK,
                                 0x12345678UL, MMC_RSP_R1, &response,
                                 unaligned, 512UL, MMC_DATA_READ);
    CHECK(actual == 0UL);
    check_data_request(MMC_CMD_READ_SINGLE_BLOCK, 0x12345678UL, MMC_RSP_R1,
                       unaligned, 512UL, 512UL, FALSE);
    CHECK(captured_request.writing == FALSE);
    CHECK(response.ti_Data == (IPTR)0x76543210UL);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_cmd18_two_block_read_mapping(void)
{
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    actual = p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK,
                                 0x200UL, MMC_RSP_R1, NULL, unaligned,
                                 1024UL, MMC_DATA_READ);
    CHECK(actual == 0UL);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 0x200UL, MMC_RSP_R1,
                       unaligned, 1024UL, 512UL, TRUE);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_cmd18_maximum_read_mapping(void)
{
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    actual = p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK,
                                 0x400UL, MMC_RSP_R1, NULL, unaligned,
                                 P4SD_MAX_DATA_LEN, MMC_DATA_READ);
    CHECK(actual == 0UL);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 0x400UL, MMC_RSP_R1,
                       unaligned, 65536UL, 512UL, TRUE);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_cmd51_short_scr_read_mapping(void)
{
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    actual = p4sd_shared_command(&test_bus, 51UL, 0UL, MMC_RSP_R1, NULL,
                                 unaligned, 8UL, MMC_DATA_READ);
    CHECK(actual == 0UL);
    check_data_request(51UL, 0UL, MMC_RSP_R1, unaligned, 8UL, 8UL, FALSE);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_null_data_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, NULL, 512UL, MMC_DATA_READ);
}

static void test_write_only_data_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 512UL,
                       MMC_DATA_WRITE);
}

static void test_read_write_data_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 512UL,
                       MMC_DATA_READ | MMC_DATA_WRITE);
}

static void test_stream_data_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 512UL,
                       MMC_DATA_READ | MMC_DATA_STREAM);
}

static void test_oversize_data_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data,
                       P4SD_MAX_DATA_LEN + 1UL, MMC_DATA_READ);
}

static void test_bad_block_length_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 513UL,
                       MMC_DATA_READ);
}

static void test_short_cmd17_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 8UL,
                       MMC_DATA_READ);
}

static void test_multi_block_cmd17_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, test_data, 1024UL,
                       MMC_DATA_READ);
}

static void test_short_cmd18_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_MULTIPLE_BLOCK, test_data, 512UL,
                       MMC_DATA_READ);
}

static void test_zero_length_cmd17_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_SINGLE_BLOCK, NULL, 0UL, 0UL);
}

static void test_zero_length_cmd18_refused_without_callback_or_mutation(void)
{
    begin_case();
    check_data_refusal(MMC_CMD_READ_MULTIPLE_BLOCK, NULL, 0UL, 0UL);
}

static void test_ordinary_failure_preserves_ready_and_response(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xCAFEBABEUL};
    ULONG actual;

    begin_case();
    prepare();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_ERROR;
    command_result.started = FALSE;
    actual = p4sd_shared_command(&test_bus, 7UL, 11UL, MMC_RSP_R1, &response,
                                 NULL, 0UL, 0UL);
    CHECK(actual == (ULONG)-1);
    check_request(7UL, 11UL, MMC_RSP_R1);
    CHECK(response.ti_Data == (IPTR)0xCAFEBABEUL);
    CHECK(test_priv.host_ready == TRUE);
    CHECK(confirm_calls == 0U);
    CHECK(bug_calls == 1U);
}

static void test_r2_failure_does_not_publish(void)
{
    ULONG response_words[4] = {
        0xA1A2A3A4UL, 0xB1B2B3B4UL, 0xC1C2C3C4UL, 0xD1D2D3D4UL
    };
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)response_words};
    ULONG actual;

    begin_case();
    prepare();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_ERROR;
    actual = p4sd_shared_command(&test_bus, 2UL, 12UL, MMC_RSP_R2, &response,
                                 NULL, 0UL, 0UL);
    CHECK(actual == (ULONG)-1);
    check_request(2UL, 12UL, MMC_RSP_R2);
    CHECK(response.ti_Data == (IPTR)response_words);
    CHECK(response_words[0] == 0xA1A2A3A4UL);
    CHECK(response_words[1] == 0xB1B2B3B4UL);
    CHECK(response_words[2] == 0xC1C2C3C4UL);
    CHECK(response_words[3] == 0xD1D2D3D4UL);
    CHECK(memcpy_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
}

static void test_data_error_before_start_skips_recovery_and_publication(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0xAABBCCDDUL};
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_ERROR;
    command_result.started = FALSE;
    actual = p4sd_shared_command(&test_bus, MMC_CMD_READ_SINGLE_BLOCK, 3UL,
                                 MMC_RSP_R1, &response, unaligned, 512UL,
                                 MMC_DATA_READ);
    CHECK(actual == (ULONG)-1);
    check_data_request(MMC_CMD_READ_SINGLE_BLOCK, 3UL, MMC_RSP_R1, unaligned,
                       512UL, 512UL, FALSE);
    CHECK(response.ti_Data == (IPTR)0xAABBCCDDUL);
    CHECK(memcpy_calls == 0U);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    CHECK(bug_calls == 1U);
    check_data_unchanged();
}

static void test_started_cmd17_error_confirms_tran(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x10293847UL};
    UBYTE *unaligned = &test_data[1];
    ULONG actual;

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_CARD_ERROR;
    command_result.started = TRUE;
    actual = p4sd_shared_command(&test_bus, MMC_CMD_READ_SINGLE_BLOCK, 4UL,
                                 MMC_RSP_R1, &response, unaligned, 512UL,
                                 MMC_DATA_READ);
    CHECK(actual == (ULONG)-1);
    check_data_request(MMC_CMD_READ_SINGLE_BLOCK, 4UL, MMC_RSP_R1, unaligned,
                       512UL, 512UL, FALSE);
    CHECK(confirm_calls == 1U);
    CHECK(recover_calls == 0U);
    CHECK(event_count == 2U);
    CHECK(events[0] == EVENT_COMMAND);
    CHECK(events[1] == EVENT_CONFIRM);
    CHECK(response.ti_Data == (IPTR)0x10293847UL);
    CHECK(memcpy_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_started_cmd17_error_confirm_failure_disables_ready(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x56473829UL};
    UBYTE *unaligned = &test_data[1];

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_ERROR;
    command_result.started = TRUE;
    confirm_return = FALSE;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_READ_SINGLE_BLOCK, 5UL,
                              MMC_RSP_R1, &response, unaligned, 512UL,
                              MMC_DATA_READ) == (ULONG)-1);
    check_data_request(MMC_CMD_READ_SINGLE_BLOCK, 5UL, MMC_RSP_R1, unaligned,
                       512UL, 512UL, FALSE);
    CHECK(confirm_calls == 1U);
    CHECK(recover_calls == 0U);
    CHECK(response.ti_Data == (IPTR)0x56473829UL);
    CHECK(memcpy_calls == 0U);
    CHECK(test_priv.host_ready == FALSE);
    check_data_unchanged();
}

static void test_started_cmd18_error_recovery_success(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x89ABCDEFUL};
    UBYTE *unaligned = &test_data[1];

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_TIMEOUT;
    command_result.started = TRUE;
    recover_return = TRUE;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK, 6UL,
                              MMC_RSP_R1, &response, unaligned, 1024UL,
                              MMC_DATA_READ) == (ULONG)-1);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 6UL, MMC_RSP_R1,
                       unaligned, 1024UL, 512UL, TRUE);
    CHECK(recover_calls == 1U);
    CHECK(confirm_calls == 0U);
    CHECK(response.ti_Data == (IPTR)0x89ABCDEFUL);
    CHECK(memcpy_calls == 0U);
    CHECK(test_priv.host_ready == TRUE);
    check_data_unchanged();
}

static void test_started_cmd18_error_recovery_failure_disables_ready(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x76543210UL};
    UBYTE *unaligned = &test_data[1];

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_ERROR;
    command_result.started = TRUE;
    recover_return = FALSE;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK, 7UL,
                              MMC_RSP_R1, &response, unaligned, 1024UL,
                              MMC_DATA_READ) == (ULONG)-1);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 7UL, MMC_RSP_R1,
                       unaligned, 1024UL, 512UL, TRUE);
    CHECK(recover_calls == 1U);
    CHECK(confirm_calls == 0U);
    CHECK(response.ti_Data == (IPTR)0x76543210UL);
    CHECK(memcpy_calls == 0U);
    CHECK(test_priv.host_ready == FALSE);
    check_data_unchanged();
}

static void test_dma_fault_skips_data_recovery(void)
{
    UBYTE *unaligned = &test_data[1];

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_DMA_ERROR;
    command_result.started = TRUE;
    host.dma_faulted = TRUE;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK, 8UL,
                              MMC_RSP_R1, NULL, unaligned, 1024UL,
                              MMC_DATA_READ) == (ULONG)-1);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 8UL, MMC_RSP_R1,
                       unaligned, 1024UL, 512UL, TRUE);
    CHECK(test_priv.host_ready == FALSE);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    check_data_unchanged();
}

static void test_stale_generation_skips_data_recovery(void)
{
    UBYTE *unaligned = &test_data[1];

    begin_case();
    prepare();
    prepare_data();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_REFUSED;
    command_result.started = TRUE;
    ++host.generation;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_READ_MULTIPLE_BLOCK, 9UL,
                              MMC_RSP_R1, NULL, unaligned, 1024UL,
                              MMC_DATA_READ) == (ULONG)-1);
    check_data_request(MMC_CMD_READ_MULTIPLE_BLOCK, 9UL, MMC_RSP_R1,
                       unaligned, 1024UL, 512UL, TRUE);
    CHECK(test_priv.host_ready == FALSE);
    CHECK(confirm_calls == 0U);
    CHECK(recover_calls == 0U);
    check_data_unchanged();
}

static void test_dma_fault_failure_disables_ready(void)
{
    begin_case();
    prepare();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_DMA_ERROR;
    host.dma_faulted = TRUE;
    CHECK(p4sd_shared_command(&test_bus, 7UL, 0UL, MMC_RSP_R1, NULL,
                              NULL, 0UL, 0UL) ==
          (ULONG)-1);
    CHECK(test_priv.host_ready == FALSE);
    CHECK(confirm_calls == 0U);
}

static void test_stale_generation_failure_disables_ready(void)
{
    begin_case();
    prepare();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_REFUSED;
    ++host.generation;
    CHECK(p4sd_shared_command(&test_bus, 7UL, 0UL, MMC_RSP_R1, NULL,
                              NULL, 0UL, 0UL) ==
          (ULONG)-1);
    CHECK(test_priv.host_ready == FALSE);
}

static void test_cmd12_engine_failure_disables_ready(void)
{
    begin_case();
    prepare();
    command_return = FALSE;
    command_result.status = P4HOST_COMMAND_TIMEOUT;
    CHECK(p4sd_shared_command(&test_bus, MMC_CMD_STOP_TRANSMISSION, 0UL,
                              MMC_RSP_R1b, NULL, NULL, 0UL, 0UL) == (ULONG)-1);
    CHECK(test_priv.host_ready == FALSE);
    CHECK(confirm_calls == 0U);
}

static void test_cmd12_confirm_failure_keeps_response_private(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x10293847UL};
    ULONG actual;

    begin_case();
    prepare();
    confirm_return = FALSE;
    confirm_response = &response;
    actual = p4sd_shared_command(&test_bus, MMC_CMD_STOP_TRANSMISSION, 0UL,
                                 MMC_RSP_R1b, &response, NULL, 0UL, 0UL);
    CHECK(actual == (ULONG)-1);
    check_request(MMC_CMD_STOP_TRANSMISSION, 0UL, MMC_RSP_R1b);
    CHECK(confirm_calls == 1U);
    CHECK(event_count == 2U);
    CHECK(events[0] == EVENT_COMMAND);
    CHECK(events[1] == EVENT_CONFIRM);
    CHECK(response_data_seen_during_confirm == (IPTR)0x10293847UL);
    CHECK(response.ti_Data == (IPTR)0x10293847UL);
    CHECK(test_priv.host_ready == FALSE);
    CHECK(memcpy_calls == 0U);
}

static void test_cmd12_publication_follows_confirm(void)
{
    struct TagItem response = {SDCARD_TAG_RSP, (IPTR)0x19283746UL};
    ULONG actual;

    begin_case();
    prepare();
    command_result.response[0] = 0xFEDCBA98UL;
    confirm_response = &response;
    actual = p4sd_shared_command(&test_bus, MMC_CMD_STOP_TRANSMISSION, 0UL,
                                 MMC_RSP_R1b, &response, NULL, 0UL, 0UL);
    CHECK(actual == 0UL);
    check_request(MMC_CMD_STOP_TRANSMISSION, 0UL, MMC_RSP_R1b);
    CHECK(confirm_calls == 1U);
    CHECK(event_count == 2U);
    CHECK(events[0] == EVENT_COMMAND);
    CHECK(events[1] == EVENT_CONFIRM);
    CHECK(response_data_seen_during_confirm == (IPTR)0x19283746UL);
    CHECK(response.ti_Data == (IPTR)0xFEDCBA98UL);
    CHECK(test_priv.host_ready == TRUE);
}

static void test_actual_denylist_is_exact(void)
{
    static const ULONG denied[] = {
        20UL, 24UL, 25UL, 26UL, 27UL, 28UL, 29UL,
        32UL, 33UL, 35UL, 36UL, 38UL, 42UL, 56UL
    };
    ULONG command;
    size_t index;

    begin_case();
    for (command = 0; command <= 63UL; ++command)
    {
        BOOL expected = FALSE;
        for (index = 0; index < sizeof(denied) / sizeof(denied[0]); ++index)
            if (command == denied[index])
                expected = TRUE;
        CHECK(p4sd_media_mutating_command(command) == expected);
    }
    CHECK(p4sd_media_mutating_command(64UL) == FALSE);
}

int main(void)
{
    test_r1_request_and_publication();
    test_r2_request_and_word_publication();
    test_null_response_tag_pointer();
    test_absent_response_is_untouched();
    test_null_r2_storage_is_safe();
    test_cmd17_single_block_read_mapping();
    test_cmd18_two_block_read_mapping();
    test_cmd18_maximum_read_mapping();
    test_cmd51_short_scr_read_mapping();
    test_null_data_refused_without_callback_or_mutation();
    test_write_only_data_refused_without_callback_or_mutation();
    test_read_write_data_refused_without_callback_or_mutation();
    test_stream_data_refused_without_callback_or_mutation();
    test_oversize_data_refused_without_callback_or_mutation();
    test_bad_block_length_refused_without_callback_or_mutation();
    test_short_cmd17_refused_without_callback_or_mutation();
    test_multi_block_cmd17_refused_without_callback_or_mutation();
    test_short_cmd18_refused_without_callback_or_mutation();
    test_zero_length_cmd17_refused_without_callback_or_mutation();
    test_zero_length_cmd18_refused_without_callback_or_mutation();
    test_ordinary_failure_preserves_ready_and_response();
    test_r2_failure_does_not_publish();
    test_data_error_before_start_skips_recovery_and_publication();
    test_started_cmd17_error_confirms_tran();
    test_started_cmd17_error_confirm_failure_disables_ready();
    test_started_cmd18_error_recovery_success();
    test_started_cmd18_error_recovery_failure_disables_ready();
    test_dma_fault_skips_data_recovery();
    test_stale_generation_skips_data_recovery();
    test_dma_fault_failure_disables_ready();
    test_stale_generation_failure_disables_ready();
    test_cmd12_engine_failure_disables_ready();
    test_cmd12_confirm_failure_keeps_response_private();
    test_cmd12_publication_follows_confirm();
    test_actual_denylist_is_exact();
    printf("PASS: %u cases, %u checks\n", cases, checks);
    return 0;
}
