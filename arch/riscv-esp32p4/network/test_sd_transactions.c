/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <inttypes.h>
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
#define IOERR_ABORTED 5
#define IOERR_NOCMD 6
#define IOERR_OPENFAIL 1
#define TAG_DONE 0
#define SDCARD_TAG_CMD 1
#define SDCARD_TAG_ARG 2
#define SDCARD_TAG_RSPTYPE 3
#define SDCARD_TAG_RSP 4
#define SDCARD_TAG_DATA 5
#define SDCARD_TAG_DATALEN 6
#define SDCARD_TAG_DATAFLAGS 7
#define MMC_CMD_READ_SINGLE_BLOCK 17
#define MMC_CMD_READ_MULTIPLE_BLOCK 18
#define MMC_CMD_STOP_TRANSMISSION 12
#define MMC_RSP_R1 1
#define MMC_RSP_R1b 2
#define MMC_DATA_READ 1
#define SDHCI_PS_CMD_INHIBIT 1
#define SDHCI_PS_DATA_INHIBIT 2
/* INSERT_ACTUAL_UNIT_FLAGS */

#define FNAME_SDCIO(name) test_##name
#define FNAME_SDCBUS(name) test_bus_##name
#define FNAME_SDC(name) test_device_##name
#define D(expression) ((void)0)
#define DTRANS(expression) ((void)0)
#define DDEV(expression) ((void)0)
#define bug(...) ((void)0)
#define SDCBUS_SendCmd(tags, bus) mock_send_cmd((tags), (bus))
#define SDCBUS_WaitCmd(mask, timeout, bus) \
    mock_wait_cmd((mask), (timeout), (bus))
#define Reschedule() mock_reschedule()

struct TagItem
{
    IPTR ti_Tag;
    IPTR ti_Data;
};

struct sdcard_Unit;
struct sdcard_BusUnits
{
    ULONG sdcbu_UnitBase;
    ULONG sdcbu_UnitCnt;
    struct sdcard_Unit *sdcbu_Units;
};

struct sdcard_Bus
{
    struct sdcard_Bus *next;
    struct sdcard_BusUnits *sdcb_BusUnits;
    ULONG sdcb_SectorShift;
    BOOL (*sdcb_BeginTransaction)(struct sdcard_Bus *, BOOL may_wait);
    void (*sdcb_EndTransaction)(struct sdcard_Bus *);
};

struct Unit
{
    ULONG unit_OpenCnt;
};

struct sdcard_Unit
{
    struct Unit sdcu_Unit;
    struct sdcard_Bus *sdcu_Bus;
    ULONG sdcu_Flags;
    ULONG sdcu_UnitNum;
    ULONG sdcu_CardRCA;
    ULONG sdcu_CardPower;
    UQUAD sdcu_Capacity;
    ULONG sdcu_Cylinders;
    UBYTE sdcu_Heads;
    UBYTE sdcu_Sectors;
    BYTE (*sdcu_Read32)(struct sdcard_Unit *, ULONG, ULONG, APTR, ULONG *);
    BYTE (*sdcu_Read64)(struct sdcard_Unit *, UQUAD, ULONG, APTR, ULONG *);
    ULONG sdcu_ChangeNum;
};

struct IORequest
{
    BYTE io_Error;
    struct Unit *io_Unit;
};

struct sdcard_BusList
{
    struct sdcard_Bus *first;
};

struct SDCardBase
{
    struct sdcard_BusList sdcard_Buses;
};

typedef struct SDCardBase *LIBBASETYPEPTR;

#define ForeachNode(list, node) \
    for ((node) = (list)->first; (node) != NULL; (node) = (node)->next)

enum event_kind
{
    EVENT_BEGIN = 1,
    EVENT_SEND,
    EVENT_WAIT,
    EVENT_END,
    EVENT_YIELD
};

