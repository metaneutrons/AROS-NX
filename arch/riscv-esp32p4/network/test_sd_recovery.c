/* Host fixture for the generic SD same-media recovery and publication path.
 * tools/test-sd-recovery.py inserts production structs and function bodies. */
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

/* INSERT_ACTUAL_SD_CONSTANTS */

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
    ULONG unit_OpenCnt;
};

struct Interrupt;
struct SCSICmd;
struct TagItem;
struct sdcard_Bus;
struct SDCardBase;

/* INSERT_ACTUAL_SD_TYPES */

typedef ULONG spinlock_t;

struct sdcard_Bus
{
    struct sdcard_Bus *next;
    struct sdcard_BusUnits *sdcb_BusUnits;
    struct SDCardBase *sdcb_DeviceBase;
    ULONG sdcb_BusFlags;
    spinlock_t sdcb_UnitLock;
    struct TagItem *sdcb_RespListener;
    struct TagItem *sdcb_DataListener;
    BOOL (*sdcb_BeginTransaction)(struct sdcard_Bus *, BOOL may_wait);
    void (*sdcb_EndTransaction)(struct sdcard_Bus *);
    BOOL (*sdcb_RecoveryNeeded)(struct sdcard_Bus *);
    BOOL (*sdcb_RecoveryPrepare)(struct sdcard_Bus *);
    BOOL (*sdcb_RecoveryFinish)(struct sdcard_Bus *, BOOL);
};

struct sdcard_BusList
{
    struct sdcard_Bus *first;
};

struct SDCardBase
{
    struct sdcard_BusList sdcard_Buses;
};

struct IORequest
{
    BYTE io_Error;
    struct Unit *io_Unit;
};

typedef struct SDCardBase *LIBBASETYPEPTR;

/* INSERT_ACTUAL_SD_FUNCTION_MACROS */
#define ForeachNode(list, node) \
    for ((node) = (list)->first; (node) != NULL; (node) = (node)->next)
#define DDEV(...) ((void)0)

#if defined(__AROSEXEC_SMP__)
static void mock_spin_lock(spinlock_t *lock, void *success, ULONG mode,
                           struct SDCardBase *base);
static void mock_spin_unlock(spinlock_t *lock, struct SDCardBase *base);
#endif
static void mock_forbid(void);
static void mock_permit(void);
#define Forbid() mock_forbid()
#define Permit() mock_permit()
/* These macros reference the production helper's local SDCardBase mapping. */
#if defined(__AROSEXEC_SMP__)
#define KrnSpinLock(lock, success, mode) \
    mock_spin_lock((lock), (success), (mode), SDCardBase)
#define KrnSpinUnLock(lock) mock_spin_unlock((lock), SDCardBase)
#endif

static BOOL sdcard_StartUnitLocked(struct sdcard_Unit *candidate);
BOOL FNAME_SDCBUS(IdentifyCardLocked)(struct sdcard_Bus *,
                                     struct sdcard_CardInfo *);

enum event_kind
{
    EVENT_BEGIN = 1,
    EVENT_NEEDED,
    EVENT_PREPARE,
    EVENT_IDENTIFY,
    EVENT_ACTIVATE,
    EVENT_FINISH,
    EVENT_END
};

#define EVENT_CAPACITY 64U

struct test_model
{
    int events[EVENT_CAPACITY];
    unsigned int event_count;
    unsigned int checks;
    unsigned int cases;
    unsigned int forbid_depth;
    unsigned int spin_depth;
    unsigned int owner_depth;
    unsigned int forbid_calls;
    unsigned int permit_calls;
    unsigned int spin_lock_calls;
    unsigned int spin_unlock_calls;
    unsigned int begin_calls;
    unsigned int end_calls;
    unsigned int needed_calls;
    unsigned int prepare_calls;
    unsigned int identify_calls;
    unsigned int activate_calls;
    unsigned int finish_calls;
    BOOL acquire_ok;
    BOOL begin_may_wait;
    BOOL recovery_needed;
    BOOL prepare_ok;
    BOOL identify_ok;
    BOOL activation_ok;
    BOOL finish_ok;
    BOOL finish_argument;
    BOOL activation_candidate_is_distinct;
    ULONG activation_flags;
    ULONG candidate_rca;
    ULONG activation_rca;
    struct sdcard_Unit *live_unit;
    struct sdcard_Bus *bus;
    spinlock_t *held_lock;
    struct sdcard_CardInfo card;
    struct SDCardBase base;
};

