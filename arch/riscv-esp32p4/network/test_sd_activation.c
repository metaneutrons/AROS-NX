/* Host fixture for generic SD unit activation and its frequency helpers.
 * tools/test-sd-activation.py inserts the current production source bodies. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef uint8_t UBYTE;
typedef int8_t BYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef uint64_t UQUAD;
typedef uintptr_t IPTR;
typedef void *APTR;
typedef int BOOL;

#define TRUE 1
#define FALSE 0

/* INSERT_ACTUAL_SD_CONSTANTS */

/* INSERT_ACTUAL_TAG_ITEM_TYPE */

struct Node
{
    struct Node *ln_Succ;
    struct Node *ln_Pred;
};

struct List
{
    struct Node *lh_Head;
    struct Node *lh_Tail;
    struct Node *lh_TailPred;
};

struct Unit
{
    ULONG placeholder;
};

struct SCSICmd;
struct Interrupt;
struct Task;
struct MsgPort;
struct IORequest;
struct sdcard_Bus;
struct sdcard_BusUnits;

/* INSERT_ACTUAL_SD_UNIT_TYPE */

/* The bus is a mock, while the unit and command/status constants are taken
 * from the production headers. */
struct sdcard_Bus
{
    ULONG sdcb_BusFlags;
    ULONG sdcb_BusStatus;
    UBYTE sdcb_SectorShift;
    ULONG (*sdcb_IOReadLong)(ULONG, struct sdcard_Bus *);
    void (*sdcb_SetClock)(ULONG, struct sdcard_Bus *);
    void (*sdcb_SetBusWidth)(UBYTE, struct sdcard_Bus *);
    ULONG (*sdcb_SendCmd)(struct TagItem *, struct sdcard_Bus *);
    ULONG (*sdcb_WaitCmd)(ULONG, ULONG, struct sdcard_Bus *);
    BOOL (*sdcb_BeginTransaction)(struct sdcard_Bus *, BOOL may_wait);
    void (*sdcb_EndTransaction)(struct sdcard_Bus *);
};

/* INSERT_ACTUAL_SD_FUNCTION_MACROS */

#define D(...) ((void)0)
#define DFUNCS(...) ((void)0)
#define bug(...) mock_bug(__VA_ARGS__)

ULONG FNAME_SDCUNIT(WaitStatus)(ULONG timeout, struct sdcard_Unit *unit);
ULONG FNAME_SDCUNIT(SDSCChangeFrequency)(struct sdcard_Unit *unit);
ULONG FNAME_SDCUNIT(MMCChangeFrequency)(struct sdcard_Unit *unit);
ULONG FNAME_SDCUNIT(SDSCSwitch)(BOOL test, int group, UBYTE value,
                                APTR buffer, struct sdcard_Unit *unit);
ULONG FNAME_SDCUNIT(MMCSwitch)(UBYTE offset, UBYTE value,
                               struct sdcard_Unit *unit);
void FNAME_SDCBUS(ADMAVerify)(struct sdcard_Unit *unit);

#define COMMAND_CAPACITY 128U
#define STATUS_CAPACITY 32U
#define NO_COMMAND 0xFFFFFFFFUL
#define READY_TRAN_STATUS \
    (MMC_STATUS_RDY_FOR_DATA | MMC_STATUS_STATE_TRAN)

struct command_event
{
    ULONG command;
    IPTR argument;
    ULONG response_type;
    unsigned int occurrence;
};

struct test_model
{
    struct command_event events[COMMAND_CAPACITY];
    unsigned int event_count;
    unsigned int send_calls;
    unsigned int wait_calls;
    unsigned int command_occurrences[64];
    unsigned int wait_occurrences[64];
    ULONG pending_command;
    unsigned int pending_occurrence;
    struct TagItem *pending_tags;
    BOOL pending_valid;
    ULONG fail_send_command;
    unsigned int fail_send_occurrence;
    ULONG fail_wait_command;
    unsigned int fail_wait_occurrence;
    ULONG error_response_command;
    unsigned int error_response_occurrence;
    ULONG status[STATUS_CAPACITY];
    unsigned int status_count;
    unsigned int status_index;
    ULONG default_status;
    ULONG delays;
    ULONG delayed_us;
    ULONG frequency_result;
    ULONG frequency_flags;
    ULONG frequency_flags_seen;
    unsigned int sd_frequency_calls;
    unsigned int mmc_frequency_calls;
    unsigned int set_clock_calls;
    ULONG clock_hz;
    unsigned int set_width_calls;
    UBYTE bus_width;
    unsigned int adma_verify_calls;
    BOOL invalidate_on_verify;
    BOOL verify_seen_before_second_status;
    ULONG io_read_value;
    unsigned int io_read_calls;
    unsigned int begin_calls;
    unsigned int end_calls;
    BOOL begin_succeeds;
    BOOL last_may_wait;
    BOOL transaction_owned;
    unsigned int bug_calls;
    ULONG scr_word0;
    BOOL sd_highspeed_supported;
    unsigned int sd_test_busy_count;
    unsigned int sd_test_calls;
    ULONG sd_switch_result_word;
    UBYTE ext_csd_card_type;
    UBYTE ext_csd_hs_timing;
};