static unsigned int checks;
static unsigned int event_count;
static int event_kind[32];
static IPTR event_value[32];
static unsigned int begin_calls;
static unsigned int end_calls;
static BOOL begin_wait_modes[32];
static unsigned int send_calls;
static unsigned int wait_calls;
static unsigned int yield_calls;
static int owner_depth;
static int begin_allowed;
static unsigned int fail_send_call;
static unsigned int fail_wait_call;
static int sent_commands[16];
static unsigned int sent_command_count;
static unsigned int start_body_calls;
static unsigned int register_body_calls;
static BOOL start_body_result;
static BOOL register_body_result;
static ULONG start_body_flags;
static ULONG start_body_rca;
static struct sdcard_Unit *expected_start_source;
static unsigned int unit_lock_calls;
static unsigned int unit_unlock_calls;
static unsigned int unit_lock_depth;
static unsigned int unit_lock_errors;
static unsigned int callbacks_under_unit_lock;

static void record_external_callback(void)
{
    if (unit_lock_depth != 0U)
        ++callbacks_under_unit_lock;
}

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void record_event(int kind, IPTR value)
{
    CHECK(event_count < sizeof(event_kind) / sizeof(event_kind[0]));
    event_kind[event_count] = kind;
    event_value[event_count] = value;
    ++event_count;
}

static void reset_mocks(void)
{
    event_count = 0;
    begin_calls = 0;
    end_calls = 0;
    send_calls = 0;
    wait_calls = 0;
    yield_calls = 0;
    owner_depth = 0;
    unit_lock_calls = 0;
    unit_unlock_calls = 0;
    unit_lock_depth = 0;
    unit_lock_errors = 0;
    callbacks_under_unit_lock = 0;
    begin_allowed = 1;
    fail_send_call = 0;
    fail_wait_call = 0;
    sent_command_count = 0;
    memset(event_kind, 0, sizeof(event_kind));
    memset(event_value, 0, sizeof(event_value));
    memset(begin_wait_modes, 0, sizeof(begin_wait_modes));
    memset(sent_commands, 0, sizeof(sent_commands));
    start_body_flags = 0;
    start_body_rca = 0;
    expected_start_source = NULL;
}

static void reset_bus(struct sdcard_Bus *bus, BOOL with_hooks)
{
    memset(bus, 0, sizeof(*bus));
    bus->sdcb_SectorShift = 9;
    if (with_hooks)
    {
        bus->sdcb_BeginTransaction = NULL;
        bus->sdcb_EndTransaction = NULL;
    }
    reset_mocks();
}

static BOOL mock_begin(struct sdcard_Bus *bus, BOOL may_wait)
{
    record_external_callback();
    (void)bus;
    CHECK(begin_calls < sizeof(begin_wait_modes) / sizeof(begin_wait_modes[0]));
    begin_wait_modes[begin_calls] = may_wait;
    ++begin_calls;
    record_event(EVENT_BEGIN, begin_allowed ? owner_depth + 1 : 0);
    if (!begin_allowed)
        return FALSE;
    ++owner_depth;
    return TRUE;
}

static void mock_end(struct sdcard_Bus *bus)
{
    record_external_callback();
    (void)bus;
    CHECK(owner_depth > 0);
    --owner_depth;
    ++end_calls;
    record_event(EVENT_END, owner_depth);
}

static void install_hooks(struct sdcard_Bus *bus)
{
    bus->sdcb_BeginTransaction = mock_begin;
    bus->sdcb_EndTransaction = mock_end;
}

static BOOL hooks_are_paired(const struct sdcard_Bus *bus)
{
    return bus->sdcb_BeginTransaction && bus->sdcb_EndTransaction;
}

static ULONG mock_send_cmd(struct TagItem *tags, struct sdcard_Bus *bus)
{
    int command = (int)tags[0].ti_Data;
    record_external_callback();
    ++send_calls;
    if (hooks_are_paired(bus))
        CHECK(owner_depth > 0);
    else
        CHECK(owner_depth == 0);
    CHECK(sent_command_count < sizeof(sent_commands) / sizeof(sent_commands[0]));
    sent_commands[sent_command_count++] = command;
    record_event(EVENT_SEND, (IPTR)command);
    return send_calls == fail_send_call ? (ULONG)-1 : 0;
}