static struct test_model model;

static void fail_check(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
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
    model.acquire_ok = TRUE;
    model.prepare_ok = TRUE;
    model.identify_ok = TRUE;
    model.activation_ok = TRUE;
    model.finish_ok = TRUE;
    model.candidate_rca = 0x4321UL;
    model.card.rca = model.candidate_rca;
    model.card.power = 0x00300000UL;
    model.card.flags = AF_Card_HighCapacity | AF_Card_4bitData |
                       AF_Card_HighSpeed;
    model.card.capacity = 0x100000ULL;
    model.card.cylinders = 0x800UL;
    model.card.heads = 1;
    model.card.sectors = 2;
    model.card.cid[0] = 0x10203040UL;
    model.card.cid[1] = 0x50607080UL;
    model.card.cid[2] = 0x90A0B0C0UL;
    model.card.cid[3] = 0xD0E0F000UL;
    model.card.csd[0] = 0x01234567UL;
    model.card.csd[1] = 0x89ABCDEFUL;
    model.card.csd[2] = 0x13579BDFUL;
    model.card.csd[3] = 0x2468ACE0UL;
    model.base.sdcard_Buses.first = NULL;
    model.bus = NULL;
    model.live_unit = NULL;
}

static void begin_case(void)
{
    ++model.cases;
}

static void mock_forbid(void)
{
    ++model.forbid_calls;
    ++model.forbid_depth;
}

static void mock_permit(void)
{
    CHECK(model.forbid_depth > 0U);
    --model.forbid_depth;
    ++model.permit_calls;
}

#if defined(__AROSEXEC_SMP__)
static void mock_spin_lock(spinlock_t *lock, void *success, ULONG mode,
                           struct SDCardBase *base)
{
    CHECK(lock != NULL);
    CHECK(success == NULL);
    CHECK(mode == SPINLOCK_MODE_WRITE);
    CHECK(base == &model.base);
    CHECK(model.forbid_depth == 1U);
    CHECK(model.spin_depth == 0U);
    CHECK(model.held_lock == NULL);
    model.held_lock = lock;
    ++model.spin_lock_calls;
    ++model.spin_depth;
}

static void mock_spin_unlock(spinlock_t *lock, struct SDCardBase *base)
{
    CHECK(lock == model.held_lock);
    CHECK(base == &model.base);
    CHECK(model.spin_depth == 1U);
    --model.spin_depth;
    model.held_lock = NULL;
    ++model.spin_unlock_calls;
}
#endif

static void check_callback_context(void)
{
    CHECK(model.owner_depth == 1U);
    CHECK(model.forbid_depth == 0U);
    CHECK(model.spin_depth == 0U);
}

static BOOL mock_begin(struct sdcard_Bus *bus, BOOL may_wait)
{
    CHECK(bus == model.bus);
    CHECK(model.forbid_depth == 0U);
    CHECK(model.spin_depth == 0U);
    ++model.begin_calls;
    model.begin_may_wait = may_wait;
    record_event(EVENT_BEGIN);
    if (model.acquire_ok)
        ++model.owner_depth;
    return model.acquire_ok;
}

static void mock_end(struct sdcard_Bus *bus)
{
    CHECK(bus == model.bus);
    CHECK(model.owner_depth == 1U);
    CHECK(model.forbid_depth == 0U);
    CHECK(model.spin_depth == 0U);
    --model.owner_depth;
    ++model.end_calls;
    record_event(EVENT_END);
}

static BOOL mock_needed(struct sdcard_Bus *bus)
{
    CHECK(bus == model.bus);
    check_callback_context();
    ++model.needed_calls;
    record_event(EVENT_NEEDED);
    return model.recovery_needed;
}

static BOOL mock_prepare(struct sdcard_Bus *bus)
{
    CHECK(bus == model.bus);
    check_callback_context();
    CHECK(model.live_unit != NULL);
    CHECK(!(model.live_unit->sdcu_Flags & AF_Card_Active));
    CHECK(model.live_unit->sdcu_Flags & AF_Card_Configuring);
    ++model.prepare_calls;
    record_event(EVENT_PREPARE);
    return model.prepare_ok;
}

