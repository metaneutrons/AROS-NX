/* Host fixture for the generic SD identity and publication path.
 * tools/test-sd-identity.py inserts the current production definitions and
 * the relevant public structure declarations. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define TAG_USER 0x80000000UL
#define TAG_DONE 0UL
#define SDHCI_READONLY 1
#define MODEL_OCR_BUSY (1UL << 31)

/* INSERT_ACTUAL_SD_CONSTANTS */

struct TagItem
{
    ULONG ti_Tag;
    IPTR ti_Data;
};

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
struct sdcard_Bus;
struct sdcard_Unit;

/* INSERT_ACTUAL_SD_TYPES */

struct DeviceNode
{
    const char *dn_Name;
};

struct SDCardBase
{
    APTR sdcard_MemPool;
};

typedef struct SDCardBase *LIBBASETYPEPTR;

struct sdcard_Bus
{
    struct SDCardBase *sdcb_DeviceBase;
    struct sdcard_BusUnits *sdcb_BusUnits;
    ULONG sdcb_BusNum;
    ULONG sdcb_BusFlags;
    ULONG sdcb_Power;
    ULONG (*sdcb_SendCmd)(struct TagItem *, struct sdcard_Bus *);
    ULONG (*sdcb_WaitCmd)(ULONG, ULONG, struct sdcard_Bus *);
    void (*sdcb_SetPowerLevel)(ULONG, BOOL, struct sdcard_Bus *);
};

struct bus_units_layout
{
    ULONG unit_base;
    ULONG unit_max;
    ULONG unit_count;
    struct sdcard_Unit *entries[1];
};

union bus_units_model
{
    struct sdcard_BusUnits header;
    struct bus_units_layout layout;
};

typedef char bus_units_pointer_offset_matches[
    (offsetof(struct bus_units_layout, entries) ==
     offsetof(struct sdcard_BusUnits, sdcbu_Units)) ? 1 : -1];
typedef char bus_units_single_slot_size_matches[
    (sizeof(struct bus_units_layout) == sizeof(struct sdcard_BusUnits)) ? 1 : -1];

#define FNAME_SDCBUS(name) SDCBUS_##name
#define FNAME_SDCIO(name) SDCIO_##name
#define SDCBUS_SendCmd(tags, bus) ((bus)->sdcb_SendCmd((tags), (bus)))
#define SDCBUS_WaitCmd(mask, timeout, bus) \
    ((bus)->sdcb_WaitCmd((mask), (timeout), (bus)))
#define SDCBUS_SetPowerLevel(levels, lowest, bus) \
    ((bus)->sdcb_SetPowerLevel((levels), (lowest), (bus)))
#define NEWLIST(list_pointer)                                                  \
    do                                                                         \
    {                                                                          \
        (list_pointer)->lh_Head = (struct Node *)&(list_pointer)->lh_Tail;      \
        (list_pointer)->lh_Tail = NULL;                                        \
        (list_pointer)->lh_TailPred = (struct Node *)&(list_pointer)->lh_Head;  \
    } while (0)
#define MOD_NAME_STRING "sdcard.device"

static ULONG mock_send_cmd(struct TagItem *tags, struct sdcard_Bus *bus);
static ULONG mock_wait_cmd(ULONG mask, ULONG timeout, struct sdcard_Bus *bus);
static void mock_set_power(ULONG levels, BOOL lowest, struct sdcard_Bus *bus);
static void sdcard_Udelay(ULONG usec);
static APTR AllocVecPooled(APTR pool, ULONG size);
static void FreeVecPooled(APTR pool, APTR memory);
static struct DeviceNode *MakeDosNode(IPTR *parameters);
static void AddBootNode(ULONG priority, ULONG flags,
                        struct DeviceNode *device_node, APTR boot_node);
static void mock_bug(const char *format, ...);

#define bug(...) mock_bug(__VA_ARGS__)

static BYTE SDCIO_ReadSector32(struct sdcard_Unit *, ULONG, ULONG, APTR, ULONG *);
static BYTE SDCIO_WriteSector32(struct sdcard_Unit *, ULONG, ULONG, APTR, ULONG *);
static BYTE SDCIO_ReadSector64(struct sdcard_Unit *, UQUAD, ULONG, APTR, ULONG *);
static BYTE SDCIO_WriteSector64(struct sdcard_Unit *, UQUAD, ULONG, APTR, ULONG *);
static BYTE SDCIO_Eject(struct sdcard_Unit *);