static ULONG mock_wait_cmd(ULONG mask, ULONG timeout, struct sdcard_Bus *bus)
{
    record_external_callback();
    (void)mask;
    (void)timeout;
    ++wait_calls;
    if (hooks_are_paired(bus))
        CHECK(owner_depth > 0);
    else
        CHECK(owner_depth == 0);
    record_event(EVENT_WAIT, wait_calls);
    return wait_calls == fail_wait_call ? (ULONG)-1 : 0;
}

static void mock_reschedule(void)
{
    record_external_callback();
    CHECK(owner_depth == 0);
    ++yield_calls;
    record_event(EVENT_YIELD, 0);
}

static BOOL sdcard_StartUnitLocked(struct sdcard_Unit *unit)
{
    CHECK(owner_depth == 1);
    CHECK(unit_lock_depth == 0U);
    CHECK(unit->sdcu_Bus != NULL);
    if (expected_start_source != NULL)
    {
        CHECK(unit != expected_start_source);
        CHECK(unit->sdcu_Flags == (expected_start_source->sdcu_Flags &
                                   ~(AF_Card_Active | AF_Card_Configuring)));
        CHECK(unit->sdcu_Bus == expected_start_source->sdcu_Bus);
        CHECK(unit->sdcu_UnitNum == expected_start_source->sdcu_UnitNum);
        CHECK(unit->sdcu_CardRCA == expected_start_source->sdcu_CardRCA);
        CHECK(unit->sdcu_CardPower == expected_start_source->sdcu_CardPower);
        CHECK(unit->sdcu_Capacity == expected_start_source->sdcu_Capacity);
        CHECK(unit->sdcu_Cylinders == expected_start_source->sdcu_Cylinders);
        CHECK(unit->sdcu_Heads == expected_start_source->sdcu_Heads);
        CHECK(unit->sdcu_Sectors == expected_start_source->sdcu_Sectors);
        CHECK(unit->sdcu_Read32 == expected_start_source->sdcu_Read32);
        CHECK(unit->sdcu_Read64 == expected_start_source->sdcu_Read64);
    }
    record_external_callback();
    ++start_body_calls;
    unit->sdcu_Flags = (unit->sdcu_Flags &
                        ~(AF_Card_4bitData | AF_Card_HighSpeed |
                          AF_Card_HighSpeed52)) | start_body_flags;
    if (start_body_rca != 0UL)
        unit->sdcu_CardRCA = start_body_rca;
    return start_body_result;
}

static BOOL sdcard_RegisterUnitLocked(struct sdcard_Bus *bus)
{
    CHECK(owner_depth == 1);
    CHECK(unit_lock_depth == 0U);
    CHECK(bus != NULL);
    record_external_callback();
    ++register_body_calls;
    return register_body_result;
}

static void SDCBUS_LockUnit(struct sdcard_Bus *bus)
{
    (void)bus;
    ++unit_lock_calls;
    if (unit_lock_depth != 0U)
        ++unit_lock_errors;
    ++unit_lock_depth;
}

static void SDCBUS_UnlockUnit(struct sdcard_Bus *bus)
{
    (void)bus;
    ++unit_unlock_calls;
    if (unit_lock_depth == 0U)
        ++unit_lock_errors;
    else
        --unit_lock_depth;
}

/* INSERT_EXTRACTED_SDCARD_FUNCTIONS */

typedef BYTE (*read32_fn)(struct sdcard_Unit *, ULONG, ULONG, APTR, ULONG *);
typedef BYTE (*read64_fn)(struct sdcard_Unit *, UQUAD, ULONG, APTR, ULONG *);

static void setup_read(struct sdcard_Bus *bus, struct sdcard_Unit *unit,
    BOOL with_hooks)
{
    reset_bus(bus, with_hooks);
    unit->sdcu_Bus = bus;
    unit->sdcu_Flags = AF_Card_HighCapacity;
    unit->sdcu_UnitNum = 0;
    if (with_hooks)
        install_hooks(bus);
}