static BOOL mock_finish(struct sdcard_Bus *bus, BOOL configured)
{
    CHECK(bus == model.bus);
    check_callback_context();
    CHECK(model.live_unit != NULL);
    CHECK(!(model.live_unit->sdcu_Flags & AF_Card_Active));
    CHECK(model.live_unit->sdcu_Flags & AF_Card_Configuring);
    CHECK(model.live_unit->sdcu_CardRCA != model.candidate_rca);
    model.finish_argument = configured;
    ++model.finish_calls;
    record_event(EVENT_FINISH);
    return model.finish_ok;
}

BOOL FNAME_SDCBUS(IdentifyCardLocked)(struct sdcard_Bus *bus,
                                     struct sdcard_CardInfo *card)
{
    CHECK(bus == model.bus);
    check_callback_context();
    ++model.identify_calls;
    record_event(EVENT_IDENTIFY);
    if (!model.identify_ok)
        return FALSE;
    *card = model.card;
    return TRUE;
}

static BOOL sdcard_StartUnitLocked(struct sdcard_Unit *candidate)
{
    CHECK(candidate != model.live_unit);
    CHECK(candidate->sdcu_Bus == model.bus);
    CHECK(!(candidate->sdcu_Flags &
            (AF_Card_Active | AF_Card_Configuring)));
    if (model.recovery_needed)
        CHECK(!(candidate->sdcu_Flags &
                (AF_Card_4bitData | AF_Card_HighSpeed | AF_Card_HighSpeed52)));
    CHECK(candidate->sdcu_Unit.unit_OpenCnt == 0UL);
    CHECK(candidate->sdcu_RemoveInt == NULL);
    CHECK(candidate->sdcu_SoftList.lh_Head == NULL);
    CHECK(candidate->sdcu_Read32 == model.live_unit->sdcu_Read32);
    CHECK(candidate->sdcu_Read64 == model.live_unit->sdcu_Read64);
    check_callback_context();
    ++model.activate_calls;
    record_event(EVENT_ACTIVATE);
    candidate->sdcu_Flags |= model.activation_flags;
    if (model.activation_ok)
        candidate->sdcu_CardRCA = model.candidate_rca;
    model.activation_candidate_is_distinct = candidate != model.live_unit;
    model.activation_rca = candidate->sdcu_CardRCA;
    return model.activation_ok;
}

/* INSERT_ACTUAL_SD_FUNCTIONS */

static void setup_unit(struct sdcard_Bus *bus, struct sdcard_Unit *unit,
                       struct sdcard_BusUnits *units)
{
    memset(bus, 0, sizeof(*bus));
    memset(unit, 0, sizeof(*unit));
    memset(units, 0, sizeof(*units));
    reset_model();
    model.bus = bus;
    model.live_unit = unit;
    bus->sdcb_BusUnits = units;
    bus->sdcb_DeviceBase = &model.base;
    bus->sdcb_BeginTransaction = mock_begin;
    bus->sdcb_EndTransaction = mock_end;
    bus->sdcb_RecoveryNeeded = mock_needed;
    bus->sdcb_RecoveryPrepare = mock_prepare;
    bus->sdcb_RecoveryFinish = mock_finish;
    units->sdcbu_UnitBase = 4UL;
    units->sdcbu_UnitMax = 2UL;
    units->sdcbu_UnitCnt = 1UL;
    units->sdcbu_Units = unit;
    unit->sdcu_Bus = bus;
    unit->sdcu_UnitNum = 4UL;
    unit->sdcu_Unit.unit_OpenCnt = 3UL;
    unit->sdcu_CardRCA = 0x1111UL;
    unit->sdcu_CardPower = 0x00100000UL;
    unit->sdcu_Flags = AF_Card_Active | AF_Card_HighCapacity |
                       AF_Card_WriteProtect | AF_Card_4bitData |
                       AF_Card_HighSpeed52;
    unit->sdcu_Capacity = model.card.capacity;
    unit->sdcu_Cylinders = model.card.cylinders;
    unit->sdcu_Heads = model.card.heads;
    unit->sdcu_Sectors = model.card.sectors;
    unit->sdcu_Read32 = (BYTE (*)(struct sdcard_Unit *, ULONG, ULONG, APTR,
                                  ULONG *))(uintptr_t)0x101UL;
    unit->sdcu_Read64 = (BYTE (*)(struct sdcard_Unit *, UQUAD, ULONG, APTR,
                                  ULONG *))(uintptr_t)0x202UL;
    unit->sdcu_Write32 = (BYTE (*)(struct sdcard_Unit *, ULONG, ULONG, APTR,
                                   ULONG *))(uintptr_t)0x303UL;
    unit->sdcu_Write64 = (BYTE (*)(struct sdcard_Unit *, UQUAD, ULONG, APTR,
                                   ULONG *))(uintptr_t)0x404UL;
    unit->sdcu_DirectSCSI = (BYTE (*)(struct sdcard_Unit *, struct SCSICmd *))
                            (uintptr_t)0x505UL;
    unit->sdcu_Eject = (BYTE (*)(struct sdcard_Unit *))(uintptr_t)0x606UL;
    unit->sdcu_ChangeNum = 0xA55AUL;
    unit->sdcu_RemoveInt = (struct Interrupt *)(uintptr_t)0x707UL;
    unit->sdcu_SoftList.lh_Head = (struct Node *)(uintptr_t)0x808UL;
    unit->sdcu_SoftList.lh_Tail = (struct Node *)(uintptr_t)0x909UL;
    unit->sdcu_SoftList.lh_TailPred = (struct Node *)(uintptr_t)0xA0AUL;
    unit->sdcu_DevMask = 0x21U;
    unit->sdcu_SenseKey = 0x32U;
    unit->sdcu_DevType = 0x43U;
    memcpy(unit->sdcu_CardCID, model.card.cid, sizeof(unit->sdcu_CardCID));
    memcpy(unit->sdcu_CardCSD, model.card.csd, sizeof(unit->sdcu_CardCSD));
    bus->sdcb_RespListener = (struct TagItem *)(uintptr_t)0xABCUL;
    bus->sdcb_DataListener = (struct TagItem *)(uintptr_t)0xDEFUL;
}