ULONG FNAME_SDCBUS(Rsp136Unpack)(ULONG *buffer, ULONG offset, const ULONG length);

/* Registration publication is guarded in production. This single-task
 * identity fixture does not model SMP lock behavior; recovery tests do. */
static void SDCBUS_LockUnit(struct sdcard_Bus *target_bus)
{
    (void)target_bus;
}

static void SDCBUS_UnlockUnit(struct sdcard_Bus *target_bus)
{
    (void)target_bus;
}

/* INSERT_ACTUAL_SD_PRODUCTION */

#define CMD_LOG_CAPACITY 2208U
#define EXPECTED_VOLTAGE 0x00300000UL
#define TEST_RCA 0x1234UL

struct command_event
{
    ULONG command;
    ULONG argument;
    ULONG response_type;
    IPTR response_target;
};

struct card_model
{
    struct command_event commands[CMD_LOG_CAPACITY];
    unsigned int send_calls;
    unsigned int wait_calls;
    unsigned int command_count;
    unsigned int acmd41_attempts;
    unsigned int set_power_calls;
    unsigned int delay_calls;
    ULONG delayed_us;
    ULONG ready_after;
    ULONG ocr_flags;
    BOOL ocr_omit_voltage;
    ULONG cmd8_echo;
    ULONG app_cmd_response;
    ULONG rca_response;
    ULONG cid[4];
    ULONG csd[4];
    ULONG fail_send_at;
    ULONG fail_wait_at;
    BOOL cmd8_no_response;
    BOOL sticky_failure;
    BOOL backend_failed;
    BOOL allocation_fails;
    BOOL make_node_fails;
    ULONG power_levels;
    BOOL lowest_voltage;
    struct TagItem *pending_tags;
    struct command_event pending;
    BOOL pending_valid;
};

static struct card_model model;
static struct sdcard_Bus bus;
static union bus_units_model bus_units_storage;
#define bus_units (bus_units_storage.header)
#define unit_slots (bus_units_storage.layout.entries)
static struct sdcard_Unit allocated_unit;
static struct SDCardBase device_base;
static struct DeviceNode device_node;
static IPTR captured_dos_parameters[24];
static unsigned int alloc_calls;
static unsigned int free_calls;
static unsigned int make_node_calls;
static unsigned int add_boot_node_calls;
static unsigned int bug_calls;
static ULONG allocated_size;
static ULONG boot_priority;
static ULONG boot_flags;
static struct DeviceNode *boot_device_node;
static APTR boot_node;
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

static void set_csd_bits(ULONG words[4], unsigned int offset,
                         unsigned int length, ULONG value)
{
    unsigned int bit;
    for (bit = 0U; bit < length; ++bit)
    {
        unsigned int position = offset + bit;
        unsigned int word_index = 3U - position / 32U;
        unsigned int bit_index = position % 32U;
        if ((value >> bit) & 1UL)
            words[word_index] |= (ULONG)(1UL << bit_index);
    }
}

static void configure_csd(ULONG structure, ULONG size, ULONG read_length,
                          ULONG size_multiplier)
{
    memset(model.csd, 0, sizeof(model.csd));
    set_csd_bits(model.csd, 126U, 2U, structure);
    if (structure == 0U)
    {
        set_csd_bits(model.csd, 80U, 4U, read_length);
        set_csd_bits(model.csd, 62U, 12U, size);
        set_csd_bits(model.csd, 47U, 3U, size_multiplier);
    }
    else if (structure == 1U)
        set_csd_bits(model.csd, 48U, 22U, size);
}