static void test_hook_helpers(void)
{
    struct sdcard_Bus bus;

    setup_read(&bus, &(struct sdcard_Unit){0}, FALSE);
    CHECK(SDCBUS_BeginTransaction(&bus, TRUE));
    SDCBUS_EndTransaction(&bus);
    CHECK(begin_calls == 0 && end_calls == 0);

    bus.sdcb_BeginTransaction = mock_begin;
    CHECK(!SDCBUS_BeginTransaction(&bus, TRUE));
    SDCBUS_EndTransaction(&bus);
    CHECK(begin_calls == 0 && end_calls == 0);

    bus.sdcb_BeginTransaction = NULL;
    bus.sdcb_EndTransaction = mock_end;
    CHECK(!SDCBUS_BeginTransaction(&bus, TRUE));
    SDCBUS_EndTransaction(&bus);
    CHECK(begin_calls == 0 && end_calls == 0);
}

static void test_single_success(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[512];
    ULONG actual;
    BYTE result;

    setup_read(&bus, &unit, TRUE);
    result = read32(&unit, 7, 1, buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 1 && end_calls == 1 && owner_depth == 0);
    CHECK(begin_wait_modes[0]);
    CHECK(send_calls == 1 && wait_calls == 1);
    CHECK(sent_commands[0] == MMC_CMD_READ_SINGLE_BLOCK);

    setup_read(&bus, &unit, TRUE);
    result = read64(&unit, UINT64_C(0x123456789), 1, buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 1 && end_calls == 1 && owner_depth == 0);
    CHECK(begin_wait_modes[0]);
    CHECK(send_calls == 1 && wait_calls == 1);
    CHECK(sent_commands[0] == MMC_CMD_READ_SINGLE_BLOCK);
}

static void check_multichunk_events(void)
{
    static const int kinds[] = {
        EVENT_BEGIN, EVENT_SEND, EVENT_WAIT, EVENT_SEND, EVENT_WAIT,
        EVENT_END, EVENT_YIELD, EVENT_BEGIN, EVENT_SEND, EVENT_WAIT,
        EVENT_END
    };
    static const IPTR values[] = {
        1, MMC_CMD_READ_MULTIPLE_BLOCK, 1, MMC_CMD_STOP_TRANSMISSION, 2,
        0, 0, 1, MMC_CMD_READ_SINGLE_BLOCK, 3, 0
    };
    unsigned int index;

    CHECK(event_count == sizeof(kinds) / sizeof(kinds[0]));
    for (index = 0; index < event_count; ++index)
    {
        CHECK(event_kind[index] == kinds[index]);
        CHECK(event_value[index] == values[index]);
    }
}

static void test_multichunk(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[(SD_CHUNK_BLOCKS + 1) * 512];
    ULONG actual;
    BYTE result;

    setup_read(&bus, &unit, TRUE);
    result = read32(&unit, 100, SD_CHUNK_BLOCKS + 1, buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 2 && end_calls == 2 && owner_depth == 0);
    CHECK(send_calls == 3 && wait_calls == 3 && yield_calls == 1);
    CHECK(begin_wait_modes[0] && begin_wait_modes[1]);
    check_multichunk_events();

    setup_read(&bus, &unit, TRUE);
    result = read64(&unit, UINT64_C(0x100000064), SD_CHUNK_BLOCKS + 1,
        buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 2 && end_calls == 2 && owner_depth == 0);
    CHECK(send_calls == 3 && wait_calls == 3 && yield_calls == 1);
    CHECK(begin_wait_modes[0] && begin_wait_modes[1]);
    check_multichunk_events();
}

static void test_begin_refusal(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[512];
    ULONG actual = 99;
    BYTE result;

    setup_read(&bus, &unit, TRUE);
    begin_allowed = 0;
    result = read32(&unit, 1, 1, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 0 && send_calls == 0 && wait_calls == 0);
    CHECK(begin_wait_modes[0]);

    setup_read(&bus, &unit, TRUE);
    begin_allowed = 0;
    actual = 99;
    result = read64(&unit, 1, 1, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 0 && send_calls == 0 && wait_calls == 0);
    CHECK(begin_wait_modes[0]);
}