static void assert_guard_balanced(void)
{
    CHECK(model.forbid_depth == 0U);
    CHECK(model.spin_depth == 0U);
    CHECK(model.owner_depth == 0U);
    CHECK(model.held_lock == NULL);
    CHECK(model.forbid_calls == model.permit_calls);
#if defined(__AROSEXEC_SMP__)
    CHECK(model.spin_lock_calls == model.forbid_calls);
    CHECK(model.spin_lock_calls == model.spin_unlock_calls);
#else
    CHECK(model.spin_lock_calls == 0U && model.spin_unlock_calls == 0U);
#endif
}

static void test_stage_and_same_card(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit, candidate;
    struct sdcard_BusUnits units;
    struct sdcard_CardInfo card;

    begin_case();
    setup_unit(&bus, &unit, &units);
    unit.sdcu_Flags |= AF_Card_Configuring;
    sdcard_StageUnit(&unit, &candidate);
    CHECK(candidate.sdcu_Bus == &bus);
    CHECK(candidate.sdcu_UnitNum == unit.sdcu_UnitNum);
    CHECK(candidate.sdcu_CardRCA == unit.sdcu_CardRCA);
    CHECK(candidate.sdcu_CardPower == unit.sdcu_CardPower);
    CHECK(candidate.sdcu_Flags ==
          (unit.sdcu_Flags & ~(AF_Card_Active | AF_Card_Configuring)));
    CHECK(candidate.sdcu_Capacity == unit.sdcu_Capacity);
    CHECK(candidate.sdcu_Cylinders == unit.sdcu_Cylinders);
    CHECK(candidate.sdcu_Heads == unit.sdcu_Heads);
    CHECK(candidate.sdcu_Sectors == unit.sdcu_Sectors);
    CHECK(candidate.sdcu_Read32 == unit.sdcu_Read32);
    CHECK(candidate.sdcu_Read64 == unit.sdcu_Read64);
    CHECK(candidate.sdcu_Unit.unit_OpenCnt == 0UL);
    CHECK(candidate.sdcu_Write32 == NULL && candidate.sdcu_Write64 == NULL);
    CHECK(candidate.sdcu_DirectSCSI == NULL && candidate.sdcu_Eject == NULL);
    CHECK(candidate.sdcu_RemoveInt == NULL);
    CHECK(candidate.sdcu_SoftList.lh_Head == NULL);
    CHECK(candidate.sdcu_ChangeNum == 0UL);
    CHECK(candidate.sdcu_CardCID[0] == 0UL && candidate.sdcu_CardCSD[0] == 0UL);

    memcpy(unit.sdcu_CardCID, model.card.cid, sizeof(unit.sdcu_CardCID));
    memcpy(unit.sdcu_CardCSD, model.card.csd, sizeof(unit.sdcu_CardCSD));
    card = model.card;
    CHECK(sdcard_SameCard(&unit, &card));
    card.cid[2] ^= 1UL;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    card.csd[1] ^= 1UL;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    ++card.capacity;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    ++card.cylinders;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    ++card.heads;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    ++card.sectors;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    card.flags ^= AF_Card_HighCapacity;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    card.flags |= AF_Card_MMC;
    CHECK(!sdcard_SameCard(&unit, &card));
    card = model.card;
    card.flags ^= AF_Card_HighSpeed;
    CHECK(sdcard_SameCard(&unit, &card));
    assert_guard_balanced();
}