static void reset_fixture(void)
{
    memset(&model, 0, sizeof(model));
    memset(&bus, 0, sizeof(bus));
    memset(&bus_units_storage, 0, sizeof(bus_units_storage));
    memset(&allocated_unit, 0, sizeof(allocated_unit));
    memset(&device_base, 0, sizeof(device_base));
    memset(captured_dos_parameters, 0, sizeof(captured_dos_parameters));
    alloc_calls = 0U;
    free_calls = 0U;
    make_node_calls = 0U;
    add_boot_node_calls = 0U;
    bug_calls = 0U;
    allocated_size = 0UL;
    boot_priority = 0UL;
    boot_flags = 0UL;
    boot_device_node = NULL;
    boot_node = NULL;

    model.ready_after = 1UL;
    model.ocr_flags = OCR_HCS;
    model.cmd8_echo = 0x1AAUL;
    model.rca_response = TEST_RCA << 16;
    model.cid[0] = 0x10203040UL;
    model.cid[1] = 0x50607080UL;
    model.cid[2] = 0x90A0B0C0UL;
    model.cid[3] = 0xD0E0F001UL;
    configure_csd(1UL, 0x345UL, 0UL, 0UL);

    bus.sdcb_DeviceBase = &device_base;
    bus.sdcb_BusUnits = &bus_units;
    bus.sdcb_BusNum = 3UL;
    bus.sdcb_BusFlags = 0x40UL;
    bus.sdcb_Power = EXPECTED_VOLTAGE;
    bus.sdcb_SendCmd = mock_send_cmd;
    bus.sdcb_WaitCmd = mock_wait_cmd;
    bus.sdcb_SetPowerLevel = mock_set_power;
    device_base.sdcard_MemPool = (APTR)(uintptr_t)0x1234U;
    bus_units.sdcbu_UnitBase = 0UL;
    bus_units.sdcbu_UnitMax = 1UL;
    bus_units.sdcbu_UnitCnt = 0UL;
    device_node.dn_Name = "MMC0:0";
}

static ULONG mock_send_cmd(struct TagItem *tags, struct sdcard_Bus *target_bus)
{
    struct command_event event;
    CHECK(target_bus == &bus);
    CHECK(tags != NULL);
    CHECK(tags[0].ti_Tag == SDCARD_TAG_CMD);
    CHECK(tags[1].ti_Tag == SDCARD_TAG_ARG);
    CHECK(tags[2].ti_Tag == SDCARD_TAG_RSPTYPE);
    CHECK(tags[3].ti_Tag == SDCARD_TAG_RSP);
    CHECK(tags[4].ti_Tag == TAG_DONE);
    ++model.send_calls;
    event.command = (ULONG)tags[0].ti_Data;
    event.argument = (ULONG)tags[1].ti_Data;
    event.response_type = (ULONG)tags[2].ti_Data;
    event.response_target = tags[3].ti_Data;
    CHECK(model.command_count < CMD_LOG_CAPACITY);
    model.commands[model.command_count++] = event;

    if (model.backend_failed)
        return (ULONG)-1;
    if (model.fail_send_at == model.send_calls)
    {
        if (model.sticky_failure)
            model.backend_failed = TRUE;
        return (ULONG)-1;
    }
    if (event.command == SD_CMD_SEND_IF_COND && model.cmd8_no_response)
        return (ULONG)-1;

    model.pending = event;
    model.pending_tags = tags;
    model.pending_valid = TRUE;
    return 0UL;
}

static ULONG mock_wait_cmd(ULONG mask, ULONG timeout, struct sdcard_Bus *target_bus)
{
    ULONG response = 0UL;
    ULONG *response_words;
    CHECK(target_bus == &bus);
    CHECK(mask == (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DATA_INHIBIT));
    CHECK(timeout == 1000UL);
    CHECK(model.pending_valid);
    ++model.wait_calls;
    if (model.fail_wait_at == model.wait_calls)
    {
        if (model.sticky_failure)
            model.backend_failed = TRUE;
        model.pending_valid = FALSE;
        return (ULONG)-1;
    }

    switch (model.pending.command)
    {
        case SD_CMD_SEND_IF_COND:
            response = model.cmd8_echo;
            break;
        case MMC_CMD_APP_CMD:
            response = model.app_cmd_response;
            break;
        case SD_CMD_APP_SEND_OP_COND:
            ++model.acmd41_attempts;
            response = (model.ocr_omit_voltage ? 0UL :
                        bus.sdcb_Power & 0xFF8000UL) | model.ocr_flags;
            if (model.ready_after && model.acmd41_attempts >= model.ready_after)
                response |= MODEL_OCR_BUSY;
            break;
        case SD_CMD_SEND_RELATIVE_ADDR:
            response = model.rca_response;
            break;
        case MMC_CMD_ALL_SEND_CID:
            response_words = (ULONG *)(uintptr_t)model.pending.response_target;
            CHECK(response_words != NULL);
            memcpy(response_words, model.cid, sizeof(model.cid));
            break;
        case MMC_CMD_SEND_CSD:
            response_words = (ULONG *)(uintptr_t)model.pending.response_target;
            CHECK(response_words != NULL);
            memcpy(response_words, model.csd, sizeof(model.csd));
            break;
        default:
            break;
    }
    if (model.pending.response_type != MMC_RSP_R2)
        model.pending_tags[3].ti_Data = response;
    model.pending_valid = FALSE;
    return 0UL;
}