static void test_failures(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[2 * 512];
    ULONG actual;
    BYTE result;

    setup_read(&bus, &unit, TRUE);
    fail_send_call = 1;
    result = read32(&unit, 1, 1, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 1 && send_calls == 1 && wait_calls == 0);

    setup_read(&bus, &unit, TRUE);
    fail_wait_call = 1;
    result = read64(&unit, 1, 2, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 1 && send_calls == 2 && wait_calls == 2);
    CHECK(sent_commands[0] == MMC_CMD_READ_MULTIPLE_BLOCK);
    CHECK(sent_commands[1] == MMC_CMD_STOP_TRANSMISSION);

    setup_read(&bus, &unit, TRUE);
    fail_send_call = 2;
    result = read64(&unit, 1, 2, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 1 && send_calls == 2 && wait_calls == 1);
    CHECK(sent_commands[1] == MMC_CMD_STOP_TRANSMISSION);

    setup_read(&bus, &unit, TRUE);
    fail_wait_call = 2;
    result = read32(&unit, 1, 2, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == 0);
    CHECK(begin_calls == 1 && end_calls == 1 && send_calls == 2 && wait_calls == 2);
    CHECK(sent_commands[1] == MMC_CMD_STOP_TRANSMISSION);
}

static void test_partial_accounting(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[(SD_CHUNK_BLOCKS + 1) * 512];
    ULONG actual;
    BYTE result;

    setup_read(&bus, &unit, TRUE);
    fail_send_call = 3;
    result = read32(&unit, 1, SD_CHUNK_BLOCKS + 1, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == SD_CHUNK_BLOCKS * 512);
    CHECK(begin_calls == 2 && end_calls == 2 && yield_calls == 1);
    CHECK(owner_depth == 0 && send_calls == 3);

    setup_read(&bus, &unit, TRUE);
    fail_send_call = 3;
    result = read64(&unit, 1, SD_CHUNK_BLOCKS + 1, buffer, &actual);
    CHECK(result == IOERR_ABORTED && actual == SD_CHUNK_BLOCKS * 512);
    CHECK(begin_calls == 2 && end_calls == 2 && yield_calls == 1);
    CHECK(owner_depth == 0 && send_calls == 3);
}

static void test_no_hooks(read32_fn read32, read64_fn read64)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    UBYTE buffer[(SD_CHUNK_BLOCKS + 1) * 512];
    ULONG actual;
    BYTE result;

    setup_read(&bus, &unit, FALSE);
    result = read32(&unit, 1, 1, buffer, &actual);
    CHECK(result == 0 && actual == 512);
    CHECK(begin_calls == 0 && end_calls == 0 && owner_depth == 0);
    CHECK(send_calls == 1 && wait_calls == 1);

    setup_read(&bus, &unit, FALSE);
    result = read64(&unit, 1, 1, buffer, &actual);
    CHECK(result == 0 && actual == 512);
    CHECK(begin_calls == 0 && end_calls == 0 && owner_depth == 0);
    CHECK(send_calls == 1 && wait_calls == 1);

    setup_read(&bus, &unit, FALSE);
    result = read32(&unit, 1, SD_CHUNK_BLOCKS + 1, buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 0 && end_calls == 0 && owner_depth == 0);
    CHECK(send_calls == 3 && wait_calls == 3 && yield_calls == 1);

    setup_read(&bus, &unit, FALSE);
    result = read64(&unit, 1, SD_CHUNK_BLOCKS + 1, buffer, &actual);
    CHECK(result == 0 && actual == sizeof(buffer));
    CHECK(begin_calls == 0 && end_calls == 0 && owner_depth == 0);
    CHECK(send_calls == 3 && wait_calls == 3 && yield_calls == 1);
}

static void check_unit_guard_and_transaction(void)
{
    CHECK(owner_depth == 0);
    CHECK(unit_lock_depth == 0U);
    CHECK(unit_lock_calls == unit_unlock_calls);
    CHECK(unit_lock_errors == 0U);
    CHECK(callbacks_under_unit_lock == 0U);
}