static void setup_recovery(struct sdcard_Bus *bus, struct sdcard_Unit *unit,
                           struct sdcard_BusUnits *units)
{
    setup_unit(bus, unit, units);
    model.recovery_needed = TRUE;
    model.activation_flags = AF_Card_4bitData | AF_Card_HighSpeed;
    model.card.rca = model.candidate_rca;
}

static void check_identity_and_open_state(const struct sdcard_Unit *unit,
                                          const ULONG old_rca,
                                          const ULONG old_opens,
                                          const ULONG old_change)
{
    CHECK(unit->sdcu_CardRCA == old_rca);
    CHECK(unit->sdcu_Capacity == model.card.capacity);
    CHECK(unit->sdcu_Cylinders == model.card.cylinders);
    CHECK(unit->sdcu_Heads == model.card.heads);
    CHECK(unit->sdcu_Sectors == model.card.sectors);
    CHECK(!memcmp(unit->sdcu_CardCID, model.card.cid, sizeof(model.card.cid)));
    CHECK(!memcmp(unit->sdcu_CardCSD, model.card.csd, sizeof(model.card.csd)));
    CHECK(unit->sdcu_Unit.unit_OpenCnt == old_opens);
    CHECK(unit->sdcu_ChangeNum == old_change);
    CHECK(unit->sdcu_Write32 ==
          (BYTE (*)(struct sdcard_Unit *, ULONG, ULONG, APTR, ULONG *))
              (uintptr_t)0x303UL);
    CHECK(unit->sdcu_Write64 ==
          (BYTE (*)(struct sdcard_Unit *, UQUAD, ULONG, APTR, ULONG *))
              (uintptr_t)0x404UL);
    CHECK(unit->sdcu_DirectSCSI ==
          (BYTE (*)(struct sdcard_Unit *, struct SCSICmd *))(uintptr_t)0x505UL);
    CHECK(unit->sdcu_Eject ==
          (BYTE (*)(struct sdcard_Unit *))(uintptr_t)0x606UL);
    CHECK(unit->sdcu_RemoveInt == (struct Interrupt *)(uintptr_t)0x707UL);
    CHECK(unit->sdcu_SoftList.lh_Head == (struct Node *)(uintptr_t)0x808UL);
    CHECK(unit->sdcu_SoftList.lh_Tail == (struct Node *)(uintptr_t)0x909UL);
    CHECK(unit->sdcu_SoftList.lh_TailPred == (struct Node *)(uintptr_t)0xA0AUL);
    CHECK(unit->sdcu_DevMask == 0x21U);
    CHECK(unit->sdcu_SenseKey == 0x32U);
    CHECK(unit->sdcu_DevType == 0x43U);
}