static struct test_model model;
static struct sdcard_Bus mock_bus_state;
static struct sdcard_Unit unit;
static unsigned int test_cases;
static unsigned int checks;

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression)                                                    \
    do                                                                       \
    {                                                                        \
        ++checks;                                                            \
        if (!(expression))                                                   \
            fail_check(#expression, __FILE__, __LINE__);                    \
    } while (0)

static void begin_case(void)
{
    ++test_cases;
}

static IPTR get_tag(const struct TagItem *tags, ULONG tag)
{
    unsigned int index;
    for (index = 0U; index < 16U; ++index)
    {
        if (tags[index].ti_Tag == TAG_DONE)
            break;
        if (tags[index].ti_Tag == tag)
            return tags[index].ti_Data;
    }
    return (IPTR)0;
}

static void put_tag(struct TagItem *tags, ULONG tag, IPTR value)
{
    unsigned int index;
    for (index = 0U; index < 16U; ++index)
    {
        if (tags[index].ti_Tag == TAG_DONE)
            break;
        if (tags[index].ti_Tag == tag)
        {
            tags[index].ti_Data = value;
            return;
        }
    }
}

#ifdef TEST_FREQUENCY_ONLY
static ULONG byteswap32(ULONG value)
{
    return ((value & 0x000000FFUL) << 24) |
           ((value & 0x0000FF00UL) << 8) |
           ((value & 0x00FF0000UL) >> 8) |
           ((value & 0xFF000000UL) >> 24);
}

#define AROS_BE2LONG(value) byteswap32((ULONG)(value))
#endif

static void write_be32(void *buffer, unsigned int word, ULONG value)
{
    UBYTE *bytes = (UBYTE *)buffer + (size_t)word * 4U;
    bytes[0] = (UBYTE)(value >> 24);
    bytes[1] = (UBYTE)(value >> 16);
    bytes[2] = (UBYTE)(value >> 8);
    bytes[3] = (UBYTE)value;
}

static ULONG mock_send_cmd(struct TagItem *tags, struct sdcard_Bus *mock_bus)
{
    ULONG command = (ULONG)get_tag(tags, SDCARD_TAG_CMD);
    unsigned int occurrence;
    (void)mock_bus;
    ++model.send_calls;
    if (command >= 64UL || model.event_count >= COMMAND_CAPACITY)
        return (ULONG)-1;
    occurrence = ++model.command_occurrences[command];
    model.events[model.event_count].command = command;
    model.events[model.event_count].argument = get_tag(tags, SDCARD_TAG_ARG);
    model.events[model.event_count].response_type =
        (ULONG)get_tag(tags, SDCARD_TAG_RSPTYPE);
    model.events[model.event_count].occurrence = occurrence;
    ++model.event_count;
    model.pending_command = command;
    model.pending_occurrence = occurrence;
    model.pending_tags = tags;
    model.pending_valid = TRUE;
    if (model.fail_send_command == command &&
        (model.fail_send_occurrence == 0U ||
         model.fail_send_occurrence == occurrence))
    {
        model.pending_valid = FALSE;
        return (ULONG)-1;
    }
    return 0UL;
}

static void mock_fill_command_data(void)
{
    IPTR data = get_tag(model.pending_tags, SDCARD_TAG_DATA);
    IPTR length = get_tag(model.pending_tags, SDCARD_TAG_DATALEN);
    if (!data || !length)
        return;

    if (model.pending_command == SD_CMD_APP_SEND_SCR && length >= 8U)
    {
        memset((void *)data, 0, (size_t)length);
        write_be32((void *)data, 0U, model.scr_word0);
    }
    else if (model.pending_command == SD_CMD_SWITCH_FUNC && length >= 64U)
    {
        IPTR argument = get_tag(model.pending_tags, SDCARD_TAG_ARG);
        memset((void *)data, 0, (size_t)length);
        write_be32((void *)data, 3U,
                   model.sd_highspeed_supported ? SD_SCR_HIGHSPEED : 0UL);
        if ((argument & (IPTR)(1UL << 31)) == 0)
        {
            ++model.sd_test_calls;
            write_be32((void *)data, 7U,
                       model.sd_test_calls <= model.sd_test_busy_count ?
                       SD_SCR_HIGHSPEED : 0UL);
        }
        else
            write_be32((void *)data, 4U, model.sd_switch_result_word);
    }
    else if (model.pending_command == MMC_CMD_SEND_EXT_CSD && length >= 512U)
    {
        UBYTE *bytes = (UBYTE *)data;
        memset(bytes, 0, (size_t)length);
        bytes[0xC4] = model.ext_csd_card_type;
        bytes[0xB9] = model.ext_csd_hs_timing;
    }
}

static ULONG mock_wait_cmd(ULONG mask, ULONG timeout,
                           struct sdcard_Bus *mock_bus)
{
    ULONG command;
    ULONG response = 0UL;
    unsigned int occurrence;
    (void)mock_bus;
    ++model.wait_calls;
    if (!model.pending_valid)
        return (ULONG)-1;
    command = model.pending_command;
    occurrence = model.pending_occurrence;
    if (command < 64UL)
        ++model.wait_occurrences[command];
    model.pending_valid = FALSE;
    if (command == MMC_CMD_SEND_STATUS && occurrence >= 2U &&
        model.adma_verify_calls != 0U)
        model.verify_seen_before_second_status = TRUE;
    if (model.fail_wait_command == command &&
        (model.fail_wait_occurrence == 0U ||
         model.fail_wait_occurrence == occurrence))
        return (ULONG)-1;
    if (command == MMC_CMD_SEND_STATUS)
    {
        if (model.status_index < model.status_count)
            response = model.status[model.status_index++];
        else
            response = model.default_status;
    }
    else if (model.error_response_command == command &&
             (model.error_response_occurrence == 0U ||
              model.error_response_occurrence == occurrence))
        response = MMC_STATUS_ILLEGAL_COMMAND;
    if (model.pending_tags)
        put_tag(model.pending_tags, SDCARD_TAG_RSP, response);
    mock_fill_command_data();
    (void)mask;
    (void)timeout;
    return 0UL;
}

static void sdcard_Udelay(ULONG usec)
{
    ++model.delays;
    model.delayed_us += usec;
}

static void mock_bug(const char *format, ...)
{
    (void)format;
    ++model.bug_calls;
}

static ULONG mock_read_long(ULONG offset, struct sdcard_Bus *mock_bus)
{
    (void)offset;
    (void)mock_bus;
    ++model.io_read_calls;
    return model.io_read_value;
}

static void reset_fixture(void)
{
    memset(&model, 0, sizeof(model));
    memset(&mock_bus_state, 0, sizeof(mock_bus_state));
    memset(&unit, 0, sizeof(unit));
    model.fail_send_command = NO_COMMAND;
    model.fail_wait_command = NO_COMMAND;
    model.error_response_command = NO_COMMAND;
    model.default_status = READY_TRAN_STATUS;
    model.frequency_result = 0UL;
    model.sd_highspeed_supported = TRUE;
    model.scr_word0 = 0x01000000UL | SD_SCR_DATA4BIT;
    model.sd_switch_result_word = 0x01000000UL;
    model.ext_csd_card_type = MMC_HS_52MHZ;
    model.ext_csd_hs_timing = 1U;
    mock_bus_state.sdcb_SendCmd = mock_send_cmd;
    mock_bus_state.sdcb_WaitCmd = mock_wait_cmd;
    mock_bus_state.sdcb_IOReadLong = mock_read_long;
    mock_bus_state.sdcb_SectorShift = 9U;
    unit.sdcu_Bus = &mock_bus_state;
    unit.sdcu_CardRCA = 0x1234UL;
    unit.sdcu_UnitNum = 2UL;
}

static unsigned int count_command(ULONG command)
{
    return command < 64UL ? model.command_occurrences[command] : 0U;
}

static const struct command_event *event_for_command(ULONG command,
                                                     unsigned int occurrence)
{
    unsigned int index;
    for (index = 0U; index < model.event_count; ++index)
    {
        if (model.events[index].command == command &&
            model.events[index].occurrence == occurrence)
            return &model.events[index];
    }
    return NULL;
}

#ifndef TEST_FREQUENCY_ONLY
/* INSERT_ACTUAL_BUS_TRANSACTION_HELPERS */

static BOOL mock_begin_transaction(struct sdcard_Bus *mock_bus, BOOL may_wait)
{
    (void)mock_bus;
    ++model.begin_calls;
    model.last_may_wait = may_wait;
    if (!model.begin_succeeds)
        return FALSE;
    model.transaction_owned = TRUE;
    return TRUE;
}

static void mock_end_transaction(struct sdcard_Bus *mock_bus)
{
    (void)mock_bus;
    ++model.end_calls;
    model.transaction_owned = FALSE;
}

static void mock_set_clock(ULONG hz, struct sdcard_Bus *mock_bus)
{
    (void)mock_bus;
    ++model.set_clock_calls;
    model.clock_hz = hz;
}

static void mock_set_width(UBYTE width, struct sdcard_Bus *mock_bus)
{
    (void)mock_bus;
    ++model.set_width_calls;
    model.bus_width = width;
}

static ULONG mock_frequency(struct sdcard_Unit *mock_unit, BOOL mmc)
{
    model.frequency_flags_seen = mock_unit->sdcu_Flags;
    mock_unit->sdcu_Flags |= model.frequency_flags;
    if (mmc)
        ++model.mmc_frequency_calls;
    else
        ++model.sd_frequency_calls;
    return model.frequency_result;
}

ULONG FNAME_SDCUNIT(SDSCChangeFrequency)(struct sdcard_Unit *mock_unit)
{
    return mock_frequency(mock_unit, FALSE);
}

ULONG FNAME_SDCUNIT(MMCChangeFrequency)(struct sdcard_Unit *mock_unit)
{
    return mock_frequency(mock_unit, TRUE);
}

void FNAME_SDCBUS(ADMAVerify)(struct sdcard_Unit *mock_unit)
{
    (void)mock_unit;
    ++model.adma_verify_calls;
    if (model.invalidate_on_verify)
        model.default_status = MMC_STATUS_ILLEGAL_COMMAND;
}

/* INSERT_ACTUAL_ACTIVATION_PRODUCTION */

static void prepare_sdsc_4bit(void)
{
    reset_fixture();
    model.frequency_flags = AF_Card_4bitData;
    mock_bus_state.sdcb_SetClock = mock_set_clock;
    mock_bus_state.sdcb_SetBusWidth = mock_set_width;
    mock_bus_state.sdcb_BeginTransaction = mock_begin_transaction;
    mock_bus_state.sdcb_EndTransaction = mock_end_transaction;
    model.begin_succeeds = TRUE;
}

static void test_wait_status_zero_and_finite_poll(void)
{
    begin_case();
    reset_fixture();
    CHECK(FNAME_SDCUNIT(WaitStatus)(0UL, &unit) == (ULONG)-1);
    CHECK(model.send_calls == 0U);
    CHECK(model.wait_calls == 0U);
    CHECK(model.delays == 0UL);

    reset_fixture();
    model.status[0] = MMC_STATUS_RDY_FOR_DATA | MMC_STATUS_STATE_PRG;
    model.status[1] = MMC_STATUS_STATE_TRAN;
    model.status[2] = READY_TRAN_STATUS;
    model.status_count = 3U;
    CHECK(FNAME_SDCUNIT(WaitStatus)(3UL, &unit) == 0UL);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 3U);
    CHECK(model.delays == 2UL);
    CHECK(model.delayed_us == 2000UL);
}