static void test_setup_wrappers(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;

    setup_read(&bus, &unit, TRUE);
    unit.sdcu_Bus = &bus;
    start_body_calls = 0;
    start_body_result = TRUE;
    expected_start_source = &unit;
    CHECK(test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 1 && begin_calls == 1 && end_calls == 1);
    CHECK(!begin_wait_modes[0]);
    CHECK(owner_depth == 0);
    CHECK(unit_lock_calls == 2 && unit_unlock_calls == 2);
    CHECK(unit.sdcu_Flags & AF_Card_Active);
    CHECK(!(unit.sdcu_Flags & AF_Card_Configuring));
    check_unit_guard_and_transaction();
    unit.sdcu_Flags &= ~AF_Card_Active;

    reset_mocks();
    start_body_result = FALSE;
    expected_start_source = &unit;
    CHECK(!test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 2 && begin_calls == 1 && end_calls == 1);
    CHECK(!begin_wait_modes[0]);
    CHECK(owner_depth == 0);
    CHECK(unit_lock_calls == 2 && unit_unlock_calls == 2);
    CHECK(!(unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring)));
    check_unit_guard_and_transaction();

    reset_mocks();
    begin_allowed = 0;
    CHECK(!test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 2 && begin_calls == 1 && end_calls == 0);
    CHECK(unit_lock_calls == 0 && unit_unlock_calls == 0);
    check_unit_guard_and_transaction();

    setup_read(&bus, &unit, TRUE);
    register_body_calls = 0;
    register_body_result = TRUE;
    CHECK(test_bus_RegisterUnit(&bus));
    CHECK(register_body_calls == 1 && begin_calls == 1 && end_calls == 1);
    CHECK(begin_wait_modes[0]);
    CHECK(owner_depth == 0);
    check_unit_guard_and_transaction();

    reset_mocks();
    register_body_result = FALSE;
    CHECK(!test_bus_RegisterUnit(&bus));
    CHECK(register_body_calls == 2 && begin_calls == 1 && end_calls == 1);
    CHECK(begin_wait_modes[0]);
    CHECK(owner_depth == 0);
    check_unit_guard_and_transaction();

    reset_mocks();
    begin_allowed = 0;
    CHECK(!test_bus_RegisterUnit(&bus));
    CHECK(register_body_calls == 2 && begin_calls == 1 && end_calls == 0);
    check_unit_guard_and_transaction();
}