static void test_recovery_success(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    struct sdcard_BusUnits units;
    ULONG old_opens, old_change;
    struct sdcard_Unit *address;

    begin_case();
    setup_recovery(&bus, &unit, &units);
    old_opens = unit.sdcu_Unit.unit_OpenCnt;
    old_change = unit.sdcu_ChangeNum;
    address = &unit;
    CHECK(FNAME_SDCBUS(RecoverUnit)(&unit));
    CHECK(&unit == address);
    CHECK(model.begin_calls == 1U && model.begin_may_wait == TRUE);
    CHECK(model.end_calls == 1U);
    CHECK(model.needed_calls == 1U && model.prepare_calls == 1U);
    CHECK(model.identify_calls == 1U && model.activate_calls == 1U);
    CHECK(model.finish_calls == 1U && model.finish_argument == TRUE);
    CHECK(model.activation_candidate_is_distinct);
    CHECK(model.activation_rca == model.candidate_rca);
    CHECK(unit.sdcu_CardRCA == model.candidate_rca);
    CHECK((unit.sdcu_Flags & AF_Card_Active) != 0UL);
    CHECK((unit.sdcu_Flags & AF_Card_Configuring) == 0UL);
    CHECK((unit.sdcu_Flags & AF_Card_WriteProtect) != 0UL);
    CHECK((unit.sdcu_Flags &
           (AF_Card_4bitData | AF_Card_HighSpeed | AF_Card_HighSpeed52)) ==
          (AF_Card_4bitData | AF_Card_HighSpeed));
    CHECK(unit.sdcu_CardPower == 0x00100000UL);
    CHECK(unit.sdcu_CardRCA != 0x1111UL);
    CHECK(units.sdcbu_UnitCnt == 1UL && units.sdcbu_Units == &unit);
    CHECK(bus.sdcb_RespListener == (struct TagItem *)(uintptr_t)0xABCUL);
    CHECK(bus.sdcb_DataListener == (struct TagItem *)(uintptr_t)0xDEFUL);
    check_identity_and_open_state(&unit, model.candidate_rca, old_opens,
                                  old_change);
    CHECK(model.event_count == 7U);
    CHECK(model.events[0] == EVENT_BEGIN);
    CHECK(model.events[1] == EVENT_NEEDED);
    CHECK(model.events[2] == EVENT_PREPARE);
    CHECK(model.events[3] == EVENT_IDENTIFY);
    CHECK(model.events[4] == EVENT_ACTIVATE);
    CHECK(model.events[5] == EVENT_FINISH);
    CHECK(model.events[6] == EVENT_END);
    assert_guard_balanced();
}

static void test_recovery_failure(BOOL prepare_ok, BOOL identify_ok,
                                  BOOL activation_ok, BOOL finish_ok)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    struct sdcard_BusUnits units;
    ULONG old_opens, old_change, old_flags;
    struct Node *old_response, *old_data;
    BOOL reached_activation = prepare_ok && identify_ok;
    BOOL configured = reached_activation && activation_ok;

    begin_case();
    setup_recovery(&bus, &unit, &units);
    old_opens = unit.sdcu_Unit.unit_OpenCnt;
    old_change = unit.sdcu_ChangeNum;
    old_flags = unit.sdcu_Flags;
    old_response = (struct Node *)(uintptr_t)0x111UL;
    old_data = (struct Node *)(uintptr_t)0x222UL;
    /* Listener fields live on the shared bus and must not be rewritten. */
    bus.sdcb_RecoveryNeeded = mock_needed;
    model.prepare_ok = prepare_ok;
    model.identify_ok = identify_ok;
    model.activation_ok = activation_ok;
    model.finish_ok = finish_ok;
    model.bus = &bus;
    /* Add the two production listener pointers to the fixture bus model. */
    bus.sdcb_RespListener = (struct TagItem *)old_response;
    bus.sdcb_DataListener = (struct TagItem *)old_data;
    {
        BOOL result = FNAME_SDCBUS(RecoverUnit)(&unit);
        CHECK(result == (configured && finish_ok));
    }
    CHECK(unit.sdcu_CardRCA == 0x1111UL);
    CHECK((unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring)) == 0UL);
    CHECK((unit.sdcu_Flags & ~(AF_Card_Active | AF_Card_Configuring)) ==
          (old_flags & ~(AF_Card_Active | AF_Card_Configuring)));
    CHECK(unit.sdcu_Unit.unit_OpenCnt == old_opens);
    CHECK(units.sdcbu_UnitCnt == 1UL && units.sdcbu_Units == &unit);
    CHECK(unit.sdcu_ChangeNum == old_change);
    CHECK(bus.sdcb_RespListener == (struct TagItem *)old_response);
    CHECK(bus.sdcb_DataListener == (struct TagItem *)old_data);
    CHECK(model.finish_calls == 1U);
    CHECK(model.finish_argument == configured);
    CHECK(model.activate_calls == (reached_activation ? 1U : 0U));
    check_identity_and_open_state(&unit, 0x1111UL, old_opens, old_change);
    assert_guard_balanced();
}

