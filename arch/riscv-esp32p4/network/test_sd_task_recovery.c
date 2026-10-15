/*
 * Host model for the production SD BusTask recovery/IO dispatch wrappers.
 * The extracted wrappers are production source. RecoverUnit and HandleIO are
 * deliberately scripted models: this fixture does not exercise SD hardware,
 * card identification, DMA, or the full recovery algorithm.
 */
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

struct IORequest
{
    UWORD io_Command;
    BYTE io_Error;
    void *io_Unit;
    void *io_Data;
};

struct IOStdReq
{
    struct IORequest io;
    ULONG io_Actual;
};

struct Node { ULONG unused; };
struct Unit { ULONG unused; };
struct List { ULONG unused; };
struct TagItem;
struct Task;
struct MsgPort;
struct Interrupt;
struct SDCardBase;

/* INSERT_ACTUAL_SD_COMMANDS */
/* INSERT_ACTUAL_SD_FUNCTION_MACROS */
/* INSERT_ACTUAL_SD_TYPES */

static void SDCBUS_LockUnit(struct sdcard_Bus *bus);
static void SDCBUS_UnlockUnit(struct sdcard_Bus *bus);
static BOOL SDCARD__SDBus__RecoverUnit(struct sdcard_Unit *unit);
static BOOL SDCARD__Device__HandleIO(struct IORequest *io);

/* INSERT_ACTUAL_SD_FUNCTIONS */

#define EVENT_CAPACITY 32U
#define IO_CAPACITY 32U
#define SLOT_CAPACITY 4U

struct bus_unit_table
{
    struct sdcard_BusUnits header;
    struct sdcard_Unit *tail[SLOT_CAPACITY - 1U];
};

struct test_model
{
    struct sdcard_Bus bus;
    struct bus_unit_table unit_table;
    struct sdcard_Unit unit_a;
    struct sdcard_Unit unit_b;
    struct sdcard_Unit unit_c;

    unsigned int checks;
    unsigned int cases;
    unsigned int spin_depth;
    unsigned int unit_lock_calls;
    unsigned int unit_unlock_calls;
    unsigned int owner_depth;
    unsigned int max_owner_depth;
    unsigned int recover_calls;
    unsigned int handle_calls;
    unsigned int begin_calls;
    unsigned int end_calls;
    unsigned int needed_calls;
    unsigned int prepare_calls;
    unsigned int finish_calls;
    unsigned int identify_activate_calls;
    unsigned int event_count;
    unsigned int io_count;
    int events[EVENT_CAPACITY];
    UWORD io_commands[IO_CAPACITY];
    struct sdcard_Unit *recovered_units[IO_CAPACITY];
    struct sdcard_Unit *current_unit;
    BOOL begin_ok;
    BOOL needed;
    BOOL prepare_ok;
    BOOL identify_activate_ok;
    BOOL finish_ok;
    BOOL handle_result;
    BOOL handle_writes_result;
    BYTE handle_error;
    ULONG handle_actual;
    BOOL handle_updates_result;
    BOOL handle_writes_data;
    BOOL last_finish_argument;
};

static struct test_model model;

enum event_kind
{
    EVENT_BEGIN = 1,
    EVENT_NEEDED,
    EVENT_PREPARE,
    EVENT_IDENTIFY_ACTIVATE,
    EVENT_FINISH,
    EVENT_END,
    EVENT_HANDLE,
    EVENT_UNIT_LOCK,
    EVENT_UNIT_UNLOCK
};

static void fail_check(const char *expression, const char *file, int line)
{
    (void)fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}