static void test_start_unit_staging_and_publication(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    ULONG original_flags;
    ULONG original_rca;
    ULONG original_change_num;

    setup_read(&bus, &unit, TRUE);
    unit.sdcu_Flags = AF_Card_Active | AF_Card_HighCapacity;
    unit.sdcu_CardRCA = 0x1234UL;
    start_body_calls = 0;
    CHECK(test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 0);
    CHECK(begin_calls == 1 && end_calls == 1);
    CHECK(unit_lock_calls == 1 && unit_unlock_calls == 1);
    CHECK(unit.sdcu_Flags == (AF_Card_Active | AF_Card_HighCapacity));
    check_unit_guard_and_transaction();

    setup_read(&bus, &unit, TRUE);
    unit.sdcu_Flags = AF_Card_Configuring | AF_Card_HighCapacity;
    start_body_calls = 0;
    CHECK(!test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 0);
    CHECK(begin_calls == 1 && end_calls == 1);
    CHECK(unit_lock_calls == 1 && unit_unlock_calls == 1);
    CHECK(unit.sdcu_Flags == (AF_Card_Configuring | AF_Card_HighCapacity));
    check_unit_guard_and_transaction();

    setup_read(&bus, &unit, TRUE);
    unit.sdcu_Flags = AF_Card_HighCapacity | AF_Card_HighSpeed52 |
                      AF_Card_4bitData;
    unit.sdcu_CardRCA = 0x2345UL;
    unit.sdcu_CardPower = 0x3456UL;
    unit.sdcu_Capacity = UINT64_C(0x123456789ABCDEF0);
    unit.sdcu_Cylinders = 0x1234UL;
    unit.sdcu_Heads = 9U;
    unit.sdcu_Sectors = 17U;
    unit.sdcu_Read32 = test_ReadSector32;
    unit.sdcu_Read64 = test_ReadSector64;
    unit.sdcu_ChangeNum = 0x4567UL;
    original_change_num = unit.sdcu_ChangeNum;
    start_body_calls = 0;
    start_body_result = TRUE;
    start_body_flags = AF_Card_HighSpeed;
    start_body_rca = 0x5678UL;
    expected_start_source = &unit;
    CHECK(test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 1);
    CHECK((unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring |
                               AF_Card_HighCapacity | AF_Card_HighSpeed |
                               AF_Card_HighSpeed52 | AF_Card_4bitData)) ==
          (AF_Card_Active | AF_Card_HighCapacity | AF_Card_HighSpeed));
    CHECK(unit.sdcu_CardRCA == 0x5678UL);
    CHECK(unit.sdcu_CardPower == 0x3456UL);
    CHECK(unit.sdcu_Capacity == UINT64_C(0x123456789ABCDEF0));
    CHECK(unit.sdcu_Cylinders == 0x1234UL);
    CHECK(unit.sdcu_Heads == 9U && unit.sdcu_Sectors == 17U);
    CHECK(unit.sdcu_Read32 == test_ReadSector32);
    CHECK(unit.sdcu_Read64 == test_ReadSector64);
    CHECK(unit.sdcu_ChangeNum == original_change_num);
    CHECK(unit_lock_calls == 2 && unit_unlock_calls == 2);
    CHECK(begin_calls == 1 && end_calls == 1);
    check_unit_guard_and_transaction();

    setup_read(&bus, &unit, TRUE);
    unit.sdcu_Flags = AF_Card_HighCapacity | AF_Card_HighSpeed52 |
                      AF_Card_4bitData;
    unit.sdcu_CardRCA = 0x6789UL;
    unit.sdcu_ChangeNum = 0x89ABUL;
    original_flags = unit.sdcu_Flags;
    original_rca = unit.sdcu_CardRCA;
    original_change_num = unit.sdcu_ChangeNum;
    start_body_calls = 0;
    start_body_result = FALSE;
    start_body_flags = AF_Card_HighSpeed;
    start_body_rca = 0x9876UL;
    expected_start_source = &unit;
    CHECK(!test_bus_StartUnit(&unit));
    CHECK(start_body_calls == 1);
    CHECK((unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring)) == 0UL);
    CHECK((unit.sdcu_Flags & (AF_Card_HighCapacity | AF_Card_HighSpeed |
                               AF_Card_HighSpeed52 | AF_Card_4bitData)) ==
          original_flags);
    CHECK(unit.sdcu_CardRCA == original_rca);
    CHECK(unit.sdcu_ChangeNum == original_change_num);
    CHECK(unit_lock_calls == 2 && unit_unlock_calls == 2);
    CHECK(begin_calls == 1 && end_calls == 1);
    check_unit_guard_and_transaction();
}