static void test_wait_status_error_precedence_and_bounds(void)
{
    begin_case();
    reset_fixture();
    model.status[0] = MMC_STATUS_ILLEGAL_COMMAND | READY_TRAN_STATUS;
    model.status_count = 1U;
    CHECK(FNAME_SDCUNIT(WaitStatus)(10UL, &unit) == (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 1U);
    CHECK(model.delays == 0UL);

    reset_fixture();
    model.default_status = MMC_STATUS_STATE_TRAN;
    CHECK(FNAME_SDCUNIT(WaitStatus)(4UL, &unit) == (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 4U);
    CHECK(model.delays == 4UL);

    reset_fixture();
    model.fail_wait_command = MMC_CMD_SEND_STATUS;
    model.fail_wait_occurrence = 0U;
    CHECK(FNAME_SDCUNIT(WaitStatus)(20UL, &unit) == (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 5U);
    CHECK(model.wait_calls == 5U);
    CHECK(model.delays == 4UL);
}

static void test_activation_success_paths(void)
{
    begin_case();
    prepare_sdsc_4bit();
    unit.sdcu_Flags = AF_Card_HighCapacity | AF_Card_HighSpeed |
                      AF_Card_HighSpeed52 | AF_Card_4bitData;
    model.frequency_flags = AF_Card_HighSpeed | AF_Card_4bitData;
    CHECK(sdcard_StartUnitLocked(&unit) == TRUE);
    CHECK(model.sd_frequency_calls == 1U);
    CHECK(model.mmc_frequency_calls == 0U);
    CHECK(model.frequency_flags_seen == AF_Card_HighCapacity);
    CHECK(count_command(MMC_CMD_SET_BLOCKLEN) == 0U);
    CHECK(count_command(MMC_CMD_APP_CMD) == 1U);
    CHECK(count_command(SD_CMD_APP_SET_BUS_WIDTH) == 1U);
    CHECK(model.clock_hz == 50000000UL);
    CHECK(model.bus_width == 4U);
    CHECK(model.adma_verify_calls == 1U);
    CHECK(model.verify_seen_before_second_status == TRUE);

    begin_case();
    prepare_sdsc_4bit();
    CHECK(sdcard_StartUnitLocked(&unit) == TRUE);
    CHECK(count_command(MMC_CMD_SET_BLOCKLEN) == 1U);
    CHECK(event_for_command(MMC_CMD_SET_BLOCKLEN, 1U)->argument == 512U);
    CHECK(model.sd_frequency_calls == 1U);
    CHECK(model.clock_hz == 25000000UL);
    CHECK(model.set_width_calls == 1U);
    CHECK(model.adma_verify_calls == 1U);

    begin_case();
    prepare_sdsc_4bit();
    unit.sdcu_Flags = AF_Card_MMC | AF_Card_HighCapacity |
                      AF_Card_HighSpeed | AF_Card_4bitData;
    model.frequency_flags = AF_Card_HighSpeed | AF_Card_HighSpeed52;
    CHECK(sdcard_StartUnitLocked(&unit) == TRUE);
    CHECK(model.mmc_frequency_calls == 1U);
    CHECK(model.sd_frequency_calls == 0U);
    CHECK(model.frequency_flags_seen == (AF_Card_MMC | AF_Card_HighCapacity));
    CHECK(count_command(MMC_CMD_SET_BLOCKLEN) == 0U);
    CHECK(model.clock_hz == 52000000UL);
    CHECK(model.adma_verify_calls == 1U);
}

static void test_activation_failures_and_r1_commands(void)
{
    static const ULONG commands[] = {
        MMC_CMD_SELECT_CARD,
        MMC_CMD_SET_BLOCKLEN,
        MMC_CMD_APP_CMD,
        SD_CMD_APP_SET_BUS_WIDTH
    };
    unsigned int command_index;
    unsigned int failure_mode;
    begin_case();
    reset_fixture();
    mock_bus_state.sdcb_SectorShift = 32U;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(model.send_calls == 0U);

    for (command_index = 0U;
         command_index < (unsigned int)(sizeof(commands) / sizeof(commands[0]));
         ++command_index)
    {
        for (failure_mode = 0U; failure_mode < 3U; ++failure_mode)
        {
            begin_case();
            prepare_sdsc_4bit();
            if (failure_mode == 0U)
            {
                model.fail_send_command = commands[command_index];
                model.fail_send_occurrence = 1U;
            }
            else if (failure_mode == 1U)
            {
                model.fail_wait_command = commands[command_index];
                model.fail_wait_occurrence = 1U;
            }
            else
            {
                model.error_response_command = commands[command_index];
                model.error_response_occurrence = 1U;
            }
            CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
            CHECK(count_command(commands[command_index]) == 1U);
            CHECK(model.adma_verify_calls == 0U);
            if (failure_mode == 0U)
                CHECK(model.wait_occurrences[commands[command_index]] == 0U);
            else
                CHECK(model.wait_occurrences[commands[command_index]] == 1U);
            if (commands[command_index] == MMC_CMD_SELECT_CARD)
                CHECK(model.io_read_calls == 1U);
            else
                CHECK(model.io_read_calls == 0U);
            if (commands[command_index] == SD_CMD_APP_SET_BUS_WIDTH)
                CHECK(model.set_width_calls == 0U);
        }
    }

    begin_case();
    prepare_sdsc_4bit();
    model.frequency_result = (ULONG)-1;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(model.set_clock_calls == 0U);
    CHECK(model.adma_verify_calls == 0U);

    begin_case();
    prepare_sdsc_4bit();
    mock_bus_state.sdcb_IOReadLong = NULL;
    model.fail_send_command = MMC_CMD_SELECT_CARD;
    model.fail_send_occurrence = 1U;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(model.io_read_calls == 0U);

    begin_case();
    prepare_sdsc_4bit();
    model.status[0] = MMC_STATUS_ILLEGAL_COMMAND;
    model.status_count = 1U;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 1U);
    CHECK(model.sd_frequency_calls == 0U);

    begin_case();
    prepare_sdsc_4bit();
    model.status[0] = READY_TRAN_STATUS;
    model.status_count = 1U;
    model.invalidate_on_verify = TRUE;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(model.adma_verify_calls == 1U);
    CHECK(model.verify_seen_before_second_status == TRUE);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 2U);

    begin_case();
    prepare_sdsc_4bit();
    model.frequency_flags = AF_Card_4bitData;
    mock_bus_state.sdcb_SetBusWidth = NULL;
    CHECK(sdcard_StartUnitLocked(&unit) == FALSE);
    CHECK(count_command(MMC_CMD_APP_CMD) == 0U);
    CHECK(count_command(SD_CMD_APP_SET_BUS_WIDTH) == 0U);
    CHECK(model.adma_verify_calls == 0U);
}

static void test_public_start_transaction(void)
{
    begin_case();
    prepare_sdsc_4bit();
    model.begin_succeeds = FALSE;
    CHECK(FNAME_SDCBUS(StartUnit)(&unit) == FALSE);
    CHECK(model.begin_calls == 1U);
    CHECK(model.end_calls == 0U);
    CHECK(model.last_may_wait == FALSE);
    CHECK(model.send_calls == 0U);

    begin_case();
    prepare_sdsc_4bit();
    CHECK(FNAME_SDCBUS(StartUnit)(&unit) == TRUE);
    CHECK(model.begin_calls == 1U);
    CHECK(model.end_calls == 1U);
    CHECK(model.last_may_wait == FALSE);
    CHECK(model.transaction_owned == FALSE);

    begin_case();
    prepare_sdsc_4bit();
    model.fail_send_command = MMC_CMD_SELECT_CARD;
    model.fail_send_occurrence = 1U;
    CHECK(FNAME_SDCBUS(StartUnit)(&unit) == FALSE);
    CHECK(model.begin_calls == 1U);
    CHECK(model.end_calls == 1U);
    CHECK(model.last_may_wait == FALSE);
    CHECK(model.transaction_owned == FALSE);
    CHECK(model.io_read_calls == 1U);
}

int main(void)
{
    test_wait_status_zero_and_finite_poll();
    test_wait_status_error_precedence_and_bounds();
    test_activation_success_paths();
    test_activation_failures_and_r1_commands();
    test_public_start_transaction();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
#else
/* INSERT_ACTUAL_FREQUENCY_PRODUCTION */

static void test_sd_switch_argument_and_errors(void)
{
    ULONG response_buffer[16];
    const struct command_event *event;
    begin_case();
    reset_fixture();
    memset(response_buffer, 0, sizeof(response_buffer));
    CHECK(FNAME_SDCUNIT(SDSCSwitch)(FALSE, 0, 1U, response_buffer, &unit) == 0UL);
    event = event_for_command(SD_CMD_SWITCH_FUNC, 1U);
    CHECK(event != NULL);
    CHECK(event->argument == (IPTR)0x80FFFFF1UL);

    begin_case();
    reset_fixture();
    model.error_response_command = SD_CMD_SWITCH_FUNC;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(SDSCSwitch)(TRUE, 0, 1U, response_buffer, &unit) ==
          (ULONG)-1);
}

static void test_sd_frequency_status_and_switch_failures(void)
{
    begin_case();
    reset_fixture();
    model.error_response_command = MMC_CMD_APP_CMD;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_APP_SEND_SCR) == 0U);

    begin_case();
    reset_fixture();
    model.error_response_command = SD_CMD_APP_SEND_SCR;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_SWITCH_FUNC) == 0U);

    begin_case();
    reset_fixture();
    model.error_response_command = SD_CMD_SWITCH_FUNC;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_SWITCH_FUNC) == 1U);

    begin_case();
    reset_fixture();
    model.error_response_command = SD_CMD_SWITCH_FUNC;
    model.error_response_occurrence = 2U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_SWITCH_FUNC) == 2U);

    begin_case();
    reset_fixture();
    model.sd_switch_result_word = 0x02000000UL;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_SWITCH_FUNC) == 2U);

    begin_case();
    reset_fixture();
    model.sd_test_busy_count = 3U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == 0UL);
    CHECK(model.sd_test_calls == 3U);
    CHECK(count_command(SD_CMD_SWITCH_FUNC) == 3U);
    CHECK((unit.sdcu_Flags & AF_Card_HighSpeed) == 0UL);

    begin_case();
    reset_fixture();
    model.fail_wait_command = SD_CMD_APP_SEND_SCR;
    model.fail_wait_occurrence = 0U;
    CHECK(FNAME_SDCUNIT(SDSCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(SD_CMD_APP_SEND_SCR) == 3U);
}