static void mock_set_power(ULONG levels, BOOL lowest, struct sdcard_Bus *target_bus)
{
    CHECK(target_bus == &bus);
    ++model.set_power_calls;
    model.power_levels = levels;
    model.lowest_voltage = lowest;
}

static void sdcard_Udelay(ULONG usec)
{
    ++model.delay_calls;
    model.delayed_us += usec;
}

static APTR AllocVecPooled(APTR pool, ULONG size)
{
    CHECK(pool == device_base.sdcard_MemPool);
    ++alloc_calls;
    allocated_size = size;
    return model.allocation_fails ? NULL : &allocated_unit;
}

static void FreeVecPooled(APTR pool, APTR memory)
{
    CHECK(pool == device_base.sdcard_MemPool);
    CHECK(memory == &allocated_unit);
    ++free_calls;
}

static struct DeviceNode *MakeDosNode(IPTR *parameters)
{
    ++make_node_calls;
    CHECK(parameters != NULL);
    memcpy(captured_dos_parameters, parameters, sizeof(captured_dos_parameters));
    return model.make_node_fails ? NULL : &device_node;
}

static void AddBootNode(ULONG priority, ULONG flags,
                        struct DeviceNode *node, APTR new_boot_node)
{
    ++add_boot_node_calls;
    boot_priority = priority;
    boot_flags = flags;
    boot_device_node = node;
    boot_node = new_boot_node;
}

static void mock_bug(const char *format, ...)
{
    CHECK(format != NULL);
    ++bug_calls;
}

static BYTE SDCIO_ReadSector32(struct sdcard_Unit *unit, ULONG block,
                               ULONG count, APTR buffer, ULONG *actual)
{
    (void)unit;
    (void)block;
    (void)count;
    (void)buffer;
    (void)actual;
    return 0;
}

static BYTE SDCIO_WriteSector32(struct sdcard_Unit *unit, ULONG block,
                                ULONG count, APTR buffer, ULONG *actual)
{
    (void)unit;
    (void)block;
    (void)count;
    (void)buffer;
    (void)actual;
    return 0;
}

static BYTE SDCIO_ReadSector64(struct sdcard_Unit *unit, UQUAD block,
                               ULONG count, APTR buffer, ULONG *actual)
{
    (void)unit;
    (void)block;
    (void)count;
    (void)buffer;
    (void)actual;
    return 0;
}

static BYTE SDCIO_WriteSector64(struct sdcard_Unit *unit, UQUAD block,
                                ULONG count, APTR buffer, ULONG *actual)
{
    (void)unit;
    (void)block;
    (void)count;
    (void)buffer;
    (void)actual;
    return 0;
}

static BYTE SDCIO_Eject(struct sdcard_Unit *unit)
{
    (void)unit;
    return 0;
}

static void expect_command(unsigned int index, ULONG command, ULONG argument,
                           ULONG response_type)
{
    CHECK(index < model.command_count);
    CHECK(model.commands[index].command == command);
    CHECK(model.commands[index].argument == argument);
    CHECK(model.commands[index].response_type == response_type);
}

static void check_no_publication(void)
{
    CHECK(alloc_calls == 0U);
    CHECK(free_calls == 0U);
    CHECK(make_node_calls == 0U);
    CHECK(add_boot_node_calls == 0U);
    CHECK((bus.sdcb_BusFlags & AF_Bus_MediaPresent) == 0UL);
    CHECK(bus_units.sdcbu_UnitCnt == 0UL);
    CHECK(unit_slots[0] == NULL);
}