static void test_open_metadata_guard_shortcuts(void)
{
    struct SDCardBase base;
    struct sdcard_Bus bus;
    struct sdcard_BusUnits bus_units;
    struct sdcard_Unit unit;
    struct IORequest request;

    memset(&base, 0, sizeof(base));
    memset(&bus, 0, sizeof(bus));
    memset(&bus_units, 0, sizeof(bus_units));
    memset(&unit, 0, sizeof(unit));
    memset(&request, 0, sizeof(request));
    setup_read(&bus, &unit, TRUE);
    bus.sdcb_BusUnits = &bus_units;
    bus_units.sdcbu_UnitCnt = 1;
    bus_units.sdcbu_Units = &unit;
    base.sdcard_Buses.first = &bus;
    unit.sdcu_Flags = AF_Card_Active | AF_Card_HighCapacity;
    start_body_calls = 0;
    CHECK(test_device_Open(&base, &request, 0, 0));
    CHECK(unit.sdcu_Unit.unit_OpenCnt == 1);
    CHECK(request.io_Unit == (struct Unit *)&unit && request.io_Error == 0);
    CHECK(start_body_calls == 0 && begin_calls == 0 && end_calls == 0);
    CHECK(unit_lock_calls == 1 && unit_unlock_calls == 1);
    check_unit_guard_and_transaction();

    memset(&base, 0, sizeof(base));
    memset(&bus, 0, sizeof(bus));
    memset(&bus_units, 0, sizeof(bus_units));
    memset(&unit, 0, sizeof(unit));
    memset(&request, 0, sizeof(request));
    setup_read(&bus, &unit, TRUE);
    bus.sdcb_BusUnits = &bus_units;
    bus_units.sdcbu_UnitCnt = 1;
    bus_units.sdcbu_Units = &unit;
    base.sdcard_Buses.first = &bus;
    unit.sdcu_Flags = AF_Card_Configuring | AF_Card_HighCapacity;
    start_body_calls = 0;
    CHECK(!test_device_Open(&base, &request, 0, 0));
    CHECK(unit.sdcu_Unit.unit_OpenCnt == 0);
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    CHECK(start_body_calls == 0 && begin_calls == 0 && end_calls == 0);
    CHECK(unit_lock_calls == 1 && unit_unlock_calls == 1);
    check_unit_guard_and_transaction();
}

static void test_open_start_failure(void)
{
    struct SDCardBase base;
    struct sdcard_Bus bus;
    struct sdcard_BusUnits bus_units;
    struct sdcard_Unit unit;
    struct IORequest request;

    memset(&base, 0, sizeof(base));
    memset(&bus, 0, sizeof(bus));
    memset(&bus_units, 0, sizeof(bus_units));
    memset(&unit, 0, sizeof(unit));
    memset(&request, 0, sizeof(request));
    setup_read(&bus, &unit, TRUE);
    bus.next = NULL;
    bus.sdcb_BusUnits = &bus_units;
    bus_units.sdcbu_UnitBase = 0;
    bus_units.sdcbu_UnitCnt = 1;
    bus_units.sdcbu_Units = &unit;
    unit.sdcu_Bus = &bus;
    base.sdcard_Buses.first = &bus;
    start_body_calls = 0;
    start_body_result = FALSE;
    expected_start_source = &unit;

    CHECK(!test_device_Open(&base, &request, 0, 0));
    CHECK(unit.sdcu_Unit.unit_OpenCnt == 0);
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    CHECK(start_body_calls == 1 && begin_calls == 1 && end_calls == 1);
    CHECK(!begin_wait_modes[0]);
    CHECK(unit_lock_calls == 4 && unit_unlock_calls == 4);
    CHECK(!(unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring)));
    check_unit_guard_and_transaction();

    reset_mocks();
    start_body_calls = 0;
    begin_allowed = 0;
    CHECK(!test_device_Open(&base, &request, 0, 0));
    CHECK(unit.sdcu_Unit.unit_OpenCnt == 0);
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    CHECK(start_body_calls == 0 && begin_calls == 1 && end_calls == 0);
    CHECK(!begin_wait_modes[0]);
    CHECK(unit_lock_calls == 2 && unit_unlock_calls == 2);
    check_unit_guard_and_transaction();
}

int main(void)
{
    test_hook_helpers();
    test_single_success(test_ReadSector32, test_ReadSector64);
    test_multichunk(test_ReadSector32, test_ReadSector64);
    test_begin_refusal(test_ReadSector32, test_ReadSector64);
    test_failures(test_ReadSector32, test_ReadSector64);
    test_partial_accounting(test_ReadSector32, test_ReadSector64);
    test_no_hooks(test_ReadSector32, test_ReadSector64);
    test_setup_wrappers();
    test_start_unit_staging_and_publication();
    test_open_metadata_guard_shortcuts();
    test_open_start_failure();
    printf("PASS: %u SD transaction checks\n", checks);
    return 0;
}