static void test_identity_rejections(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    struct sdcard_BusUnits units;
    unsigned int mutation;

    for (mutation = 0U; mutation < 8U; ++mutation)
    {
        begin_case();
        setup_recovery(&bus, &unit, &units);
        switch (mutation)
        {
            case 0U: model.card.cid[0] ^= 1UL; break;
            case 1U: model.card.csd[3] ^= 1UL; break;
            case 2U: ++model.card.capacity; break;
            case 3U: ++model.card.cylinders; break;
            case 4U: ++model.card.heads; break;
            case 5U: ++model.card.sectors; break;
            case 6U: model.card.flags ^= AF_Card_HighCapacity; break;
            default: model.card.flags |= AF_Card_MMC; break;
        }
        CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
        CHECK(model.identify_calls == 1U);
        CHECK(model.activate_calls == 0U);
        CHECK(model.finish_calls == 1U && !model.finish_argument);
        CHECK(unit.sdcu_CardRCA == 0x1111UL);
        CHECK((unit.sdcu_Flags & (AF_Card_Active | AF_Card_Configuring)) == 0UL);
        CHECK(unit.sdcu_Unit.unit_OpenCnt == 3UL);
        CHECK(unit.sdcu_CardCID[0] == 0x10203040UL);
        CHECK(unit.sdcu_CardCSD[3] == 0x2468ACE0UL);
        assert_guard_balanced();
    }
}

static void test_recovery_guards_and_hooks(void)
{
    struct sdcard_Bus bus;
    struct sdcard_Unit unit;
    struct sdcard_BusUnits units;
    BOOL (*needed)(struct sdcard_Bus *);
    BOOL (*prepare)(struct sdcard_Bus *);
    BOOL (*finish)(struct sdcard_Bus *, BOOL);
    BOOL (*begin)(struct sdcard_Bus *, BOOL);
    void (*end)(struct sdcard_Bus *);

    begin_case();
    setup_recovery(&bus, &unit, &units);
    needed = bus.sdcb_RecoveryNeeded;
    prepare = bus.sdcb_RecoveryPrepare;
    finish = bus.sdcb_RecoveryFinish;
    begin = bus.sdcb_BeginTransaction;
    end = bus.sdcb_EndTransaction;
    bus.sdcb_RecoveryNeeded = NULL;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    bus.sdcb_RecoveryNeeded = needed;
    bus.sdcb_RecoveryPrepare = NULL;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    bus.sdcb_RecoveryPrepare = prepare;
    bus.sdcb_RecoveryFinish = NULL;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    bus.sdcb_RecoveryFinish = finish;
    bus.sdcb_EndTransaction = NULL;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    bus.sdcb_EndTransaction = end;
    bus.sdcb_BeginTransaction = NULL;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    bus.sdcb_BeginTransaction = begin;
    CHECK(model.begin_calls == 0U);
    CHECK(model.needed_calls == 0U && model.prepare_calls == 0U);
    CHECK(unit.sdcu_Flags & AF_Card_Active);
    CHECK(!(unit.sdcu_Flags & AF_Card_Configuring));
    assert_guard_balanced();

    begin_case();
    setup_recovery(&bus, &unit, &units);
    model.acquire_ok = FALSE;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    CHECK(model.begin_calls == 1U && model.end_calls == 0U);
    CHECK(model.needed_calls == 0U && model.prepare_calls == 0U);
    CHECK(unit.sdcu_Flags & AF_Card_Active);
    CHECK(!(unit.sdcu_Flags & AF_Card_Configuring));
    assert_guard_balanced();

    begin_case();
    setup_recovery(&bus, &unit, &units);
    model.recovery_needed = FALSE;
    CHECK(FNAME_SDCBUS(RecoverUnit)(&unit));
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.needed_calls == 1U && model.prepare_calls == 0U);
    CHECK(model.identify_calls == 0U && model.finish_calls == 0U);
    CHECK(unit.sdcu_Flags & AF_Card_Active);
    assert_guard_balanced();

    begin_case();
    setup_recovery(&bus, &unit, &units);
    unit.sdcu_Flags |= AF_Card_Configuring;
    CHECK(!FNAME_SDCBUS(RecoverUnit)(&unit));
    CHECK(model.begin_calls == 1U && model.end_calls == 1U);
    CHECK(model.needed_calls == 1U && model.prepare_calls == 0U);
    CHECK(model.identify_calls == 0U && model.finish_calls == 0U);
    CHECK(unit.sdcu_Flags & AF_Card_Active);
    CHECK(unit.sdcu_Flags & AF_Card_Configuring);
    assert_guard_balanced();
}