static void test_mmc_switch_and_frequency_errors(void)
{
    begin_case();
    reset_fixture();
    model.error_response_command = MMC_CMD_SWITCH;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(MMCSwitch)(EXT_CSD_HS_TIMING, 1U, &unit) ==
          (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 0U);

    begin_case();
    reset_fixture();
    model.status[0] = MMC_STATUS_ILLEGAL_COMMAND;
    model.status_count = 1U;
    CHECK(FNAME_SDCUNIT(MMCSwitch)(EXT_CSD_HS_TIMING, 1U, &unit) ==
          (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_STATUS) == 1U);

    begin_case();
    reset_fixture();
    unit.sdcu_Flags = AF_Card_MMC;
    CHECK(FNAME_SDCUNIT(MMCChangeFrequency)(&unit) == 0UL);
    CHECK(count_command(MMC_CMD_SEND_EXT_CSD) == 2U);
    CHECK(count_command(MMC_CMD_SWITCH) == 1U);
    CHECK(model.events[1].argument ==
          (IPTR)((MMC_SWITCH_WRITE_BYTE << 24) |
                 (EXT_CSD_HS_TIMING << 16) | (1U << 8)));
    CHECK(model.events[1].response_type == MMC_RSP_R1b);
    CHECK((unit.sdcu_Flags & (AF_Card_HighSpeed | AF_Card_HighSpeed52)) ==
          (AF_Card_HighSpeed | AF_Card_HighSpeed52));

    begin_case();
    reset_fixture();
    unit.sdcu_Flags = AF_Card_MMC;
    model.error_response_command = MMC_CMD_SEND_EXT_CSD;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(MMCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(MMC_CMD_SWITCH) == 0U);

    begin_case();
    reset_fixture();
    unit.sdcu_Flags = AF_Card_MMC;
    model.error_response_command = MMC_CMD_SWITCH;
    model.error_response_occurrence = 1U;
    CHECK(FNAME_SDCUNIT(MMCChangeFrequency)(&unit) == (ULONG)-1);
    CHECK(count_command(MMC_CMD_SEND_EXT_CSD) == 1U);

    begin_case();
    reset_fixture();
    unit.sdcu_Flags = AF_Card_MMC;
    model.error_response_command = MMC_CMD_SEND_EXT_CSD;
    model.error_response_occurrence = 2U;
    CHECK(FNAME_SDCUNIT(MMCChangeFrequency)(&unit) == (ULONG)-1);

    begin_case();
    reset_fixture();
    mock_bus_state.sdcb_BusFlags = AF_Bus_SPI;
    unit.sdcu_Flags = AF_Card_MMC;
    CHECK(FNAME_SDCUNIT(MMCChangeFrequency)(&unit) == 0UL);
    CHECK(model.send_calls == 0U);
}

int main(void)
{
    test_sd_switch_argument_and_errors();
    test_sd_frequency_status_and_switch_failures();
    test_mmc_switch_and_frequency_errors();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
#endif