#define CHECK(expression)                                                     \
    do                                                                        \
    {                                                                         \
        ++model.checks;                                                       \
        if (!(expression))                                                    \
            fail_check(#expression, __FILE__, __LINE__);                     \
    } while (0)

static void record_event(int event)
{
    CHECK(model.event_count < EVENT_CAPACITY);
    model.events[model.event_count++] = event;
}

static void reset_model(void)
{
    unsigned int checks = model.checks;
    unsigned int cases = model.cases;
    memset(&model, 0, sizeof(model));
    model.checks = checks;
    model.cases = cases;
    model.begin_ok = TRUE;
    model.needed = TRUE;
    model.prepare_ok = TRUE;
    model.identify_activate_ok = TRUE;
    model.finish_ok = TRUE;
    model.handle_result = TRUE;
    model.handle_writes_result = TRUE;
    model.handle_error = 0;
    model.handle_actual = 0;
    model.handle_updates_result = TRUE;
    model.bus.sdcb_BusUnits = &model.unit_table.header;
    model.unit_table.header.sdcbu_UnitBase = 0;
    model.unit_table.header.sdcbu_UnitMax = SLOT_CAPACITY;
    model.unit_table.header.sdcbu_UnitCnt = 0;
    model.unit_a.sdcu_Bus = &model.bus;
    model.unit_b.sdcu_Bus = &model.bus;
    model.unit_c.sdcu_Bus = &model.bus;
    model.unit_a.sdcu_UnitNum = 0;
    model.unit_b.sdcu_UnitNum = 1;
    model.unit_c.sdcu_UnitNum = 2;
}

static void begin_case(void)
{
    ++model.cases;
}

static void check_callback_context(void)
{
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 1U);
}

static void SDCBUS_LockUnit(struct sdcard_Bus *bus)
{
    CHECK(bus == &model.bus);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 0U);
    ++model.spin_depth;
    ++model.unit_lock_calls;
    record_event(EVENT_UNIT_LOCK);
}

static void SDCBUS_UnlockUnit(struct sdcard_Bus *bus)
{
    CHECK(bus == &model.bus);
    CHECK(model.spin_depth == 1U);
    --model.spin_depth;
    ++model.unit_unlock_calls;
    record_event(EVENT_UNIT_UNLOCK);
}

static BOOL mock_begin(struct sdcard_Bus *bus, BOOL may_wait)
{
    CHECK(bus == &model.bus);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 0U);
    CHECK(may_wait == TRUE);
    ++model.begin_calls;
    record_event(EVENT_BEGIN);
    if (model.begin_ok)
    {
        ++model.owner_depth;
        if (model.owner_depth > model.max_owner_depth)
            model.max_owner_depth = model.owner_depth;
    }
    return model.begin_ok;
}

static void mock_end(struct sdcard_Bus *bus)
{
    CHECK(bus == &model.bus);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 1U);
    --model.owner_depth;
    ++model.end_calls;
    record_event(EVENT_END);
}

static BOOL mock_needed(struct sdcard_Bus *bus)
{
    CHECK(bus == &model.bus);
    check_callback_context();
    ++model.needed_calls;
    record_event(EVENT_NEEDED);
    return model.needed;
}

static BOOL mock_prepare(struct sdcard_Bus *bus)
{
    CHECK(bus == &model.bus);
    check_callback_context();
    ++model.prepare_calls;
    record_event(EVENT_PREPARE);
    return model.prepare_ok;
}

static BOOL mock_finish(struct sdcard_Bus *bus, BOOL result)
{
    CHECK(bus == &model.bus);
    check_callback_context();
    ++model.finish_calls;
    model.last_finish_argument = result;
    record_event(EVENT_FINISH);
    return model.finish_ok;
}

static void install_recovery_callbacks(void)
{
    model.bus.sdcb_BeginTransaction = mock_begin;
    model.bus.sdcb_EndTransaction = mock_end;
    model.bus.sdcb_RecoveryNeeded = mock_needed;
    model.bus.sdcb_RecoveryPrepare = mock_prepare;
    model.bus.sdcb_RecoveryFinish = mock_finish;
}

static BOOL SDCARD__SDBus__RecoverUnit(struct sdcard_Unit *unit)
{
    struct sdcard_Bus *bus;
    BOOL result;

    CHECK(unit != NULL);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 0U);
    CHECK(unit->sdcu_RecoveryBlocked == TRUE);
    CHECK(model.recover_calls < IO_CAPACITY);
    model.recovered_units[model.recover_calls++] = unit;
    model.current_unit = unit;
    bus = unit->sdcu_Bus;

    /* Mirrors the production RecoverUnit hook-presence/acquisition refusal. */
    if (!bus->sdcb_RecoveryNeeded || !bus->sdcb_RecoveryPrepare ||
        !bus->sdcb_RecoveryFinish || !bus->sdcb_BeginTransaction ||
        !bus->sdcb_EndTransaction)
        return FALSE;
    if (!bus->sdcb_BeginTransaction(bus, TRUE))
        return FALSE;

    if (!bus->sdcb_RecoveryNeeded(bus))
    {
        result = TRUE;
    }
    else
    {
        result = bus->sdcb_RecoveryPrepare(bus);
        if (result)
        {
            ++model.identify_activate_calls;
            record_event(EVENT_IDENTIFY_ACTIVATE);
            result = model.identify_activate_ok;
        }
        result = bus->sdcb_RecoveryFinish(bus, result) && result;
    }
    bus->sdcb_EndTransaction(bus);
    model.current_unit = NULL;
    return result;
}