static void expect_identification_failure(void)
{
    struct sdcard_CardInfo output;
    struct sdcard_CardInfo before;
    memset(&output, 0xA5, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    CHECK(!FNAME_SDCBUS(IdentifyCardLocked)(&bus, &output));
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    check_no_publication();
}

static void test_identify_sdhc_sequence_and_geometry(void)
{
    struct sdcard_CardInfo card;
    unsigned int index = 0U;
    ULONG polls;
    UQUAD cylinders;
    begin_case();
    reset_fixture();
    model.ready_after = 3UL;
    configure_csd(1UL, 0x345UL, 0UL, 0UL);

    CHECK(FNAME_SDCBUS(IdentifyCardLocked)(&bus, &card));
    CHECK(card.rca == TEST_RCA);
    CHECK(card.power == EXPECTED_VOLTAGE);
    CHECK(card.flags & AF_Card_HighCapacity);
    CHECK(card.block_words == 256UL);
    CHECK(card.sectors == 2U);
    cylinders = (UQUAD)(0x345UL + 1UL) * 512UL;
    CHECK(card.cylinders == (ULONG)cylinders);
    CHECK(card.capacity == cylinders * 2UL);
    CHECK(memcmp(card.cid, model.cid, sizeof(card.cid)) == 0);
    CHECK(memcmp(card.csd, model.csd, sizeof(card.csd)) == 0);
    CHECK(model.acmd41_attempts == 3U);
    CHECK(model.set_power_calls == 1U);
    CHECK(model.power_levels == EXPECTED_VOLTAGE);
    CHECK(model.lowest_voltage == TRUE);
    CHECK(model.command_count == 11U);
    expect_command(index++, MMC_CMD_GO_IDLE_STATE, 0UL, MMC_RSP_NONE);
    expect_command(index++, SD_CMD_SEND_IF_COND, 0x1AAUL, MMC_RSP_R7);
    for (polls = 0UL; polls < 3UL; ++polls)
    {
        expect_command(index++, MMC_CMD_APP_CMD, 0UL, MMC_RSP_R1);
        expect_command(index++, SD_CMD_APP_SEND_OP_COND,
                       EXPECTED_VOLTAGE | OCR_HCS, MMC_RSP_R3);
    }
    expect_command(index++, MMC_CMD_ALL_SEND_CID, 0UL, MMC_RSP_R2);
    expect_command(index++, SD_CMD_SEND_RELATIVE_ADDR, 0UL, MMC_RSP_R6);
    expect_command(index++, MMC_CMD_SEND_CSD, TEST_RCA << 16, MMC_RSP_R2);
    CHECK(index == model.command_count);
    check_no_publication();
}

static void test_identify_sdsc_legacy_sequence_and_geometry(void)
{
    struct sdcard_CardInfo card;
    unsigned int index = 0U;
    UQUAD cylinders;
    begin_case();
    reset_fixture();
    model.cmd8_no_response = TRUE;
    model.ocr_flags = 0UL;
    model.ready_after = 2UL;
    configure_csd(0UL, 0x155UL, 9UL, 3UL);

    CHECK(FNAME_SDCBUS(IdentifyCardLocked)(&bus, &card));
    CHECK(card.rca == TEST_RCA);
    CHECK(card.power == EXPECTED_VOLTAGE);
    CHECK(!(card.flags & AF_Card_HighCapacity));
    CHECK(card.block_words == 128UL);
    CHECK(card.sectors == 1U);
    cylinders = (UQUAD)(0x155UL + 1UL) << (3UL + 2UL);
    CHECK(card.cylinders == (ULONG)cylinders);
    CHECK(card.capacity == cylinders);
    CHECK(memcmp(card.cid, model.cid, sizeof(card.cid)) == 0);
    CHECK(model.acmd41_attempts == 2U);
    CHECK(model.command_count == 9U);
    expect_command(index++, MMC_CMD_GO_IDLE_STATE, 0UL, MMC_RSP_NONE);
    expect_command(index++, SD_CMD_SEND_IF_COND, 0x1AAUL, MMC_RSP_R7);
    expect_command(index++, MMC_CMD_APP_CMD, 0UL, MMC_RSP_R1);
    expect_command(index++, SD_CMD_APP_SEND_OP_COND,
                   EXPECTED_VOLTAGE, MMC_RSP_R3);
    expect_command(index++, MMC_CMD_APP_CMD, 0UL, MMC_RSP_R1);
    expect_command(index++, SD_CMD_APP_SEND_OP_COND,
                   EXPECTED_VOLTAGE, MMC_RSP_R3);
    expect_command(index++, MMC_CMD_ALL_SEND_CID, 0UL, MMC_RSP_R2);
    expect_command(index++, SD_CMD_SEND_RELATIVE_ADDR, 0UL, MMC_RSP_R6);
    expect_command(index++, MMC_CMD_SEND_CSD, TEST_RCA << 16, MMC_RSP_R2);
    CHECK(index == model.command_count);
    CHECK(model.set_power_calls == 1U);
    check_no_publication();
}

static void test_decode_card_info_boundaries(void)
{
    struct sdcard_CardInfo card;
    begin_case();
    reset_fixture();
    memset(&card, 0, sizeof(card));
    card.flags = AF_Card_HighCapacity;
    configure_csd(1UL, 7UL, 0UL, 0UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(sdcard_DecodeCardInfo(&card));
    CHECK(card.block_words == 256UL);
    CHECK(card.sectors == 2U);
    CHECK(card.cylinders == 4096UL);
    CHECK(card.capacity == 8192UL);

    memset(&card, 0, sizeof(card));
    configure_csd(0UL, 15UL, 10UL, 2UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(sdcard_DecodeCardInfo(&card));
    CHECK(card.block_words == 256UL);
    CHECK(card.sectors == 2U);
    CHECK(card.cylinders == 256UL);
    CHECK(card.capacity == 512UL);

    memset(&card, 0, sizeof(card));
    card.flags = AF_Card_HighCapacity;
    configure_csd(0UL, 15UL, 10UL, 2UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(!sdcard_DecodeCardInfo(&card));

    memset(&card, 0, sizeof(card));
    configure_csd(1UL, 7UL, 0UL, 0UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(!sdcard_DecodeCardInfo(&card));

    memset(&card, 0, sizeof(card));
    configure_csd(2UL, 7UL, 0UL, 0UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(!sdcard_DecodeCardInfo(&card));

    memset(&card, 0, sizeof(card));
    configure_csd(0UL, 15UL, 8UL, 2UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(!sdcard_DecodeCardInfo(&card));

    memset(&card, 0, sizeof(card));
    configure_csd(0UL, 15UL, 16UL, 2UL);
    memcpy(card.csd, model.csd, sizeof(card.csd));
    CHECK(!sdcard_DecodeCardInfo(&card));
}

static void test_malformed_responses_and_capacity_contradictions(void)
{
    begin_case();
    reset_fixture();
    model.cmd8_echo = 0x1ABUL;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.app_cmd_response = MMC_STATUS_ERROR;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.rca_response = (TEST_RCA << 16) | 0x2000UL;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.rca_response = 0UL;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.ocr_flags = 0UL;
    model.ready_after = 1UL;
    model.ocr_flags |= MODEL_OCR_BUSY;
    model.ocr_omit_voltage = TRUE;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    configure_csd(2UL, 7UL, 0UL, 0UL);
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.cmd8_no_response = TRUE;
    model.ocr_flags = OCR_HCS;
    expect_identification_failure();

    begin_case();
    reset_fixture();
    configure_csd(0UL, 15UL, 10UL, 2UL);
    expect_identification_failure();

    begin_case();
    reset_fixture();
    model.ocr_flags = 0UL;
    configure_csd(1UL, 7UL, 0UL, 0UL);
    expect_identification_failure();
}

static void test_send_and_wait_failure_preserve_output(void)
{
    ULONG failed_command;
    for (failed_command = 1UL; failed_command <= 11UL; ++failed_command)
    {
        unsigned int wait_failure;
        for (wait_failure = 0U; wait_failure < 2U; ++wait_failure)
        {
            struct sdcard_CardInfo output;
            struct sdcard_CardInfo before;
            begin_case();
            reset_fixture();
            model.ready_after = 3UL;
            model.sticky_failure = TRUE;
            if (wait_failure)
                model.fail_wait_at = failed_command;
            else
                model.fail_send_at = failed_command;
            memset(&output, (int)(0xA0U + failed_command), sizeof(output));
            memcpy(&before, &output, sizeof(before));
            CHECK(!FNAME_SDCBUS(IdentifyCardLocked)(&bus, &output));
            CHECK(memcmp(&output, &before, sizeof(output)) == 0);
            CHECK(model.command_count >= failed_command);
            CHECK(model.commands[failed_command - 1UL].command != 99UL);
            check_no_publication();
        }
    }
}

static void test_acmd41_attempt_bound(void)
{
    struct sdcard_CardInfo output;
    struct sdcard_CardInfo before;
    unsigned int index;
    unsigned int cmd55_count = 0U;
    unsigned int acmd41_count = 0U;
    begin_case();
    reset_fixture();
    model.ready_after = 0UL;
    memset(&output, 0x5A, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    CHECK(!FNAME_SDCBUS(IdentifyCardLocked)(&bus, &output));
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(model.acmd41_attempts == 1000U);
    CHECK(model.command_count == 2002U);
    CHECK(model.send_calls == 2002U);
    CHECK(model.wait_calls == 2002U);
    for (index = 0U; index < model.command_count; ++index)
    {
        if (model.commands[index].command == MMC_CMD_APP_CMD)
            ++cmd55_count;
        if (model.commands[index].command == SD_CMD_APP_SEND_OP_COND)
            ++acmd41_count;
    }
    CHECK(cmd55_count == 1000U);
    CHECK(acmd41_count == 1000U);
    CHECK(model.set_power_calls == 0U);
    check_no_publication();
}

static void prepare_registration(void)
{
    reset_fixture();
    bus_units.sdcbu_UnitCnt = 0UL;
    bus_units.sdcbu_UnitBase = 0UL;
    bus_units.sdcbu_UnitMax = 1UL;
}

static void check_registration_unpublished(void)
{
    CHECK(bus_units.sdcbu_UnitCnt == 0UL);
    CHECK(unit_slots[0] == NULL);
    CHECK((bus.sdcb_BusFlags & AF_Bus_MediaPresent) == 0UL);
    CHECK(add_boot_node_calls == 0U);
}

static void test_register_discovery_failure_is_unpublished(void)
{
    begin_case();
    prepare_registration();
    model.cmd8_echo = 0x1ABUL;
    CHECK(!sdcard_RegisterUnitLocked(&bus));
    CHECK(alloc_calls == 0U);
    CHECK(make_node_calls == 0U);
    CHECK(free_calls == 0U);
    check_registration_unpublished();
}

static void test_register_allocation_failure_is_unpublished(void)
{
    begin_case();
    prepare_registration();
    model.allocation_fails = TRUE;
    CHECK(!sdcard_RegisterUnitLocked(&bus));
    CHECK(alloc_calls == 1U);
    CHECK(allocated_size == sizeof(struct sdcard_Unit));
    CHECK(make_node_calls == 0U);
    CHECK(free_calls == 0U);
    check_registration_unpublished();
}

static void test_register_bootnode_failure_is_unpublished(void)
{
    begin_case();
    prepare_registration();
    model.make_node_fails = TRUE;
    CHECK(!sdcard_RegisterUnitLocked(&bus));
    CHECK(alloc_calls == 1U);
    CHECK(make_node_calls == 1U);
    CHECK(free_calls == 1U);
    CHECK(add_boot_node_calls == 0U);
    check_registration_unpublished();
}

static void check_published_unit(BOOL high_capacity)
{
    struct sdcard_Unit *unit = unit_slots[0];
    CHECK(unit == &allocated_unit);
    CHECK(bus_units.sdcbu_UnitCnt == 1UL);
    CHECK(bus.sdcb_BusFlags & AF_Bus_MediaPresent);
    CHECK(unit->sdcu_UnitNum == 0UL);
    CHECK(unit->sdcu_Bus == &bus);
    CHECK(unit->sdcu_CardRCA == TEST_RCA);
    CHECK(unit->sdcu_CardPower == EXPECTED_VOLTAGE);
    CHECK(((unit->sdcu_Flags & AF_Card_HighCapacity) != 0UL) ==
          (high_capacity != FALSE));
    CHECK(unit->sdcu_Flags & AF_Card_WriteProtect);
    CHECK(unit->sdcu_Read32 == SDCIO_ReadSector32);
    CHECK(unit->sdcu_Write32 == SDCIO_WriteSector32);
    CHECK(unit->sdcu_Eject == SDCIO_Eject);
    CHECK(unit->sdcu_Read64 == (high_capacity ? SDCIO_ReadSector64 : NULL));
    CHECK(unit->sdcu_Write64 == (high_capacity ? SDCIO_WriteSector64 : NULL));
    CHECK(memcmp(unit->sdcu_CardCID, model.cid, sizeof(model.cid)) == 0);
    CHECK(memcmp(unit->sdcu_CardCSD, model.csd, sizeof(model.csd)) == 0);
    CHECK(unit->sdcu_SoftList.lh_Head == (struct Node *)&unit->sdcu_SoftList.lh_Tail);
    CHECK(unit->sdcu_SoftList.lh_Tail == NULL);
    CHECK(unit->sdcu_SoftList.lh_TailPred == (struct Node *)&unit->sdcu_SoftList.lh_Head);
    CHECK(add_boot_node_calls == 1U);
    CHECK(boot_priority == 0UL);
    CHECK(boot_flags == 0UL);
    CHECK(boot_device_node == &device_node);
    CHECK(boot_node == NULL);
    CHECK(bug_calls == 1U);
    CHECK(strcmp((const char *)(uintptr_t)captured_dos_parameters[0], "MMC0") == 0);
    CHECK(strcmp((const char *)(uintptr_t)captured_dos_parameters[1],
                 MOD_NAME_STRING) == 0);
    CHECK(captured_dos_parameters[2] == 0U);
    CHECK(captured_dos_parameters[DE_TABLESIZE + 4U] == DE_BOOTBLOCKS);
    CHECK(captured_dos_parameters[DE_SIZEBLOCK + 4U] ==
          (high_capacity ? 256U : 128U));
    CHECK(captured_dos_parameters[DE_SECSPERBLOCK + 4U] ==
          (high_capacity ? 2U : 1U));
    CHECK(captured_dos_parameters[DE_HIGHCYL + 4U] ==
          (IPTR)(unit->sdcu_Cylinders - 1UL));
    CHECK(captured_dos_parameters[DE_NUMHEADS + 4U] == 1U);
    CHECK(captured_dos_parameters[DE_BLKSPERTRACK + 4U] == 1U);
    CHECK(captured_dos_parameters[DE_RESERVEDBLKS + 4U] == 2U);
    CHECK(captured_dos_parameters[DE_LOWCYL + 4U] == 0U);
    CHECK(captured_dos_parameters[DE_NUMBUFFERS + 4U] == 10U);
    CHECK(captured_dos_parameters[DE_BUFMEMTYPE + 4U] ==
          (MEMF_PUBLIC | MEMF_31BIT));
    CHECK(captured_dos_parameters[DE_MAXTRANSFER + 4U] == 0x00200000U);
    CHECK(captured_dos_parameters[DE_MASK + 4U] == 0x7FFFFFFEU);
    CHECK(captured_dos_parameters[DE_BOOTBLOCKS + 4U] == 2U);
}

static void test_register_sdhc_caches_identity_and_callbacks(void)
{
    ULONG initial_flags;
    begin_case();
    prepare_registration();
    initial_flags = bus.sdcb_BusFlags;
    CHECK(sdcard_RegisterUnitLocked(&bus));
    CHECK(alloc_calls == 1U);
    CHECK(free_calls == 0U);
    CHECK(make_node_calls == 1U);
    CHECK(bus.sdcb_BusFlags == (initial_flags | AF_Bus_MediaPresent));
    check_published_unit(TRUE);
}

static void test_register_sdsc_omits_64bit_callbacks(void)
{
    begin_case();
    prepare_registration();
    model.cmd8_no_response = TRUE;
    model.ocr_flags = 0UL;
    configure_csd(0UL, 0x155UL, 9UL, 3UL);
    CHECK(sdcard_RegisterUnitLocked(&bus));
    check_published_unit(FALSE);
}

static void test_register_full_unit_array_does_not_identify(void)
{
    begin_case();
    prepare_registration();
    bus_units.sdcbu_UnitCnt = bus_units.sdcbu_UnitMax;
    CHECK(!sdcard_RegisterUnitLocked(&bus));
    CHECK(model.send_calls == 0U);
    CHECK(alloc_calls == 0U);
    CHECK(make_node_calls == 0U);
    CHECK(bus_units.sdcbu_UnitCnt == bus_units.sdcbu_UnitMax);
    CHECK(add_boot_node_calls == 0U);
}

int main(void)
{
    test_identify_sdhc_sequence_and_geometry();
    test_identify_sdsc_legacy_sequence_and_geometry();
    test_decode_card_info_boundaries();
    test_malformed_responses_and_capacity_contradictions();
    test_send_and_wait_failure_preserve_output();
    test_acmd41_attempt_bound();
    test_register_discovery_failure_is_unpublished();
    test_register_allocation_failure_is_unpublished();
    test_register_bootnode_failure_is_unpublished();
    test_register_sdhc_caches_identity_and_callbacks();
    test_register_sdsc_omits_64bit_callbacks();
    test_register_full_unit_array_does_not_identify();
    printf("PASS: %u cases, %u checks\n", test_cases, checks);
    return 0;
}