static void test_open_close_guards(void)
{
    struct sdcard_Bus first, second;
    struct sdcard_Unit units_array[2];
    struct sdcard_BusUnits first_units, second_units;
    struct SDCardBase base;
    struct IORequest request;
    struct sdcard_Unit *selected;

    begin_case();
    setup_unit(&first, &units_array[0], &first_units);
    setup_unit(&second, &units_array[1], &second_units);
    first.next = &second;
    second.next = NULL;
    first_units.sdcbu_UnitBase = 1UL;
    first_units.sdcbu_UnitCnt = 1UL;
    second_units.sdcbu_UnitBase = 10UL;
    second_units.sdcbu_UnitCnt = 1UL;
    first_units.sdcbu_Units = &units_array[0];
    second_units.sdcbu_Units = &units_array[1];
    base.sdcard_Buses.first = &first;
    model.base = base;
    model.bus = &second;
    model.live_unit = &units_array[1];
    units_array[1].sdcu_Bus = &second;
    units_array[1].sdcu_Flags |= AF_Card_Active;
    request.io_Unit = NULL;
    CHECK(FNAME_SDC(Open)(&base, &request, 10UL, 0UL));
    selected = (struct sdcard_Unit *)request.io_Unit;
    CHECK(selected == &units_array[1]);
    CHECK(selected->sdcu_Unit.unit_OpenCnt == 4UL);
    CHECK(model.begin_calls == 0U);
    CHECK(request.io_Error == 0);
    CHECK(FNAME_SDC(Close)(&base, &request));
    CHECK(request.io_Unit == (struct Unit *)~(IPTR)0);
    CHECK(selected->sdcu_Unit.unit_OpenCnt == 3UL);
    assert_guard_balanced();

    begin_case();
    reset_model();
    setup_unit(&first, &units_array[0], &first_units);
    first.next = NULL;
    first_units.sdcbu_UnitBase = 10UL;
    first_units.sdcbu_UnitCnt = 1UL;
    first_units.sdcbu_Units = &units_array[0];
    base.sdcard_Buses.first = &first;
    model.base = base;
    model.bus = &first;
    model.live_unit = &units_array[0];
    units_array[0].sdcu_Flags &= ~AF_Card_Active;
    request.io_Unit = NULL;
    model.activation_ok = FALSE;
    CHECK(!FNAME_SDC(Open)(&base, &request, 10UL, 0UL));
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    CHECK(units_array[0].sdcu_Unit.unit_OpenCnt == 3UL);
    CHECK(model.activate_calls == 1U);
    assert_guard_balanced();

    begin_case();
    reset_model();
    setup_unit(&first, &units_array[0], &first_units);
    first.next = NULL;
    first_units.sdcbu_UnitBase = 10UL;
    first_units.sdcbu_UnitCnt = 1UL;
    first_units.sdcbu_Units = &units_array[0];
    base.sdcard_Buses.first = &first;
    model.base = base;
    model.bus = &first;
    model.live_unit = &units_array[0];
    units_array[0].sdcu_Flags |= AF_Card_Configuring;
    request.io_Unit = NULL;
    CHECK(!FNAME_SDC(Open)(&base, &request, 10UL, 0UL));
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    CHECK(units_array[0].sdcu_Unit.unit_OpenCnt == 3UL);
    CHECK(model.begin_calls == 0U);
    assert_guard_balanced();

    begin_case();
    reset_model();
    setup_unit(&first, &units_array[0], &first_units);
    first.next = NULL;
    first_units.sdcbu_UnitBase = 10UL;
    first_units.sdcbu_UnitCnt = 1UL;
    first_units.sdcbu_Units = &units_array[0];
    base.sdcard_Buses.first = &first;
    model.base = base;
    model.bus = &first;
    model.live_unit = &units_array[0];
    units_array[0].sdcu_Flags |= AF_Card_Active;
    units_array[0].sdcu_Unit.unit_OpenCnt = 0xFFFFUL;
    request.io_Unit = NULL;
    CHECK(!FNAME_SDC(Open)(&base, &request, 10UL, 0UL));
    CHECK(units_array[0].sdcu_Unit.unit_OpenCnt == 0xFFFFUL);
    CHECK(request.io_Unit == NULL && request.io_Error == IOERR_OPENFAIL);
    assert_guard_balanced();
}

int main(void)
{
    test_stage_and_same_card();
    test_recovery_success();
    test_recovery_failure(FALSE, TRUE, TRUE, TRUE);
    test_recovery_failure(TRUE, FALSE, TRUE, TRUE);
    test_recovery_failure(TRUE, TRUE, FALSE, TRUE);
    test_recovery_failure(TRUE, TRUE, TRUE, FALSE);
    test_identity_rejections();
    test_recovery_guards_and_hooks();
    test_open_close_guards();
    printf("SD recovery fixture: %u cases, %u checks\n",
           model.cases, model.checks);
    return 0;
}