static BOOL SDCARD__Device__HandleIO(struct IORequest *io)
{
    BOOL result;
    CHECK(io != NULL);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 0U);
    CHECK(model.handle_calls < IO_CAPACITY);
    model.io_commands[model.handle_calls] = io->io_Command;
    ++model.handle_calls;
    record_event(EVENT_HANDLE);
    if (model.handle_updates_result)
    {
        io->io_Error = model.handle_error;
        ((struct IOStdReq *)io)->io_Actual = model.handle_actual;
    }
    if (model.handle_writes_data && io->io_Data != NULL)
        memset(io->io_Data, 0xA5, 8U);
    result = model.handle_result;
    if (io->io_Command == TD_ADDCHANGEINT)
        result = model.handle_writes_result;
    return result;
}

static struct IOStdReq make_io(struct sdcard_Unit *unit, UWORD command,
                               BYTE error, ULONG actual)
{
    struct IOStdReq io;
    memset(&io, 0, sizeof(io));
    io.io.io_Command = command;
    io.io.io_Error = error;
    io.io.io_Unit = unit;
    io.io_Actual = actual;
    return io;
}

static void check_single_io_dispatch(UWORD command)
{
    struct IOStdReq io;
    reset_model();
    install_recovery_callbacks();
    io = make_io(&model.unit_a, command, 7, 11);
    CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
    CHECK(model.recover_calls == 1U);
    CHECK(model.recovered_units[0] == &model.unit_a);
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.needed_calls == 1U && model.prepare_calls == 1U);
    CHECK(model.identify_activate_calls == 1U && model.finish_calls == 1U);
    CHECK(model.handle_calls == 1U);
    CHECK(model.io_commands[0] == command);
    CHECK(model.owner_depth == 0U && model.max_owner_depth == 1U);
    CHECK(model.spin_depth == 0U);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == FALSE);
    CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);
    CHECK(io.io.io_Error == 0 && io.io_Actual == 0U);
}

static void test_legacy_no_hooks_bypass(void)
{
    struct IOStdReq io;
    begin_case();
    reset_model();
    model.bus.sdcb_BeginTransaction = mock_begin;
    model.bus.sdcb_EndTransaction = mock_end;
    model.unit_a.sdcu_RecoveryBlocked = TRUE;
    io = make_io(&model.unit_a, CMD_READ, 12, 99);
    CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
    CHECK(model.recover_calls == 0U);
    CHECK(model.handle_calls == 1U);
    CHECK(io.io.io_Error == 0 && io.io_Actual == 0U);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
    CHECK(sdcard_TaskRecoverUnit(&model.unit_a, TRUE) == TRUE);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
    CHECK(model.unit_lock_calls == 0U && model.unit_unlock_calls == 0U);
}

static void test_partial_callbacks_fail_closed(void)
{
    unsigned int mask;
    begin_case();
    for (mask = 1U; mask < 7U; ++mask)
    {
        struct IOStdReq io;
        reset_model();
        if ((mask & 1U) != 0U)
            model.bus.sdcb_RecoveryNeeded = mock_needed;
        if ((mask & 2U) != 0U)
            model.bus.sdcb_RecoveryPrepare = mock_prepare;
        if ((mask & 4U) != 0U)
            model.bus.sdcb_RecoveryFinish = mock_finish;
        io = make_io(&model.unit_a, CMD_READ, (BYTE)mask, 88U + mask);
        CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
        CHECK(model.recover_calls == 1U);
        CHECK(model.begin_calls == 0U);
        CHECK(model.handle_calls == 0U);
        CHECK(io.io.io_Error == TDERR_DiskChanged && io.io_Actual == 0U);
        CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
        CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);
    }

    {
        struct IOStdReq io;
        reset_model();
        model.bus.sdcb_BeginTransaction = mock_begin;
        model.bus.sdcb_EndTransaction = mock_end;
        model.bus.sdcb_RecoveryNeeded = mock_needed;
        model.bus.sdcb_RecoveryPrepare = mock_prepare;
        model.bus.sdcb_RecoveryFinish = mock_finish;
        model.bus.sdcb_EndTransaction = NULL;
        io = make_io(&model.unit_a, CMD_READ, 6, 90);
        CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
        CHECK(model.recover_calls == 1U);
        CHECK(model.begin_calls == 0U);
        CHECK(model.handle_calls == 0U);
        CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
        CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);
    }
}

static void test_read_commands_gate(void)
{
    static const UWORD commands[] = {
        CMD_READ,
        TD_READ64,
        NSCMD_TD_READ64
    };
    unsigned int index;
    begin_case();
    for (index = 0; index < (unsigned int)(sizeof(commands) / sizeof(commands[0]));
         ++index)
        check_single_io_dispatch(commands[index]);
}

static void test_healthy_no_recovery_needed_still_dispatches_once(void)
{
    struct IOStdReq io;
    begin_case();
    reset_model();
    install_recovery_callbacks();
    model.needed = FALSE;
    model.handle_error = 17;
    model.handle_actual = 512U;
    io = make_io(&model.unit_a, CMD_READ, 0, 0U);
    CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
    CHECK(model.recover_calls == 1U);
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.needed_calls == 1U);
    CHECK(model.prepare_calls == 0U && model.finish_calls == 0U);
    CHECK(model.identify_activate_calls == 0U);
    CHECK(model.handle_calls == 1U && model.io_commands[0] == CMD_READ);
    CHECK(io.io.io_Error == 17 && io.io_Actual == 512U);
    CHECK(model.owner_depth == 0U && model.spin_depth == 0U);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == FALSE);
    CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);
}

static void test_failed_recovery_latches_and_gates(void)
{
    struct IOStdReq first;
    struct IOStdReq second;
    begin_case();
    reset_model();
    install_recovery_callbacks();
    model.identify_activate_ok = FALSE;
    first = make_io(&model.unit_a, CMD_READ, 44, 900);
    CHECK(sdcard_TaskHandleIO(&first.io) == TRUE);
    CHECK(first.io.io_Error == TDERR_DiskChanged && first.io_Actual == 0U);
    CHECK(model.recover_calls == 1U && model.handle_calls == 0U);
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.finish_calls == 1U && model.last_finish_argument == FALSE);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
    CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);

    second = make_io(&model.unit_a, NSCMD_TD_READ64, 33, 777);
    CHECK(sdcard_TaskHandleIO(&second.io) == TRUE);
    CHECK(second.io.io_Error == TDERR_DiskChanged && second.io_Actual == 0U);
    CHECK(model.recover_calls == 1U && model.handle_calls == 0U);
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);

    model.unit_table.header.sdcbu_UnitCnt = 1U;
    model.unit_table.header.sdcbu_Units = &model.unit_a;
    sdcard_TaskRecoverBus(&model.bus);
    CHECK(model.recover_calls == 2U);
    CHECK(model.begin_calls == 2U && model.end_calls == 2U);
    CHECK(model.unit_lock_calls == 5U && model.unit_unlock_calls == 5U);
    CHECK(model.finish_calls == 2U && model.last_finish_argument == FALSE);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);

    second = make_io(&model.unit_a, CMD_READ, 29, 660U);
    CHECK(sdcard_TaskHandleIO(&second.io) == TRUE);
    CHECK(second.io.io_Error == TDERR_DiskChanged && second.io_Actual == 0U);
    CHECK(model.recover_calls == 2U && model.handle_calls == 0U);
    CHECK(model.begin_calls == 2U && model.end_calls == 2U);
    CHECK(model.unit_lock_calls == 6U && model.unit_unlock_calls == 6U);
}

static void test_transaction_acquire_failure_latches_without_data_io(void)
{
    struct IOStdReq io;
    unsigned char payload[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    unsigned char expected[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    begin_case();
    reset_model();
    install_recovery_callbacks();
    model.begin_ok = FALSE;
    model.handle_writes_data = TRUE;
    io = make_io(&model.unit_a, CMD_READ, 31, 1024U);
    io.io.io_Data = payload;
    CHECK(sdcard_TaskHandleIO(&io.io) == TRUE);
    CHECK(model.recover_calls == 1U);
    CHECK(model.begin_calls == 1U && model.end_calls == 0U);
    CHECK(model.needed_calls == 0U && model.prepare_calls == 0U);
    CHECK(model.finish_calls == 0U && model.handle_calls == 0U);
    CHECK(io.io.io_Error == TDERR_DiskChanged && io.io_Actual == 0U);
    CHECK(memcmp(payload, expected, sizeof(payload)) == 0);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == TRUE);
    CHECK(model.owner_depth == 0U && model.spin_depth == 0U);
    CHECK(model.unit_lock_calls == 2U && model.unit_unlock_calls == 2U);
}

static void set_slot(struct bus_unit_table *table, unsigned int ordinal,
                     struct sdcard_Unit *unit)
{
    if (ordinal == 0U)
        table->header.sdcbu_Units = unit;
    else
        table->tail[ordinal - 1U] = unit;
}

static struct sdcard_Unit *get_slot(const struct bus_unit_table *table,
                                    unsigned int ordinal)
{
    if (ordinal == 0U)
        return table->header.sdcbu_Units;
    return table->tail[ordinal - 1U];
}

static void test_task_wake_rearms_multiunit_stably(void)
{
    struct sdcard_Unit *unit_pointer;
    struct sdcard_BusUnits *units_pointer;
    ULONG count;
    ULONG cid_a[4] = {0x10203040UL, 0x50607080UL, 0x90A0B0C0UL, 0xD0E0F000UL};
    ULONG csd_a[4] = {0x11112222UL, 0x33334444UL, 0x55556666UL, 0x77778888UL};
    ULONG cid_b[4] = {0xABCDEF01UL, 0x23456789UL, 0x13579BDFUL, 0x2468ACE0UL};
    ULONG csd_b[4] = {0x01020304UL, 0x05060708UL, 0x090A0B0CUL, 0x0D0E0F10UL};
    begin_case();
    reset_model();
    install_recovery_callbacks();

    CHECK(offsetof(struct sdcard_BusUnits, sdcbu_Units) +
              sizeof(model.unit_table.header.sdcbu_Units) ==
          sizeof(struct sdcard_BusUnits));
    model.unit_table.header.sdcbu_UnitCnt = 3U;
    set_slot(&model.unit_table, 0U, &model.unit_a);
    set_slot(&model.unit_table, 1U, NULL);
    set_slot(&model.unit_table, 2U, &model.unit_b);
    model.unit_a.sdcu_RecoveryBlocked = TRUE;
    model.unit_b.sdcu_RecoveryBlocked = TRUE;
    memcpy(model.unit_a.sdcu_CardCID, cid_a, sizeof(cid_a));
    memcpy(model.unit_a.sdcu_CardCSD, csd_a, sizeof(csd_a));
    memcpy(model.unit_b.sdcu_CardCID, cid_b, sizeof(cid_b));
    memcpy(model.unit_b.sdcu_CardCSD, csd_b, sizeof(csd_b));

    units_pointer = model.bus.sdcb_BusUnits;
    unit_pointer = model.unit_table.header.sdcbu_Units;
    count = model.unit_table.header.sdcbu_UnitCnt;
    sdcard_TaskRecoverBus(&model.bus);

    CHECK(model.recover_calls == 2U);
    CHECK(model.recovered_units[0] == &model.unit_a);
    CHECK(model.recovered_units[1] == &model.unit_b);
    CHECK(model.begin_calls == 2U && model.end_calls == 2U);
    CHECK(model.unit_a.sdcu_RecoveryBlocked == FALSE);
    CHECK(model.unit_b.sdcu_RecoveryBlocked == FALSE);
    CHECK(model.bus.sdcb_BusUnits == units_pointer);
    CHECK(model.unit_table.header.sdcbu_Units == unit_pointer);
    CHECK(model.unit_table.header.sdcbu_UnitCnt == count);
    CHECK(get_slot(&model.unit_table, 0U) == &model.unit_a);
    CHECK(get_slot(&model.unit_table, 1U) == NULL);
    CHECK(get_slot(&model.unit_table, 2U) == &model.unit_b);
    CHECK(memcmp(model.unit_a.sdcu_CardCID, cid_a, sizeof(cid_a)) == 0);
    CHECK(memcmp(model.unit_a.sdcu_CardCSD, csd_a, sizeof(csd_a)) == 0);
    CHECK(memcmp(model.unit_b.sdcu_CardCID, cid_b, sizeof(cid_b)) == 0);
    CHECK(memcmp(model.unit_b.sdcu_CardCSD, csd_b, sizeof(csd_b)) == 0);
    CHECK(model.owner_depth == 0U && model.spin_depth == 0U);
    CHECK(model.unit_lock_calls == 4U && model.unit_unlock_calls == 4U);
}

static void test_writes_control_and_change_listeners_dispatch_unchanged(void)
{
    static const UWORD commands[] = {
        CMD_WRITE,
        TD_WRITE64,
        NSCMD_TD_WRITE64,
        CMD_UPDATE,
        TD_CHANGESTATE,
        TD_PROTSTATUS,
        TD_ADDCHANGEINT,
        TD_REMCHANGEINT,
        HD_SCSICMD
    };
    unsigned int index;
    begin_case();
    for (index = 0; index < (unsigned int)(sizeof(commands) / sizeof(commands[0]));
         ++index)
    {
        struct IOStdReq io;
        reset_model();
        install_recovery_callbacks();
        model.unit_a.sdcu_RecoveryBlocked = TRUE;
        io = make_io(&model.unit_a, commands[index], 13, 23);
        if (commands[index] == HD_SCSICMD)
        {
            model.handle_error = (BYTE)IOERR_BADADDRESS;
            model.handle_actual = sizeof(struct SCSICmd);
        }
        if (commands[index] == TD_ADDCHANGEINT)
        {
            model.handle_writes_result = FALSE;
            model.handle_updates_result = FALSE;
        }
        CHECK(sdcard_TaskHandleIO(&io.io) ==
              (commands[index] == TD_ADDCHANGEINT ? FALSE : TRUE));
        CHECK(model.recover_calls == 0U);
        CHECK(model.begin_calls == 0U && model.handle_calls == 1U);
        CHECK(model.unit_lock_calls == 0U && model.unit_unlock_calls == 0U);
        CHECK(model.io_commands[0] == commands[index]);
        if (commands[index] == TD_ADDCHANGEINT)
            CHECK(io.io.io_Error == 13 && io.io_Actual == 23U);
        else if (commands[index] == HD_SCSICMD)
            CHECK(io.io.io_Error == (BYTE)IOERR_BADADDRESS &&
                  io.io_Actual == sizeof(struct SCSICmd));
        else
            CHECK(io.io.io_Error == 0 && io.io_Actual == 0U);
    }
}

static void test_failed_underlying_io_is_not_replayed(void)
{
    struct IOStdReq io;
    begin_case();
    reset_model();
    install_recovery_callbacks();
    model.handle_result = FALSE;
    model.handle_error = (BYTE)-23;
    model.handle_actual = 271U;
    io = make_io(&model.unit_a, CMD_READ, 1, 2);
    CHECK(sdcard_TaskHandleIO(&io.io) == FALSE);
    CHECK(model.recover_calls == 1U && model.handle_calls == 1U);
    CHECK(model.io_commands[0] == CMD_READ);
    CHECK(io.io.io_Error == (BYTE)-23 && io.io_Actual == 271U);
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
}

int main(void)
{
    test_legacy_no_hooks_bypass();
    test_partial_callbacks_fail_closed();
    test_read_commands_gate();
    test_healthy_no_recovery_needed_still_dispatches_once();
    test_failed_recovery_latches_and_gates();
    test_transaction_acquire_failure_latches_without_data_io();
    test_task_wake_rearms_multiunit_stably();
    test_writes_control_and_change_listeners_dispatch_unchanged();
    test_failed_underlying_io_is_not_replayed();
    (void)printf("PASS: %u cases, %u checks\n", model.cases, model.checks);
    return 0;
}
