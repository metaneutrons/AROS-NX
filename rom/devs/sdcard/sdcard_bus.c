/*
    Copyright (C) 2013-2026, The AROS Development Team. All rights reserved.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <aros/bootloader.h>
#include <aros/symbolsets.h>
#include <exec/exec.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <utility/utility.h>
#include <libraries/expansion.h>
#include <libraries/configvars.h>
#include <dos/bptr.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/bootloader.h>
#include <proto/expansion.h>
#include <proto/utility.h>
#include <proto/kernel.h>

#include <hardware/mmc.h>
#include <hardware/sdhc.h>

#include <string.h>

#include "sdcard_base.h"
#include "sdcard_bus.h"
#include "sdcard_unit.h"
#include "timer.h"

#include LC_LIBDEFS_FILE

/* Generic "Bus Unit" Functions */

/* How long a single block may keep the PIO transfer waiting */
#define SDHCI_DATA_TIMEOUT_US   1000000

/*
 * How often the controller is re-examined while waiting on it. Waiting a
 * millisecond at a time costs more than the work being waited for: a 64K
 * transfer only takes about 2.7ms, so a single overshoot is a quarter of it.
 */
#define SDHCI_POLL_US           25

static const char *str_mmc0 = "MMC0";

void SDCBUS_LockUnit(struct sdcard_Bus *bus)
{
    Forbid();
#if defined(__AROSEXEC_SMP__)
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;
    KrnSpinLock(&bus->sdcb_UnitLock, NULL, SPINLOCK_MODE_WRITE);
#else
    (void)bus;
#endif
}

void SDCBUS_UnlockUnit(struct sdcard_Bus *bus)
{
#if defined(__AROSEXEC_SMP__)
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;
    KrnSpinUnLock(&bus->sdcb_UnitLock);
#else
    (void)bus;
#endif
    Permit();
}

static BOOL sdcard_StartUnitLocked(struct sdcard_Unit *sdcUnit)
{
    struct TagItem sdcStartTags[] =
    {
        {SDCARD_TAG_CMD,         MMC_CMD_SELECT_CARD},
        {SDCARD_TAG_ARG,         sdcUnit->sdcu_CardRCA << 16},
        {SDCARD_TAG_RSPTYPE,     MMC_RSP_R1},
        {SDCARD_TAG_RSP,         0},
        {TAG_DONE,               0}
    };

    DFUNCS(bug("[SDCard%02ld] %s()\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));

    if (sdcUnit->sdcu_Bus->sdcb_SectorShift >= 32)
        return FALSE;

    /*
     * Fix #40: Send CMD7 (SELECT_CARD) BEFORE capability detection.
     * ACMD51 (SEND_SCR) and CMD6 (SWITCH_FUNC) are data transfer commands
     * that require the card to be in Transfer state. On real hardware (e.g.
     * Pi 3B), issuing these before CMD7 causes data transfer errors that
     * can leave the SDHCI controller in a bad state, breaking subsequent
     * reads (including partition table scanning by partition.library).
     * QEMU's SD emulation is more permissive and doesn't enforce this.
     */
    if ((SDCBUS_SendCmd(sdcStartTags, sdcUnit->sdcu_Bus) != -1) &&
        (SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT|SDHCI_PS_DATA_INHIBIT, 1000, sdcUnit->sdcu_Bus) != -1) &&
        !(sdcStartTags[3].ti_Data & MMC_STATUS_MASK))
    {
        if (FNAME_SDCUNIT(WaitStatus)(10000, sdcUnit) == -1)
        {
            D(bug("[SDCard%02ld] %s: Failed to Wait for Cards status\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));
            return FALSE;
        }

        D(bug("[SDCard%02ld] %s: Selected card with RCA %d [select response %08x]\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__, sdcUnit->sdcu_CardRCA, sdcStartTags[3].ti_Data));
        D(bug("[SDCard%02ld] %s: Card is now operating in Transfer Mode\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));

        /* Recompute negotiated features rather than retaining a previous session. */
        sdcUnit->sdcu_Flags &= ~(AF_Card_4bitData | AF_Card_HighSpeed | AF_Card_HighSpeed52);
        /* Now that card is in transfer state, detect capabilities. */
        if (sdcUnit->sdcu_Flags & AF_Card_MMC)
        {
            if (FNAME_SDCUNIT(MMCChangeFrequency)(sdcUnit) == (ULONG)-1)
                return FALSE;
        }
        else
        {
            if (FNAME_SDCUNIT(SDSCChangeFrequency)(sdcUnit) == (ULONG)-1)
                return FALSE;
        }

        sdcard_Udelay(1000);

        if (!(sdcUnit->sdcu_Flags & AF_Card_HighCapacity))
        {
            sdcStartTags[0].ti_Data = MMC_CMD_SET_BLOCKLEN;
            sdcStartTags[1].ti_Data = 1UL << sdcUnit->sdcu_Bus->sdcb_SectorShift;
            sdcStartTags[2].ti_Data = MMC_RSP_R1;
            sdcStartTags[3].ti_Data = 0;
            if ((SDCBUS_SendCmd(sdcStartTags, sdcUnit->sdcu_Bus) != -1) &&
                (SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT|SDHCI_PS_DATA_INHIBIT, 1000, sdcUnit->sdcu_Bus) != -1) &&
                !(sdcStartTags[3].ti_Data & MMC_STATUS_MASK))
            {
                D(bug("[SDCard%02ld] %s: Blocklen set to %d\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__, sdcStartTags[1].ti_Data));
            }
            else
            {
                D(bug("[SDCard%02ld] %s: Failed to change Blocklen\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));
                return FALSE;
            }
        }

        if (sdcUnit->sdcu_Flags & AF_Card_MMC)
        {
            if (sdcUnit->sdcu_Flags & AF_Card_HighSpeed)
            {
                if (sdcUnit->sdcu_Flags & AF_Card_HighSpeed52)
                    SDCBUS_SetClock(52000000, sdcUnit->sdcu_Bus);
                else
                    SDCBUS_SetClock(26000000, sdcUnit->sdcu_Bus);
            }
            else
                SDCBUS_SetClock(20000000, sdcUnit->sdcu_Bus);
        }
        else
        {
            if (sdcUnit->sdcu_Flags & AF_Card_HighSpeed)
                SDCBUS_SetClock(50000000, sdcUnit->sdcu_Bus);
            else
                SDCBUS_SetClock(25000000, sdcUnit->sdcu_Bus);
        }

        /* Enable 4-bit bus width if supported by the card */
        if (sdcUnit->sdcu_Flags & AF_Card_4bitData)
        {
            if (!sdcUnit->sdcu_Bus->sdcb_SetBusWidth)
                return FALSE;
            D(bug("[SDCard%02ld] %s: Switching to 4-bit bus width\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));

            /* Send ACMD6 (SET_BUS_WIDTH) with arg=2 for 4-bit mode */
            sdcStartTags[0].ti_Data = MMC_CMD_APP_CMD;
            sdcStartTags[1].ti_Data = sdcUnit->sdcu_CardRCA << 16;
            sdcStartTags[2].ti_Data = MMC_RSP_R1;
            sdcStartTags[3].ti_Data = 0;
            sdcStartTags[4].ti_Tag = TAG_DONE;
            if ((SDCBUS_SendCmd(sdcStartTags, sdcUnit->sdcu_Bus) != -1) &&
                (SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT|SDHCI_PS_DATA_INHIBIT, 1000, sdcUnit->sdcu_Bus) != -1) &&
                !(sdcStartTags[3].ti_Data & MMC_STATUS_MASK))
            {
                sdcStartTags[0].ti_Data = SD_CMD_APP_SET_BUS_WIDTH;
                sdcStartTags[1].ti_Data = 2; /* 4-bit mode */
                sdcStartTags[2].ti_Data = MMC_RSP_R1;
                sdcStartTags[3].ti_Data = 0;
                if ((SDCBUS_SendCmd(sdcStartTags, sdcUnit->sdcu_Bus) != -1) &&
                    (SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT|SDHCI_PS_DATA_INHIBIT, 1000, sdcUnit->sdcu_Bus) != -1) &&
                    !(sdcStartTags[3].ti_Data & MMC_STATUS_MASK))
                {
                    /* Set 4-bit mode in controller — controller-specific via BusInit */
                    sdcUnit->sdcu_Bus->sdcb_SetBusWidth(4, sdcUnit->sdcu_Bus);
                    D(bug("[SDCard%02ld] %s: 4-bit bus width enabled\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));
                }
                else
                {
                    D(bug("[SDCard%02ld] %s: Failed to set 4-bit bus width\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));
                    return FALSE;
                }
            }
            else
                return FALSE;
        }

        /* Prove the controller's own transfers land where we think they do. */
        FNAME_SDCBUS(ADMAVerify)(sdcUnit);

        /* Void backend setters and optional diagnostics cannot report errors
         * directly. Prove TRAN/ready before the caller may publish active. */
        if (FNAME_SDCUNIT(WaitStatus)(1000, sdcUnit) == (ULONG)-1)
            return FALSE;
    }
    else
    {
        /*
         * A card left in stand-by answers CMD9 but ignores CMD17/CMD18
         * outright, so the only sign of this is a bare command timeout much
         * later, once the transfer clock and bus width have quietly been
         * skipped along with everything else in here.
         */
        bug("[SDCard%02ld] %s: CMD7 (RCA %d) not acknowledged, card stays in stand-by [PS %08x BS %08x]\n",
            sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__, sdcUnit->sdcu_CardRCA,
            sdcUnit->sdcu_Bus->sdcb_IOReadLong ?
                sdcUnit->sdcu_Bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, sdcUnit->sdcu_Bus) : 0,
            sdcUnit->sdcu_Bus->sdcb_BusStatus);

        return FALSE;
    }

    D(bug("[SDCard%02ld] %s: Done.\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));

    return TRUE;
}

/* Stage protocol/transfer fields only, never the embedded Exec unit, open
 * count, change notifications or lists. The caller holds the unit guard. */
static void sdcard_StageUnit(const struct sdcard_Unit *unit,
                              struct sdcard_Unit *candidate)
{
    memset(candidate, 0, sizeof(*candidate));
    candidate->sdcu_Bus = unit->sdcu_Bus;
    candidate->sdcu_UnitNum = unit->sdcu_UnitNum;
    candidate->sdcu_CardRCA = unit->sdcu_CardRCA;
    candidate->sdcu_CardPower = unit->sdcu_CardPower;
    candidate->sdcu_Flags = unit->sdcu_Flags &
                            ~(AF_Card_Active | AF_Card_Configuring);
    candidate->sdcu_Capacity = unit->sdcu_Capacity;
    candidate->sdcu_Cylinders = unit->sdcu_Cylinders;
    candidate->sdcu_Heads = unit->sdcu_Heads;
    candidate->sdcu_Sectors = unit->sdcu_Sectors;
    candidate->sdcu_Read32 = unit->sdcu_Read32;
    candidate->sdcu_Read64 = unit->sdcu_Read64;
}

/* Publish only mutable negotiated protocol state. Geometry, identity and
 * write policy must never change underneath a mounted/open unit. */
static void sdcard_PublishUnit(struct sdcard_Unit *unit,
                                const struct sdcard_Unit *candidate, BOOL result)
{
    const ULONG negotiated = AF_Card_4bitData | AF_Card_HighSpeed |
                             AF_Card_HighSpeed52;
    SDCBUS_LockUnit(unit->sdcu_Bus);
    if (result)
    {
        unit->sdcu_CardRCA = candidate->sdcu_CardRCA;
        unit->sdcu_Flags = (unit->sdcu_Flags &
                            ~(negotiated | AF_Card_Configuring)) |
                            (candidate->sdcu_Flags & negotiated) | AF_Card_Active;
    }
    else
        unit->sdcu_Flags &= ~(AF_Card_Active | AF_Card_Configuring);
    SDCBUS_UnlockUnit(unit->sdcu_Bus);
}

BOOL FNAME_SDCBUS(StartUnit)(struct sdcard_Unit *sdcUnit)
{
    struct sdcard_Bus *bus = sdcUnit->sdcu_Bus;
    struct sdcard_Unit candidate;
    BOOL result;

    if (!SDCBUS_BeginTransaction(bus, FALSE))
        return FALSE;

    SDCBUS_LockUnit(bus);
    if (sdcUnit->sdcu_RecoveryBlocked)
    {
        SDCBUS_UnlockUnit(bus);
        SDCBUS_EndTransaction(bus);
        return FALSE;
    }
    if (sdcUnit->sdcu_Flags & (AF_Card_Active | AF_Card_Configuring))
    {
        result = !!(sdcUnit->sdcu_Flags & AF_Card_Active) &&
                   !(sdcUnit->sdcu_Flags & AF_Card_Configuring);
        SDCBUS_UnlockUnit(bus);
        SDCBUS_EndTransaction(bus);
        return result;
    }
    sdcUnit->sdcu_Flags |= AF_Card_Configuring;
    sdcard_StageUnit(sdcUnit, &candidate);
    SDCBUS_UnlockUnit(bus);

    result = sdcard_StartUnitLocked(&candidate);
    sdcard_PublishUnit(sdcUnit, &candidate, result);
    SDCBUS_EndTransaction(bus);
    return result;
}

static BOOL sdcard_SameCard(const struct sdcard_Unit *unit,
                             const struct sdcard_CardInfo *card)
{
    const ULONG protocol = AF_Card_HighCapacity | AF_Card_MMC;
    return !memcmp(unit->sdcu_CardCID, card->cid, sizeof(card->cid)) &&
           !memcmp(unit->sdcu_CardCSD, card->csd, sizeof(card->csd)) &&
           unit->sdcu_Capacity == card->capacity &&
           unit->sdcu_Cylinders == card->cylinders &&
           unit->sdcu_Heads == card->heads && unit->sdcu_Sectors == card->sectors &&
           (unit->sdcu_Flags & protocol) == (card->flags & protocol);
}

/* Explicit task-context operation. BusTask/radio dispatch is deliberately
 * separate; callers must not call this while holding a metadata/IRQ lock.
 * A failed request must release its outer host transaction first. */
BOOL FNAME_SDCBUS(RecoverUnit)(struct sdcard_Unit *unit)
{
    struct sdcard_Bus *bus = unit->sdcu_Bus;
    struct sdcard_Unit candidate;
    struct sdcard_CardInfo card;
    BOOL result = FALSE;

    if (!bus->sdcb_RecoveryNeeded || !bus->sdcb_RecoveryPrepare ||
        !bus->sdcb_RecoveryFinish || !bus->sdcb_BeginTransaction ||
        !bus->sdcb_EndTransaction || !SDCBUS_BeginTransaction(bus, TRUE))
        return FALSE;

    if (!bus->sdcb_RecoveryNeeded(bus))
    {
        SDCBUS_LockUnit(bus);
        result = !!(unit->sdcu_Flags & AF_Card_Active) &&
                    !(unit->sdcu_Flags & AF_Card_Configuring);
        SDCBUS_UnlockUnit(bus);
        SDCBUS_EndTransaction(bus);
        return result;
    }
    SDCBUS_LockUnit(bus);
    if (unit->sdcu_Flags & AF_Card_Configuring)
    {
        SDCBUS_UnlockUnit(bus);
        SDCBUS_EndTransaction(bus);
        return FALSE;
    }
    unit->sdcu_Flags = (unit->sdcu_Flags & ~AF_Card_Active) | AF_Card_Configuring;
    sdcard_StageUnit(unit, &candidate);
    SDCBUS_UnlockUnit(bus);

    if (bus->sdcb_RecoveryPrepare(bus) &&
        FNAME_SDCBUS(IdentifyCardLocked)(bus, &card) && sdcard_SameCard(unit, &card))
    {
        candidate.sdcu_CardRCA = card.rca;
        candidate.sdcu_CardPower = card.power;
        /* Keep the mounted unit's write policy, recompute negotiated bits. */
        candidate.sdcu_Flags &= ~(AF_Card_4bitData | AF_Card_HighSpeed |
                                  AF_Card_HighSpeed52);
        result = sdcard_StartUnitLocked(&candidate);
    }
    /* Backend must refuse success if the host faulted or changed generation
     * during configuration. Failure cleanup is called even after prepare fails. */
    result = bus->sdcb_RecoveryFinish(bus, result) && result;
    sdcard_PublishUnit(unit, &candidate, result);
    SDCBUS_EndTransaction(bus);
    return result;
}

ULONG FNAME_SDCUNIT(WaitStatus)(ULONG timeout, struct sdcard_Unit *sdcUnit)
{
    struct TagItem sdcStatusTags[] =
    {
        {SDCARD_TAG_CMD,         MMC_CMD_SEND_STATUS},
        {SDCARD_TAG_ARG,         sdcUnit->sdcu_CardRCA << 16},
        {SDCARD_TAG_RSPTYPE,     MMC_RSP_R1},
        {SDCARD_TAG_RSP,         0},
        {TAG_DONE,               0}
    };

    ULONG retryreq = 5;

    DFUNCS(bug("[SDCard%02ld] %s()\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__));

    while (timeout != 0) {
        --timeout;
        sdcStatusTags[3].ti_Data = 0;
        if ((SDCBUS_SendCmd(sdcStatusTags, sdcUnit->sdcu_Bus) != -1) && (SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT|SDHCI_PS_DATA_INHIBIT, 1000, sdcUnit->sdcu_Bus) != -1) )
        {
            if (sdcStatusTags[3].ti_Data & MMC_STATUS_MASK) {
                bug("[SDCard%02ld] %s: Status [Error = %08x]\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__, sdcStatusTags[3].ti_Data);
                return -1;
            }
            if ((sdcStatusTags[3].ti_Data & MMC_STATUS_RDY_FOR_DATA) &&
                (sdcStatusTags[3].ti_Data & MMC_STATUS_STATE_MASK) == MMC_STATUS_STATE_TRAN)
                return 0;
        } else if (--retryreq == 0)
                return -1;

        sdcard_Udelay(1000);
    }

    bug("[SDCard%02ld] %s: Timeout\n", sdcUnit->sdcu_UnitNum, __PRETTY_FUNCTION__);
    return (ULONG)-1;
}

/* Bus Functions */

/* Synchronous identification does not allocate or publish a unit. Responses and
 * geometry are staged locally; failure leaves the caller's snapshot untouched.
 * Command bounds belong to the backend, with at most 1000 ACMD41 attempts. */
static BOOL sdcard_IdentifyCommand(struct sdcard_Bus *bus, ULONG command,
                                  ULONG argument, ULONG response_type,
                                  IPTR response, ULONG *short_response)
{
    struct TagItem tags[] =
    {
        {SDCARD_TAG_CMD, command},
        {SDCARD_TAG_ARG, argument},
        {SDCARD_TAG_RSPTYPE, response_type},
        {SDCARD_TAG_RSP, response},
        {TAG_DONE, 0}
    };
    if (SDCBUS_SendCmd(tags, bus) == (ULONG)-1 ||
        SDCBUS_WaitCmd(SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DATA_INHIBIT,
                       1000, bus) == (ULONG)-1)
        return FALSE;
    if (short_response)
        *short_response = tags[3].ti_Data;
    return TRUE;
}

static BOOL sdcard_DecodeCardInfo(struct sdcard_CardInfo *card)
{
    ULONG structure = FNAME_SDCBUS(Rsp136Unpack)(card->csd, 126, 2);
    UQUAD cylinders;
    if (structure == 0)
    {
        ULONG read_len = FNAME_SDCBUS(Rsp136Unpack)(card->csd, 80, 4);
        /* The exposed geometry uses 512-byte sectors and an 8-bit sector count. */
        if ((card->flags & AF_Card_HighCapacity) || read_len < 9 || read_len > 15)
            return FALSE;
        card->block_words = (1UL << read_len) >> 2;
        card->sectors = 1UL << (read_len - 9);
        cylinders = (UQUAD)(1 + FNAME_SDCBUS(Rsp136Unpack)(card->csd, 62, 12)) <<
            (FNAME_SDCBUS(Rsp136Unpack)(card->csd, 47, 3) + 2);
    }
    else if (structure == 1)
    {
        if (!(card->flags & AF_Card_HighCapacity))
            return FALSE;
        card->block_words = 256;
        card->sectors = 2;
        cylinders = (UQUAD)(1 + FNAME_SDCBUS(Rsp136Unpack)(card->csd, 48, 22)) * 512;
    }
    else
        return FALSE;
    /* The cylinder-count field cannot represent a 2^32-cylinder count. */
    if (!cylinders || cylinders > 0xFFFFFFFFULL)
        return FALSE;
    card->heads = 1;
    card->cylinders = (ULONG)cylinders;
    card->capacity = cylinders * card->sectors;
    return TRUE;
}

BOOL FNAME_SDCBUS(IdentifyCardLocked)(struct sdcard_Bus *bus,
                                     struct sdcard_CardInfo *result)
{
    struct sdcard_CardInfo card;
    ULONG response = 0, condition, attempts;
    BOOL version2 = FALSE;
    if (!bus || !result)
        return FALSE;
    memset(&card, 0, sizeof(card));
    if (!sdcard_IdentifyCommand(bus, MMC_CMD_GO_IDLE_STATE, 0,
                                MMC_RSP_NONE, 0, NULL))
        return FALSE;
    sdcard_Udelay(2000);
    condition = (((bus->sdcb_Power & 0xFF8000) != 0) << 8) | 0xAA;
    if (sdcard_IdentifyCommand(bus, SD_CMD_SEND_IF_COND, condition,
                               MMC_RSP_R7, 0, &response))
    {
        if ((response & 0xFFF) != condition)
            return FALSE;
        version2 = TRUE;
    }
    /* An absent CMD8 response keeps the existing legacy-SD identification path.
     * A backend fault remains latched and refuses the following CMD55. */
    for (attempts = 0; attempts < 1000; ++attempts)
    {
        if (!sdcard_IdentifyCommand(bus, MMC_CMD_APP_CMD, 0, MMC_RSP_R1,
                                    0, &response) ||
            (response & MMC_STATUS_MASK))
            return FALSE;
        if (!sdcard_IdentifyCommand(bus, SD_CMD_APP_SEND_OP_COND,
                                    (bus->sdcb_Power & 0xFF8000) |
                                    (version2 ? OCR_HCS : 0),
                                    MMC_RSP_R3, 0, &response))
            return FALSE;
        /* OCR_BUSY's shared legacy macro uses a signed high-bit shift. */
        if (response & (1UL << 31))
            break;
        sdcard_Udelay(1000);
    }
    if (attempts == 1000)
        return FALSE;
    card.power = bus->sdcb_Power & response & 0xFF8000;
    if (!card.power)
        return FALSE;
    if (response & OCR_HCS)
    {
        if (!version2)
            return FALSE;
        card.flags |= AF_Card_HighCapacity;
    }
    SDCBUS_SetPowerLevel(card.power, TRUE, bus);
    sdcard_Udelay(2000);
    if (!sdcard_IdentifyCommand(bus, MMC_CMD_ALL_SEND_CID, 0, MMC_RSP_R2,
                                (IPTR)card.cid, NULL) ||
        !sdcard_IdentifyCommand(bus, SD_CMD_SEND_RELATIVE_ADDR, 0, MMC_RSP_R6,
                                0, &response))
        return FALSE;
    card.rca = response >> 16;
    if (!card.rca || (response & 0xE000UL))
        return FALSE;
    if (!sdcard_IdentifyCommand(bus, MMC_CMD_SEND_CSD, card.rca << 16,
                                MMC_RSP_R2, (IPTR)card.csd, NULL) ||
        !sdcard_DecodeCardInfo(&card))
        return FALSE;
    *result = card;
    return TRUE;
}

static BOOL sdcard_RegisterUnitLocked(struct sdcard_Bus *bus)
{
    LIBBASETYPEPTR LIBBASE = bus->sdcb_DeviceBase;
    struct sdcard_CardInfo card;
    struct sdcard_Unit *unit;
    struct DeviceNode *devnode;
    IPTR pp[24] = {0};
    ULONG ordinal = bus->sdcb_BusUnits->sdcbu_UnitCnt;

    if (ordinal >= bus->sdcb_BusUnits->sdcbu_UnitMax ||
        !FNAME_SDCBUS(IdentifyCardLocked)(bus, &card))
        return FALSE;
    unit = AllocVecPooled(LIBBASE->sdcard_MemPool, sizeof(*unit));
    if (!unit)
        return FALSE;
    memset(unit, 0, sizeof(*unit));
    unit->sdcu_Bus = bus;
    NEWLIST(&unit->sdcu_SoftList);
    unit->sdcu_UnitNum = ordinal + bus->sdcb_BusUnits->sdcbu_UnitBase;
    unit->sdcu_CardRCA = card.rca;
    unit->sdcu_CardPower = card.power;
    unit->sdcu_Flags = card.flags;
#if defined(SDHCI_READONLY)
    unit->sdcu_Flags |= AF_Card_WriteProtect;
#endif
    unit->sdcu_Read32 = FNAME_SDCIO(ReadSector32);
    unit->sdcu_Write32 = FNAME_SDCIO(WriteSector32);
    if (card.flags & AF_Card_HighCapacity)
    {
        unit->sdcu_Read64 = FNAME_SDCIO(ReadSector64);
        unit->sdcu_Write64 = FNAME_SDCIO(WriteSector64);
    }
    unit->sdcu_Cylinders = card.cylinders;
    unit->sdcu_Heads = card.heads;
    unit->sdcu_Sectors = card.sectors;
    unit->sdcu_Capacity = card.capacity;
    unit->sdcu_Eject = FNAME_SDCIO(Eject);
    memcpy(unit->sdcu_CardCID, card.cid, sizeof(card.cid));
    memcpy(unit->sdcu_CardCSD, card.csd, sizeof(card.csd));

    pp[0] = (IPTR)str_mmc0;
    pp[1] = (IPTR)MOD_NAME_STRING;
    pp[2] = unit->sdcu_UnitNum;
    pp[DE_TABLESIZE + 4] = DE_BOOTBLOCKS;
    pp[DE_SIZEBLOCK + 4] = card.block_words;
    pp[DE_SECSPERBLOCK + 4] = card.sectors;
    pp[DE_HIGHCYL + 4] = card.cylinders - 1;
    pp[DE_NUMHEADS + 4] = card.heads;
    pp[DE_BLKSPERTRACK + 4] = 1;
    pp[DE_RESERVEDBLKS + 4] = 2;
    pp[DE_LOWCYL + 4] = 0;
    pp[DE_NUMBUFFERS + 4] = 10;
    pp[DE_BUFMEMTYPE + 4] = MEMF_PUBLIC | MEMF_31BIT;
    pp[DE_MAXTRANSFER + 4] = 0x00200000;
    pp[DE_MASK + 4] = 0x7FFFFFFE;
    pp[DE_BOOTBLOCKS + 4] = 2;
    devnode = MakeDosNode(pp);
    if (!devnode)
    {
        FreeVecPooled(LIBBASE->sdcard_MemPool, unit);
        return FALSE;
    }
    /* Publish only after complete identity/geometry and boot-node construction.
     * Re-enumeration must use the snapshot, never this allocation/publication. */
    SDCBUS_LockUnit(bus);
    (&bus->sdcb_BusUnits->sdcbu_Units)[ordinal] = unit;
    bus->sdcb_BusUnits->sdcbu_UnitCnt = ordinal + 1;
    SDCBUS_UnlockUnit(bus);
    bus->sdcb_BusFlags |= AF_Bus_MediaPresent;
    bug("[SDBus%02u] %b: [%ldMB Capacity]\n", bus->sdcb_BusNum,
        devnode->dn_Name, (ULONG)((card.capacity >> 11) & 0xFFFFFFFFUL));
    AddBootNode(0, 0, devnode, NULL);
    return TRUE;
}

BOOL FNAME_SDCBUS(RegisterUnit)(struct sdcard_Bus *bus)
{
    BOOL result;

    if (!SDCBUS_BeginTransaction(bus, TRUE))
        return FALSE;

    result = sdcard_RegisterUnitLocked(bus);
    SDCBUS_EndTransaction(bus);
    return result;
}

void FNAME_SDCBUS(ReleaseUnit)(struct sdcard_Bus *bus)
{
    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
}

void FNAME_SDCBUS(SoftReset)(UBYTE mask, struct sdcard_Bus *bus)
{
    ULONG timeout = 100;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    bus->sdcb_IOWriteByte(SDHCI_RESET, mask, bus);
    while (bus->sdcb_IOReadByte(SDHCI_RESET, bus) & mask) {
        if (timeout == 0) {
            bug("[SDBus%02u] %s: Timeout\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);
            break;
        }
        sdcard_Udelay(1000);
        timeout--;
    }
}

void FNAME_SDCBUS(SetClock)(ULONG speed, struct sdcard_Bus *bus)
{
    ULONG       sdcClkDiv, timeout;
    UWORD       sdcClkCtrlCur, sdcClkCtrl;
    BOOL        sdcReprogram = FALSE;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    sdcClkCtrlCur = bus->sdcb_IOReadWord(SDHCI_CLOCK_CONTROL, bus);

    sdcClkDiv = FNAME_SDCBUS(GetClockDiv)(speed, bus);

    /* What the card is really clocked at, which is the base rate divided down
       and only as trustworthy as the base rate the controller reported. */
    DINIT(bug("[SDBus%02u] clock: asked %u Hz, base %u Hz, div %u -> %u Hz [HCTRL 0x%02x]\n",
        bus->sdcb_BusNum, speed, bus->sdcb_ClockMax, sdcClkDiv,
        sdcClkDiv ? (bus->sdcb_ClockMax / (sdcClkDiv * 2)) : bus->sdcb_ClockMax,
        bus->sdcb_IOReadByte(SDHCI_HOST_CONTROL, bus)));

    sdcClkCtrl = (sdcClkDiv & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
    sdcClkCtrl |= ((sdcClkDiv & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN) << SDHCI_DIVIDER_HI_SHIFT;

    /*
     * Past 25MHz the controller has to sample with high speed timing, and the
     * bit that selects it may only be touched while the card clock is stopped.
     */
    if (bus->sdcb_Capabilities & SDHCI_CAN_DO_HISPD)
    {
        UBYTE sdcCtrlCur = bus->sdcb_IOReadByte(SDHCI_HOST_CONTROL, bus);
        UBYTE sdcCtrlNew = (speed > 25000000) ? (sdcCtrlCur | SDHCI_HCTRL_HISPD)
                                              : (sdcCtrlCur & ~SDHCI_HCTRL_HISPD);

        if (sdcCtrlNew != sdcCtrlCur)
        {
            D(bug("[SDBus%02u] %s: %s high speed timing\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__,
                  (sdcCtrlNew & SDHCI_HCTRL_HISPD) ? "Enabling" : "Disabling"));

            bus->sdcb_IOWriteWord(SDHCI_CLOCK_CONTROL, 0, bus);
            bus->sdcb_IOWriteByte(SDHCI_HOST_CONTROL, sdcCtrlNew, bus);
            sdcReprogram = TRUE;
        }
    }

    if (sdcReprogram ||
        (sdcClkCtrl != (sdcClkCtrlCur & ~(SDHCI_CLOCK_INT_EN|SDHCI_CLOCK_INT_STABLE|SDHCI_CLOCK_CARD_EN))))
    {
        bus->sdcb_IOWriteWord(SDHCI_CLOCK_CONTROL, 0, bus);

        D(bug("[SDBus%02u] %s: Changing CLOCK_CONTROL [0x%04x -> 0x%04x] (div %d)\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, sdcClkCtrlCur & ~(SDHCI_CLOCK_INT_EN|SDHCI_CLOCK_INT_STABLE|SDHCI_CLOCK_CARD_EN), sdcClkCtrl, sdcClkDiv));

        bus->sdcb_IOWriteWord(SDHCI_CLOCK_CONTROL, (sdcClkCtrl | SDHCI_CLOCK_INT_EN), bus);

        timeout = 20;
        while (!((sdcClkCtrl = bus->sdcb_IOReadWord(SDHCI_CLOCK_CONTROL, bus)) & SDHCI_CLOCK_INT_STABLE)) {
            if (timeout == 0) {
                bug("[SDBus%02u] %s: SDHCI Clock failed to stabilise\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);
                break;
            }
            sdcard_Udelay(1000);
            timeout --;
        }
    }
    else
        if (sdcClkCtrlCur & SDHCI_CLOCK_CARD_EN)
            return;

    D(bug("[SDBus%02u] %s: Enabling clock...\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
    sdcClkCtrl |= SDHCI_CLOCK_CARD_EN;
    bus->sdcb_IOWriteWord(SDHCI_CLOCK_CONTROL, sdcClkCtrl, bus);
}

void FNAME_SDCBUS(SetPowerLevel)(ULONG supportedlvls, BOOL lowest, struct sdcard_Bus *bus)
{
    UBYTE sdcReg = 0, lvlCur;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    if (lowest)
    {
        if (supportedlvls & MMC_VDD_165_195)
            sdcReg = SDHCI_POWER_180;
        else if (supportedlvls & (MMC_VDD_290_300|MMC_VDD_300_310))
            sdcReg = SDHCI_POWER_300;
        else if (supportedlvls & (MMC_VDD_320_330|MMC_VDD_330_340))
            sdcReg = SDHCI_POWER_330;
    }
    else
    {
        if (supportedlvls & (MMC_VDD_320_330|MMC_VDD_330_340))
            sdcReg = SDHCI_POWER_330;
        else if (supportedlvls & (MMC_VDD_290_300|MMC_VDD_300_310))
            sdcReg = SDHCI_POWER_300;
        else if (supportedlvls & MMC_VDD_165_195)
            sdcReg = SDHCI_POWER_180;
    }

    lvlCur = bus->sdcb_IOReadByte(SDHCI_POWER_CONTROL, bus);
    if ((lvlCur & ~SDHCI_POWER_ON) != sdcReg)
    {
        D(bug("[SDBus%02u] %s: Changing Power Lvl [0x%x -> 0x%x]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, lvlCur & ~SDHCI_POWER_ON, sdcReg));
        bus->sdcb_IOWriteByte(SDHCI_POWER_CONTROL, sdcReg, bus);
        sdcReg |= SDHCI_POWER_ON;
        bus->sdcb_IOWriteByte(SDHCI_POWER_CONTROL, sdcReg, bus);
    }
    else
    {
        if (!(lvlCur & SDHCI_POWER_ON))
        {
            D(bug("[SDBus%02u] %s: Enabling Power Lvl [0x%x]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, lvlCur));
            lvlCur |= SDHCI_POWER_ON;
            bus->sdcb_IOWriteByte(SDHCI_POWER_CONTROL, lvlCur, bus);
        }
    }
}

#undef DFUNCS
#define DFUNCS(x) /*x*/
#undef DTRANS
#define DTRANS(x) /*x*/
#undef D
#define D(x) /*x*/

/********** ADMA2 **************/

/*
 * The address the controller puts on the bus. The EMMC2 block is an ARM side
 * master and sees plain physical addresses, unlike the VideoCore DMA engine
 * the SDHOST driver drives, which needs the 0xC0000000 alias. A controller
 * that needs a different view should have it applied here and nowhere else.
 */
static IPTR FNAME_SDCBUS(ADMAPhys)(struct sdcard_Bus *bus, APTR virt)
{
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;

    return (IPTR)KrnVirtualToPhysical(virt);
}

BOOL FNAME_SDCBUS(ADMAAlloc)(struct sdcard_Bus *bus)
{
    APTR desc, bounce;

    if (!(bus->sdcb_Capabilities & SDHCI_CAN_DO_ADMA2))
    {
        bug("[SDBus%02u] controller has no ADMA2, transfers will use PIO\n", bus->sdcb_BusNum);
        return FALSE;
    }

    /*
     * Both live below 2GB because the controller cannot address more, and
     * both are padded out to whole cache lines: cache maintenance works a
     * line at a time and would otherwise reach into whatever shares them.
     */
    desc = AllocMem((ADMA2_MAX_DESC * sizeof(struct sdcard_ADMADesc)) + 64,
                    MEMF_PUBLIC | MEMF_CLEAR | MEMF_31BIT);
    bounce = AllocMem(ADMA2_BOUNCE_SIZE + 64, MEMF_PUBLIC | MEMF_31BIT);

    if (!desc || !bounce)
    {
        bug("[SDBus%02u] no memory for ADMA2, transfers will use PIO\n", bus->sdcb_BusNum);
        if (desc)
            FreeMem(desc, (ADMA2_MAX_DESC * sizeof(struct sdcard_ADMADesc)) + 64);
        if (bounce)
            FreeMem(bounce, ADMA2_BOUNCE_SIZE + 64);
        return FALSE;
    }

    bus->sdcb_ADMADesc = (APTR)(((IPTR)desc + 63) & ~63);
    bus->sdcb_ADMABounce = (APTR)(((IPTR)bounce + 63) & ~63);
    bus->sdcb_ADMABounceSize = ADMA2_BOUNCE_SIZE;
    bus->sdcb_BusFlags |= AF_Bus_DMA;

    D(bug("[SDBus%02u] ADMA2 descriptors @ 0x%p, bounce @ 0x%p\n", bus->sdcb_BusNum,
          bus->sdcb_ADMADesc, bus->sdcb_ADMABounce));

    return TRUE;
}

/*
 * Describe the caller's buffer to the controller. Returns FALSE to leave the
 * transfer to PIO, which is always a valid thing to fall back on.
 */
static BOOL FNAME_SDCBUS(ADMASetup)(struct sdcard_Bus *bus, APTR data, ULONG len, BOOL isWrite)
{
    struct sdcard_ADMADesc      *desc = bus->sdcb_ADMADesc;
    UBYTE                       *pos;
    ULONG                       remaining;
    UBYTE                       sdcHostCtrl;
    UWORD                       n;
    BOOL                        bounced;

    if (!(bus->sdcb_BusFlags & AF_Bus_DMA) || (data == NULL) || (len == 0))
        return FALSE;

    bus->sdcb_BusFlags &= ~AF_Bus_DMABounced;

    /*
     * Cache maintenance works a line at a time, so a buffer that starts or
     * ends mid line would drag its neighbours along. Anything unaligned goes
     * through the bounce buffer instead.
     */
    bounced = (((IPTR)data | (IPTR)len) & 63) ? TRUE : FALSE;

    bus->sdcb_ADMAData = data;
    bus->sdcb_ADMALen = len;

retry:
    if (bounced && (len > bus->sdcb_ADMABounceSize))
    {
        DTRANS(bug("[SDBus%02u] %s: transfer larger than the bounce buffer (%u)\n",
                   bus->sdcb_BusNum, __PRETTY_FUNCTION__, len));
        return FALSE;
    }

    n = 0;
    pos = bounced ? bus->sdcb_ADMABounce : data;

    if (bounced && isWrite)
        CopyMem(data, pos, len);

    /*
     * Walk the buffer a page at a time, merging pages that turn out to be
     * physically adjacent. On a flat mapping that collapses to one descriptor,
     * and where it does not, ADMA2 handles the scatter for us.
     */
    remaining = len;
    while (remaining > 0)
    {
        IPTR    phys = FNAME_SDCBUS(ADMAPhys)(bus, pos);
        ULONG   run = 4096 - ((ULONG)((IPTR)pos & 4095));

        if (run > remaining)
            run = remaining;

        while ((run < remaining) && (run < ADMA2_MAX_XFER) &&
               (FNAME_SDCBUS(ADMAPhys)(bus, pos + run) == (phys + run)))
        {
            ULONG more = ((remaining - run) > 4096) ? 4096 : (remaining - run);

            if ((run + more) > ADMA2_MAX_XFER)
                more = ADMA2_MAX_XFER - run;

            run += more;
        }

        /* Out of the controller's reach, or too fragmented to describe. */
        if ((n >= ADMA2_MAX_DESC) || (((UQUAD)phys + run) > 0x100000000ULL))
        {
            DTRANS(bug("[SDBus%02u] %s: buffer needs PIO (desc %d, phys 0x%p)\n",
                       bus->sdcb_BusNum, __PRETTY_FUNCTION__, n, (APTR)phys));

            /*
             * The bounce buffer is contiguous and within reach, so describing
             * it can only fail on size. Start over through it rather than
             * hand a whole chunk to PIO.
             */
            if (!bounced)
            {
                bounced = TRUE;
                goto retry;
            }

            return FALSE;
        }

        desc[n].ad_Attr    = AROS_WORD2LE(ADMA2_ATTR_VALID | ADMA2_ATTR_ACT_TRAN);
        desc[n].ad_Length  = AROS_WORD2LE((UWORD)run);
        desc[n].ad_Address = AROS_LONG2LE((ULONG)phys);
        n++;

        pos += run;
        remaining -= run;
    }

    /* The last one closes the list and raises the completion interrupt. */
    desc[n - 1].ad_Attr = AROS_WORD2LE(ADMA2_ATTR_VALID | ADMA2_ATTR_END |
                                       ADMA2_ATTR_INT | ADMA2_ATTR_ACT_TRAN);

    /*
     * Push the descriptors and, for a write, the data out of the cache so the
     * controller reads what we just wrote. For a read this also drops any
     * dirty lines that would otherwise be written back over the result.
     */
    if (bounced)
        bus->sdcb_BusFlags |= AF_Bus_DMABounced;

    CacheClearE(bus->sdcb_ADMADesc, n * sizeof(struct sdcard_ADMADesc), CACRF_ClearD);
    CacheClearE(bounced ? bus->sdcb_ADMABounce : data, len, CACRF_ClearD);

    bus->sdcb_IOWriteLong(SDHCI_ADMA_ADDRESS,
        (ULONG)FNAME_SDCBUS(ADMAPhys)(bus, bus->sdcb_ADMADesc), bus);

    sdcHostCtrl = bus->sdcb_IOReadByte(SDHCI_HOST_CONTROL, bus);
    sdcHostCtrl = (sdcHostCtrl & ~SDHCI_HCTRL_DMA_MASK) | SDHCI_HCTRL_ADMA32;
    bus->sdcb_IOWriteByte(SDHCI_HOST_CONTROL, sdcHostCtrl, bus);

    bus->sdcb_BusFlags |= AF_Bus_DMAActive;

    DTRANS(bug("[SDBus%02u] %s: %d descriptor(s) for %d bytes%s\n", bus->sdcb_BusNum,
               __PRETTY_FUNCTION__, n, len,
               (bus->sdcb_BusFlags & AF_Bus_DMABounced) ? " (bounced)" : ""));

    return TRUE;
}

/*
 * Read the same sectors twice, once each way, and compare. Getting the address
 * translation wrong would otherwise show up as quietly corrupted data rather
 * than as an error, so prove it once at startup and fall back to PIO for good
 * if the two disagree.
 */
void FNAME_SDCBUS(ADMAVerify)(struct sdcard_Unit *sdcUnit)
{
    struct sdcard_Bus   *bus = sdcUnit->sdcu_Bus;
    ULONG               len = 4 << bus->sdcb_SectorShift;
    UBYTE               *viaDMA, *viaPIO;
    ULONG               act = 0;
    BYTE                err;

    if (!(bus->sdcb_BusFlags & AF_Bus_DMA))
        return;

    viaDMA = AllocMem(len, MEMF_PUBLIC | MEMF_CLEAR | MEMF_31BIT);
    viaPIO = AllocMem(len, MEMF_PUBLIC | MEMF_CLEAR | MEMF_31BIT);

    if (!viaDMA || !viaPIO)
    {
        if (viaDMA)
            FreeMem(viaDMA, len);
        if (viaPIO)
            FreeMem(viaPIO, len);
        return;
    }

    err = (sdcUnit->sdcu_Flags & AF_Card_HighCapacity)
            ? sdcUnit->sdcu_Read64(sdcUnit, 0, 4, viaDMA, &act)
            : sdcUnit->sdcu_Read32(sdcUnit, 0, 4, viaDMA, &act);

    bus->sdcb_BusFlags &= ~AF_Bus_DMA;

    if (err == 0)
        err = (sdcUnit->sdcu_Flags & AF_Card_HighCapacity)
                ? sdcUnit->sdcu_Read64(sdcUnit, 0, 4, viaPIO, &act)
                : sdcUnit->sdcu_Read32(sdcUnit, 0, 4, viaPIO, &act);

    if ((err == 0) && (memcmp(viaDMA, viaPIO, len) == 0))
    {
        bus->sdcb_BusFlags |= AF_Bus_DMA;
        bug("[SDBus%02u] ADMA2 verified against PIO, using DMA for transfers\n", bus->sdcb_BusNum);
    }
    else
    {
        bug("[SDBus%02u] ADMA2 self test FAILED (err %ld) - staying on PIO\n",
            bus->sdcb_BusNum, (LONG)err);
    }

    FreeMem(viaDMA, len);
    FreeMem(viaPIO, len);
}

ULONG FNAME_SDCBUS(SendCmd)(struct TagItem *CmdTags, struct sdcard_Bus *bus)
{
#if defined(__AROSEXEC_SMP__)
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;
#endif
    UWORD sdCommand      = (UWORD)GetTagData(SDCARD_TAG_CMD, 0, CmdTags);
    ULONG sdArg          = GetTagData(SDCARD_TAG_ARG, 0, CmdTags);
    ULONG sdResponseType = GetTagData(SDCARD_TAG_RSPTYPE, MMC_RSP_NONE, CmdTags);
    ULONG sdDataLen = 0, sdDataFlags;

    UWORD sdcTransMode = 0, sdCommandFlags;
    ULONG sdcInhibitMask = SDHCI_PS_CMD_INHIBIT;
    ULONG timeout = 10;
    ULONG retVal = 0;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    if ((sdDataLen = GetTagData(SDCARD_TAG_DATALEN, 0, CmdTags)) > 0)
    {
        sdDataFlags = GetTagData(SDCARD_TAG_DATAFLAGS, 0, CmdTags);
    };

D(bug("SendCmd(%d,%d)\n", sdCommand, sdDataLen));

    /* Dont wait for DATA inihibit for stop commands */
    if (sdCommand != MMC_CMD_STOP_TRANSMISSION)
        sdcInhibitMask |= SDHCI_PS_DATA_INHIBIT;

    while (bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus) & sdcInhibitMask) {
        if (timeout == 0) {
            bug("[SDBus%02u] %s: Controller failed to release inhibited bit(s).\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);
            bug("[SDBus%02u] %s:  failed bit(s) = %08x\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus) & sdcInhibitMask);

            bug("[SDBus%02u] %s: Reseting SDHCI CMD/DATA\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);

            FNAME_SDCBUS(SoftReset)(SDHCI_RESET_CMD, bus);
            FNAME_SDCBUS(SoftReset)(SDHCI_RESET_DATA, bus);

            return -1;
        }
        sdcard_Udelay(1000);
        timeout--;
    }

    if (!(sdResponseType & MMC_RSP_PRESENT))
        sdCommandFlags = SDHCI_CMD_RESP_NONE;
    else if (sdResponseType & MMC_RSP_136)
        sdCommandFlags = SDHCI_CMD_RESP_LONG;
    else if (sdResponseType & MMC_RSP_BUSY)
        sdCommandFlags = SDHCI_CMD_RESP_SHORT_BUSY;
    else
        sdCommandFlags = SDHCI_CMD_RESP_SHORT;

    if (sdResponseType & MMC_RSP_CRC)
        sdCommandFlags |= SDHCI_CMD_CRC;
    if (sdResponseType & MMC_RSP_OPCODE)
        sdCommandFlags |= SDHCI_CMD_INDEX;
    if (sdDataLen > 0)
        sdCommandFlags |= SDHCI_CMD_DATA;

    if (sdDataLen > 0) {
        sdcTransMode = SDHCI_TRANSMOD_BLK_CNT_EN;
        DTRANS(bug("[SDBus%02u] %s: Configuring Data Transfer\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

        if (bus->sdcb_LEDCtrl)
            bus->sdcb_LEDCtrl(LED_ON);

        bus->sdcb_IOWriteByte(SDHCI_TIMEOUT_CONTROL, SDHCI_TIMEOUT_MAX, bus);

        bus->sdcb_IOWriteWord(SDHCI_BLOCK_SIZE, ((1 << 16) | ((sdDataLen > (1 << bus->sdcb_SectorShift)) ? (1 << bus->sdcb_SectorShift) : sdDataLen)), bus);
        if ((sdDataLen >> bus->sdcb_SectorShift) > 1)
        {
            sdcTransMode |= SDHCI_TRANSMOD_MULTI;
            bus->sdcb_IOWriteWord(SDHCI_BLOCK_COUNT, sdDataLen >> bus->sdcb_SectorShift, bus);
        }
        else
        {
            bus->sdcb_IOWriteWord(SDHCI_BLOCK_COUNT, 1, bus);
        }

        if (sdDataFlags == MMC_DATA_READ)
            sdcTransMode |= SDHCI_TRANSMOD_READ;

        /*
         * Let the controller fetch the data itself where it can. PIO reads a
         * block at a time from inside the interrupt handler, which holds the
         * machine for as long as the transfer lasts.
         */
        bus->sdcb_BusFlags &= ~AF_Bus_DMAActive;
        if (FNAME_SDCBUS(ADMASetup)(bus, (APTR)GetTagData(SDCARD_TAG_DATA, 0, CmdTags),
                                    sdDataLen, (sdDataFlags != MMC_DATA_READ)))
        {
            sdcTransMode |= SDHCI_TRANSMOD_DMA;
        }

        if (!(bus->sdcb_Quirks & AF_Quirk_AtomicTMAndCMD))
        {
            bus->sdcb_IOWriteWord(SDHCI_TRANSFER_MODE, sdcTransMode, bus);
        }

        DTRANS(bug("[SDBus%02u] %s: Mode %08x [%d x %dBytes]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, sdcTransMode, (((sdDataLen >> bus->sdcb_SectorShift) > 0) ? (sdDataLen >> bus->sdcb_SectorShift) : 1), ((sdDataLen > (1 << bus->sdcb_SectorShift)) ? (1 << bus->sdcb_SectorShift) : sdDataLen)));
    }

#if defined(__AROSEXEC_SMP__)
    KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
    bus->sdcb_RespListener = CmdTags;
    if (sdDataLen > 0)
        bus->sdcb_DataListener = CmdTags;
#if defined(__AROSEXEC_SMP__)
    KrnSpinUnLock(&bus->sdcb_Lock);
#endif

    /*
     * From here until the command completes, any error the controller reports
     * belongs to it. sdcb_BusStatus cannot carry that: it is overwritten by
     * whichever interrupt happens to arrive next, so an error can be erased
     * before the waiting task ever looks at it, or a stale one inherited by a
     * command that never failed.
     */
    bus->sdcb_CmdError = 0;

    SetSignal(0, 1L << bus->sdcb_CommandSig);

    bus->sdcb_IOWriteLong(SDHCI_ARGUMENT, sdArg, bus);
    if ((bus->sdcb_Quirks & AF_Quirk_AtomicTMAndCMD) && (sdcTransMode))
    {
        bus->sdcb_IOWriteLong(SDHCI_TRANSFER_MODE, (SDHCI_MAKE_CMD(sdCommand, sdCommandFlags) << 16) | sdcTransMode, bus);
    }
    else
    {
        bus->sdcb_IOWriteWord(SDHCI_COMMAND, SDHCI_MAKE_CMD(sdCommand, sdCommandFlags), bus);
    }

//bug("Delay01Delay02\n");

    D(bug("[SDBus%02u] %s: CMD %02d Sent\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, sdCommand));

    return retVal;
}

#undef DFUNCS
#define DFUNCS(x) /* */
#undef DTRANS
#define DTRANS(x) /* */
#undef D
#define D(x) /* */


ULONG FNAME_SDCBUS(FinishCmd)(struct TagItem *CmdTags, struct sdcard_Bus *bus)
{
    struct TagItem *Response = NULL;

    D(UWORD     sdCommand       = (UWORD)GetTagData(SDCARD_TAG_CMD, 0, CmdTags));
    ULONG       sdResponseType  = GetTagData(SDCARD_TAG_RSPTYPE, MMC_RSP_NONE, CmdTags);
    ULONG       retVal          = 0;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    if (sdResponseType != MMC_RSP_NONE)
    {
        if ((Response = FindTagItem(SDCARD_TAG_RSP, CmdTags)) != NULL)
        {
            D(bug("[SDBus%02u] %s: Reading CMD %02d Response ", bus->sdcb_BusNum, __PRETTY_FUNCTION__, sdCommand));
            if (sdResponseType & MMC_RSP_136)
            {
                D(bug("[136bit]\n"));
                if (Response->ti_Data)
                {
                    ULONG i;
                    for (i = 0; i < 4; i ++)
                    {
                        ((ULONG *)Response->ti_Data)[i] = bus->sdcb_IOReadLong(SDHCI_RESPONSE + (3 - i) * 4, bus) << 8;
                        if (i != 3)
                            ((ULONG *)Response->ti_Data)[i] |= bus->sdcb_IOReadByte(SDHCI_RESPONSE + (3 - i) * 4 - 1, bus);
                    }
                    D(bug("[SDBus%02u] %s:   %08x%08x%08x%08x\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, ((ULONG *)Response->ti_Data)[0], ((ULONG *)Response->ti_Data)[1], ((ULONG *)Response->ti_Data)[2], ((ULONG *)Response->ti_Data)[3]));
                }
            }
            else
            {
                Response->ti_Data = bus->sdcb_IOReadLong(SDHCI_RESPONSE, bus);
                D(bug("[= %08x]\n", Response->ti_Data));
            }
        }
    }

    return retVal;
}

ULONG FNAME_SDCBUS(FinishData)(struct TagItem *DataTags, struct sdcard_Bus *bus)
{
    DTRANS(UWORD        sdCommand = (UWORD)GetTagData(SDCARD_TAG_CMD, 0, DataTags));
    ULONG               sdcStateMask, sdCommandMask,
                        sdDataMode = MMC_DATA_READ, sdDataLen, sdcReg = 0;
    IPTR                sdData;
    struct TagItem      *sdDataLenTag = NULL;
    ULONG               waitStart = 0;
    BOOL                waiting = FALSE;
    ULONG               retVal = 0;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    if ((sdData = GetTagData(SDCARD_TAG_DATA, 0, DataTags)) != 0)
    {
        if ((sdDataLenTag = FindTagItem(SDCARD_TAG_DATALEN, DataTags)) == NULL)
            sdData = 0;
        else
        {
            sdDataLen = sdDataLenTag->ti_Data;
            if ((sdDataMode = GetTagData(SDCARD_TAG_DATAFLAGS, MMC_DATA_READ, DataTags)) == MMC_DATA_READ)
            {
                sdcStateMask = SDHCI_PS_DATA_AVAILABLE;
                sdCommandMask = SDHCI_INT_DATA_AVAIL;
            }
            else
            {
                sdcStateMask = SDHCI_PS_SPACE_AVAILABLE;
                sdCommandMask = SDHCI_INT_SPACE_AVAIL;
            }
        }
    };

    /*
     * When the controller did the transfer there is nothing to move; the bytes
     * are already in memory. All that is left is to make them visible to the
     * CPU and, if it went the long way round, to copy them back.
     */
    if (bus->sdcb_BusFlags & AF_Bus_DMAActive)
    {
        bus->sdcb_BusFlags &= ~AF_Bus_DMAActive;

        CacheClearE((bus->sdcb_BusFlags & AF_Bus_DMABounced)
                        ? bus->sdcb_ADMABounce : bus->sdcb_ADMAData,
                    bus->sdcb_ADMALen, CACRF_ClearD);

        if (bus->sdcb_BusFlags & AF_Bus_DMABounced)
        {
            if (sdDataMode == MMC_DATA_READ)
                CopyMem(bus->sdcb_ADMABounce, bus->sdcb_ADMAData, bus->sdcb_ADMALen);

            bus->sdcb_BusFlags &= ~AF_Bus_DMABounced;
        }

        if (bus->sdcb_LEDCtrl)
            bus->sdcb_LEDCtrl(LED_OFF);

        DTRANS(bug("[SDBus%02u] %s: ADMA2 moved %d bytes\n", bus->sdcb_BusNum,
                   __PRETTY_FUNCTION__, bus->sdcb_ADMALen));

        return 0;
    }

    if (sdData)
    {
        DTRANS(bug("[SDBus%02u] %s: Transfering CMD %02d Data..\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, sdCommand));
        do {
            bus->sdcb_BusStatus = bus->sdcb_IOReadLong(SDHCI_INT_STATUS, bus);
            if (bus->sdcb_BusStatus & SDHCI_INT_ERROR) {
                bug("[SDBus%02u] %s:    Error [status 0x%X]!\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_BusStatus);
                retVal = -1;
                break;
            }

            if ((bus->sdcb_BusStatus & sdCommandMask) && (bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus) & sdcStateMask)) {
                ULONG currword, tranlen = (sdDataLen > (1 << bus->sdcb_SectorShift)) ? (1 << bus->sdcb_SectorShift) : sdDataLen;
                ULONG tranwords = tranlen >> 2;
                volatile ULONG *dataPtr = (volatile ULONG *)sdData;

                DTRANS(bug("[SDBus%02u] %s: Attempting to %s %dbytes\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, ((sdDataMode == MMC_DATA_READ) ? "read" : "write"), tranlen));
                if (sdDataMode == MMC_DATA_READ)
                {
                    /*
                     * A bus that can read its data port in bulk does so: the
                     * indirect call costs about as much as the register access
                     * itself, and this is one call per word otherwise.
                     */
                    if (bus->sdcb_IOReadLongs)
                    {
                        bus->sdcb_IOReadLongs(SDHCI_BUFFER, (ULONG *)dataPtr,
                                              tranwords, bus);
                    }
                    else
                    {
                        for (currword = 0; currword < tranwords; currword++)
                        {
                            dataPtr[currword] = bus->sdcb_IOReadLong(SDHCI_BUFFER, bus);
                        }
                    }
                }
                else
                {
                    for (currword = 0; currword < tranwords; currword++)
                    {
                        bus->sdcb_IOWriteLong(SDHCI_BUFFER, dataPtr[currword], bus);
                    }
                }
                sdData += tranlen;
                sdDataLen -= tranlen;
                waiting = FALSE;
            }
            else if (!(bus->sdcb_BusStatus & SDHCI_INT_DATA_END))
            {
                /*
                 * The card needs about 20us to put the next block in the
                 * buffer, so poll at that scale: waiting a millisecond for
                 * each block of a multi-block transfer costs more time than
                 * the transfer itself.
                 */
                if (!waiting)
                {
                    waitStart = sdcard_CurrentTime();
                    waiting = TRUE;
                }
                sdcard_Udelay(1);

                if ((sdcard_CurrentTime() - waitStart) > SDHCI_DATA_TIMEOUT_US)
                {
                    bug("[SDBus%02u] %s:    Timeout!\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);
                    retVal = -1;
                    break;
                }
            }
        } while ((sdDataLen > 0) && (!(bus->sdcb_BusStatus & SDHCI_INT_DATA_END)));

        if (bus->sdcb_LEDCtrl)
            bus->sdcb_LEDCtrl(LED_OFF);

        sdDataLenTag->ti_Data -= sdDataLen;
    }

    return retVal;
}

/*
 * WaitCmd: Wait for an SDHCI command (and optional data transfer) to complete.
 *
 * After SendCmd() issues a command, WaitCmd() waits for the SDHCI controller
 * to clear the inhibit bits specified in 'mask' (typically CMD_INHIBIT and
 * DATA_INHIBIT).
 *
 * For commands with data (reads/writes), the flow is:
 *   1. SendCmd() writes the command and sets bus->sdcb_DataListener
 *   2. RESPONSE IRQ fires -> BusIRQ() calls FinishCmd(), signals this task
 *   3. DATA_AVAIL IRQ fires -> BusIRQ() calls FinishData() to do PIO transfer
 *   4. DATA_INHIBIT clears once all data is transferred
 *
 * Problem (observed on BCM2708/Pi 3B+):
 *   The RESPONSE and DATA_AVAIL interrupts arrive as separate IRQs.
 *   When BusIRQ() handles the RESPONSE IRQ, it acknowledges the interrupt
 *   by writing to INT_STATUS. If DATA_AVAIL is asserted in the narrow
 *   window between reading INT_STATUS and the acknowledgment write, the
 *   DATA_AVAIL interrupt can be lost:
 *
 *     - BusIRQ reads INT_STATUS -> sees RESPONSE only
 *     - Hardware asserts DATA_AVAIL (sets bit in INT_STATUS)
 *     - BusIRQ writes INT_STATUS to ack RESPONSE -> may clear DATA_AVAIL too
 *     - BusIRQ returns, no further IRQ edge generated
 *     - Result: FinishData() is never called, DATA_INHIBIT stays set,
 *       WaitCmd() polls for 100 seconds before timing out.
 *
 * Fix:
 *   During the poll loop, if DATA_INHIBIT is still set and FinishData()
 *   hasn't been called yet (DataListener non-null), we check INT_STATUS
 *   directly. If DATA_AVAIL or DATA_END is pending, we recover by calling
 *   FinishData() from task context under Disable() protection to prevent
 *   a race with BusIRQ().
 */
ULONG FNAME_SDCBUS(WaitCmd)(ULONG mask, ULONG timeout, struct sdcard_Bus *bus)
{
#if defined(__AROSEXEC_SMP__)
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;
#endif
    /* Callers count their timeout in milliseconds. */
    ULONG waitStart = sdcard_CurrentTime();
    ULONG waitLimit = (timeout ? timeout : 1000) * 1000;
    BOOL  timedOut = FALSE;

    if (bus->sdcb_Task == FindTask(NULL))
    {
        /*
         * Sleep for the completion interrupt once the controller has shown
         * that it delivers them. Until then, poll instead: a controller whose
         * interrupt never arrives would otherwise wedge the boot here rather
         * than report a timeout.
         */
        if (bus->sdcb_BusFlags & AF_Bus_IRQSeen)
        {
            Wait(1L << bus->sdcb_CommandSig);
        }
        else
        {
            /*
             * Nothing will service the controller if its interrupt does not
             * reach us, and a data transfer needs servicing to move the bytes
             * at all, so run the handler from here while polling.
             */
            while ((sdcard_CurrentTime() - waitStart) < waitLimit)
            {
                if (bus->sdcb_BusIRQHandler)
                    bus->sdcb_BusIRQHandler(bus, NULL);

                if (SetSignal(0, 0) & (1L << bus->sdcb_CommandSig))
                    break;
                sdcard_Udelay(SDHCI_POLL_US);
            }
            SetSignal(0, 1L << bus->sdcb_CommandSig);
        }
    }
    else
    {
        sdcard_Udelay(SDHCI_POLL_US);
    }

    while (bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus) & mask) {
        sdcard_Udelay(SDHCI_POLL_US);

        /*
         * Lost interrupt recovery for data transfers.
         *
         * If we're waiting for DATA_INHIBIT to clear and FinishData()
         * hasn't run yet (DataListener still set), check whether the
         * hardware actually has data ready that we missed due to a
         * lost IRQ. If so, process it directly.
         */
        if ((mask & SDHCI_PS_DATA_INHIBIT) && bus->sdcb_DataListener)
        {
            ULONG intStatus = bus->sdcb_IOReadLong(SDHCI_INT_STATUS, bus);

            if (intStatus & (SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL |
                             SDHCI_INT_DATA_END))
            {
                DIRQ(bug("[SDBus%02u] WaitCmd: lost IRQ recovery, INT_STATUS=%08x PS=%08x\n",
                    bus->sdcb_BusNum, intStatus,
                    bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus)));

                /*
                 * Claim the DataListener under lock to prevent BusIRQ()
                 * on another CPU from racing with us.
                 */
                {
                    struct TagItem *dataListener;
#if defined(__AROSEXEC_SMP__)
                    KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#else
                    Disable();
#endif
                    dataListener = bus->sdcb_DataListener;
                    if (dataListener)
                        bus->sdcb_DataListener = NULL;
#if defined(__AROSEXEC_SMP__)
                    KrnSpinUnLock(&bus->sdcb_Lock);
#else
                    Enable();
#endif
                    if (dataListener)
                    {
                        bus->sdcb_BusStatus = intStatus;
                        if (FNAME_SDCBUS(FinishData)(dataListener, bus) != -1)
                        {
                            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS,
                                intStatus & SDHCI_INT_DATA_MASK, bus);
                            bus->sdcb_BusStatus &= ~SDHCI_INT_DATA_MASK;
                        }
                    }
                }

                /* Re-check PRESENT_STATE immediately after recovery */
                continue;
            }
        }

        if (bus->sdcb_CmdError)
            break;

        if ((sdcard_CurrentTime() - waitStart) > waitLimit)
        {
            bug("[SDBus%02u] WaitCmd: TIMEOUT! PS=%08x BS=%08x DL=%p\n",
                bus->sdcb_BusNum,
                bus->sdcb_IOReadLong(SDHCI_PRESENT_STATE, bus),
                bus->sdcb_BusStatus, bus->sdcb_DataListener);
            timedOut = TRUE;
            break;
        }
    }

    /*
     * Response recovery for non-bus-task callers.
     *
     * On QEMU (and potentially real hardware), the SDHCI controller clears
     * CMD_INHIBIT as soon as the command response is received — before the
     * interrupt is delivered to the CPU. When WaitCmd is called from a task
     * other than the bus task, we poll PRESENT_STATE and may exit the loop
     * before the IRQ handler has had a chance to call FinishCmd() to read
     * the response register. The result: the caller's response tag stays 0.
     *
     * Fix: if RespListener is still set after the poll loop, the IRQ handler
     * hasn't processed it yet. Claim it under Disable() (to prevent a race
     * with the IRQ handler) and call FinishCmd() directly from task context.
     * Also acknowledge the RESPONSE interrupt so it doesn't fire later and
     * try to process a stale (now-NULL) RespListener.
     */
    if (bus->sdcb_RespListener)
    {
        struct TagItem *respListener;
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#else
        Disable();
#endif
        respListener = bus->sdcb_RespListener;
        if (respListener)
            bus->sdcb_RespListener = NULL;
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&bus->sdcb_Lock);
#else
        Enable();
#endif
        if (respListener)
        {
            FNAME_SDCBUS(FinishCmd)(respListener, bus);
            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE, bus);
        }
    }

    /* Same recovery for data transfers — see response recovery comment above */
    if (bus->sdcb_DataListener)
    {
        struct TagItem *dataListener;
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#else
        Disable();
#endif
        dataListener = bus->sdcb_DataListener;
        if (dataListener)
            bus->sdcb_DataListener = NULL;
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&bus->sdcb_Lock);
#else
        Enable();
#endif
        if (dataListener)
        {
            bus->sdcb_BusStatus = bus->sdcb_IOReadLong(SDHCI_INT_STATUS, bus);
            FNAME_SDCBUS(FinishData)(dataListener, bus);
            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS,
                bus->sdcb_BusStatus & SDHCI_INT_DATA_MASK, bus);
        }
    }

    if (timedOut || bus->sdcb_CmdError)
    {
        return -1;
    }

    return 0;
}


ULONG FNAME_SDCBUS(Rsp136Unpack)(ULONG *buf, ULONG offset, const ULONG len)
{
    const ULONG mask = ((len < 32) ? (1 << len) : 0) - 1;
    const ULONG shift = (offset) & 0x1F;
    ULONG retval;

    retval = buf[3 - (offset >> 5)] >> shift;
    if (len + shift > 32)
        retval |= buf[3 - (offset >> 5) - 1] << ((32 - shift) % 32);

    return (retval & mask);
}

/********** BUS IRQ HANDLER **************/

#undef DIRQ
#define DIRQ(x) /* x */

/* Only a real interrupt records that the controller delivers them. */
static void FNAME_SDCBUS(BusIRQEntry)(struct sdcard_Bus *bus, void *data)
{
    if (!(bus->sdcb_BusFlags & AF_Bus_IRQSeen))
        bug("[SDBus%02u] controller interrupt is being delivered\n", bus->sdcb_BusNum);

    bus->sdcb_BusFlags |= AF_Bus_IRQSeen;

    /* Dispatch through the bus's own handler: not every controller uses the
       SDHCI register layout, and the SDHOST bus leaves the accessors this
       one reads through set to NULL. */
    if (bus->sdcb_BusIRQHandler)
        bus->sdcb_BusIRQHandler(bus, data);
}

void FNAME_SDCBUS(BusIRQ)(struct sdcard_Bus *bus, void *_unused)
{
#if defined(__AROSEXEC_SMP__)
    struct SDCardBase *SDCardBase = bus->sdcb_DeviceBase;
#endif
    BOOL        error = FALSE;
    int         loops = 0;

    DIRQ(bug("[SDBus%02u] %s(bus @ 0x%p)\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus));

    if (!(bus))
    {
        DIRQ(bug("[SDBus%02u] %s: Bad Params!\n", __PRETTY_FUNCTION__));
        return;
    }

    /*
     * Re-read INT_STATUS after processing to catch bits (e.g. DATA_AVAIL)
     * that arrive while we handle earlier events (e.g. RESPONSE).
     * Some controllers may not re-assert the IRQ line for newly-set bits.
     */
    bus->sdcb_BusStatus = bus->sdcb_IOReadLong(SDHCI_INT_STATUS, bus);

    while ((bus->sdcb_BusStatus & bus->sdcb_IntrMask) && loops < 8)
    {
    loops++;

    if (!(bus->sdcb_BusStatus & SDHCI_INT_ERROR))
    {
        DIRQ(bug("[SDBus%02u] %s: Status = %08x\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_BusStatus));

        if (bus->sdcb_BusStatus & (SDHCI_INT_CARD_INSERT|SDHCI_INT_CARD_REMOVE))
        {
            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, (bus->sdcb_BusStatus & (SDHCI_INT_CARD_INSERT|SDHCI_INT_CARD_REMOVE)), bus);

#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
            bus->sdcb_BusFlags &= ~AF_Bus_MediaPresent;
            bus->sdcb_BusFlags |= AF_Bus_MediaChanged;

            if (bus->sdcb_BusStatus & SDHCI_INT_CARD_INSERT)
                bus->sdcb_BusFlags |= AF_Bus_MediaPresent;
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&bus->sdcb_Lock);
#endif

            bus->sdcb_BusStatus &= ~(SDHCI_INT_CARD_INSERT|SDHCI_INT_CARD_REMOVE);

            if (bus->sdcb_Task)
                Signal(bus->sdcb_Task, (1L << bus->sdcb_MediaSig));
        }
        if (bus->sdcb_BusStatus & SDHCI_INT_CMD_MASK)
        {
            struct TagItem *respListener;

            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, (bus->sdcb_BusStatus & SDHCI_INT_CMD_MASK), bus);

#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
            respListener = bus->sdcb_RespListener;
            if ((bus->sdcb_BusStatus & SDHCI_INT_RESPONSE) && respListener)
                bus->sdcb_RespListener = NULL;
            else
                respListener = NULL;
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&bus->sdcb_Lock);
#endif
            if (respListener)
            {
                if (FNAME_SDCBUS(FinishCmd)(respListener, bus) == -1)
                    error = TRUE;
            }
            if (bus->sdcb_Task && (bus->sdcb_BusStatus & SDHCI_INT_RESPONSE))
            {
                DIRQ(bug("Signalling task %p (%s) with signal %d\n", bus->sdcb_Task, bus->sdcb_Task->tc_Node.ln_Name, bus->sdcb_CommandSig));
                Signal(bus->sdcb_Task, 1L << bus->sdcb_CommandSig);
            }

            bus->sdcb_BusStatus &= ~SDHCI_INT_CMD_MASK;
        }
        if (bus->sdcb_BusStatus & SDHCI_INT_DATA_MASK)
        {
            struct TagItem *dataListener;
            /*
             * An ADMA2 transfer never reports a buffer ready - the controller
             * is doing the moving - so its completion is the transfer end.
             */
            ULONG claimMask = (bus->sdcb_BusFlags & AF_Bus_DMAActive)
                                ? SDHCI_INT_DATA_END
                                : (SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL);

#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
            dataListener = bus->sdcb_DataListener;
            if ((bus->sdcb_BusStatus & claimMask) && dataListener)
                bus->sdcb_DataListener = NULL;
            else
                dataListener = NULL;
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&bus->sdcb_Lock);
#endif
            if (dataListener)
            {
                if (FNAME_SDCBUS(FinishData)(dataListener, bus) == -1)
                    error = TRUE;
                bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, (bus->sdcb_BusStatus & (SDHCI_INT_DATA_AVAIL|SDHCI_INT_SPACE_AVAIL)), bus);
                bus->sdcb_BusStatus &= ~SDHCI_INT_DATA_MASK;
            }
            else
            {
                bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, (bus->sdcb_BusStatus & SDHCI_INT_DATA_MASK), bus);
                bus->sdcb_BusStatus &= ~SDHCI_INT_DATA_MASK;
            }
        }
    }
    else
    {
        bug("[SDBus%02u] %s: ERROR [Status = %08x]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_BusStatus);
        if (bus->sdcb_BusStatus & SDHCI_INT_ACMD12ERR)
        {
            bug("[SDBus%02u] %s:       [acmd12err = %04x    ]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_IOReadWord(SDHCI_ACMD12_ERR, bus));
        }
        error = TRUE;
    }

    if (error)
    {
        /* Hand the failure to whoever is waiting on this command, before the
           bits are cleared out of the shared status copy below. */
        bus->sdcb_CmdError |= (bus->sdcb_BusStatus & SDHCI_INT_ERROR_MASK) | SDHCI_INT_ERROR;

        if (bus->sdcb_BusStatus & bus->sdcb_IntrMask)
        {
            bug("[SDBus%02u] %s: Clearing Unhandled Interrupts [%08x]\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_BusStatus & bus->sdcb_IntrMask);
            bus->sdcb_IOWriteLong(SDHCI_INT_STATUS, bus->sdcb_BusStatus & bus->sdcb_IntrMask, bus);
            bus->sdcb_BusStatus &= ~bus->sdcb_IntrMask;
        }
        bug("[SDBus%02u] %s: Reseting SDHCI CMD/DATA\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__);

        FNAME_SDCBUS(SoftReset)(SDHCI_RESET_CMD, bus);
        FNAME_SDCBUS(SoftReset)(SDHCI_RESET_DATA, bus);

        /*
         * The reset took the command with it, so there is no response left to
         * read and no data left to move. Drop the listeners rather than let
         * WaitCmd's recovery run FinishCmd()/FinishData() over a controller
         * that has been wiped, and put back the transfer state FinishData()
         * would have cleaned up had it run.
         */
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
        bus->sdcb_RespListener = NULL;
        bus->sdcb_DataListener = NULL;
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&bus->sdcb_Lock);
#endif
        bus->sdcb_BusFlags &= ~(AF_Bus_DMAActive | AF_Bus_DMABounced);

        if (bus->sdcb_LEDCtrl)
            bus->sdcb_LEDCtrl(LED_OFF);

        /*
         * Signal the bus task so it wakes up from WaitCmd().
         * Without this, a command timeout (no RESPONSE received)
         * leaves the bus task stuck in Wait() indefinitely,
         * because the success path is the only place that signals.
         */
        if (bus->sdcb_Task)
            Signal(bus->sdcb_Task, 1L << bus->sdcb_CommandSig);

        break;  /* Don't re-read after error */
    }

    /* Re-read INT_STATUS to catch bits that arrived during processing */
    bus->sdcb_BusStatus = bus->sdcb_IOReadLong(SDHCI_INT_STATUS, bus);

    } /* end while loop */

    DIRQ(bug("[SDBus%02u] %s: Done.\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
}

#undef DIRQ
#define DIRQ(x) /* x */
/********** BUS MANAGEMENT TASK **************/

/* Only the bus task writes the retry latch. Backend calls and host waits
 * run outside metadata/IRQ locks. Buses without recovery retain their path. */
static BOOL sdcard_TaskRecoverUnit(struct sdcard_Unit *unit, BOOL rearm)
{
    struct sdcard_Bus *bus = unit->sdcu_Bus;
    BOOL blocked, result;
    if (!bus->sdcb_RecoveryNeeded && !bus->sdcb_RecoveryPrepare &&
        !bus->sdcb_RecoveryFinish)
        return TRUE;
    SDCBUS_LockUnit(bus);
    if (rearm)
        unit->sdcu_RecoveryBlocked = FALSE;
    blocked = unit->sdcu_RecoveryBlocked;
    /* Reserve the attempt before releasing the guard: a concurrent Open
     * cannot start from cached protocol state after failed recovery. */
    if (!blocked)
        unit->sdcu_RecoveryBlocked = TRUE;
    SDCBUS_UnlockUnit(bus);
    if (blocked)
        return FALSE;
    result = FNAME_SDCBUS(RecoverUnit)(unit);
    SDCBUS_LockUnit(bus);
    unit->sdcu_RecoveryBlocked = !result;
    SDCBUS_UnlockUnit(bus);
    return result;
}

/* TaskSig is an explicit recovery notification, not a media-change event.
 * A radio client must release host ownership before signalling this task.
 * No registration, replacement unit or retry of a completed IO occurs here. */
static void sdcard_TaskRecoverBus(struct sdcard_Bus *bus)
{
    ULONG ordinal;
    for (ordinal = 0; ordinal < bus->sdcb_BusUnits->sdcbu_UnitCnt; ordinal++)
    {
        struct sdcard_Unit *unit = (&bus->sdcb_BusUnits->sdcbu_Units)[ordinal];
        if (unit)
            sdcard_TaskRecoverUnit(unit, TRUE);
    }
}

static BOOL sdcard_TaskHandleIO(struct IORequest *io)
{
    /* Metadata queries, write-protection replies and change listeners remain
     * usable after recovery failure. Gate every sector-read entry. The
     * existing direct-SCSI handler is unsupported and performs no card IO. */
    if (io->io_Command == CMD_READ || io->io_Command == TD_READ64 ||
        io->io_Command == NSCMD_TD_READ64)
        if (!sdcard_TaskRecoverUnit(Unit(io), FALSE))
        {
            io->io_Error = TDERR_DiskChanged;
            ((struct IOStdReq *)io)->io_Actual = 0;
            return TRUE;
        }
    /* Exactly one dispatch. Failure is returned unchanged, never replayed.
     * The next request observes backend invalidation under host ownership. */
    return FNAME_SDC(HandleIO)(io);
}

void FNAME_SDCBUS(BusTask)(struct sdcard_Bus *bus)
{
    LIBBASETYPEPTR LIBBASE = bus->sdcb_DeviceBase;
    struct IORequest *msg;
    ULONG sig, tasksig;
    BOOL startupScanDone = FALSE;

    DFUNCS(bug("[SDBus%02u] %s()\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));

    bus->sdcb_Timer = sdcard_OpenTimer(LIBBASE);

    /* Get the signal used for sleeping */
    bus->sdcb_Task = FindTask(0);

    D(bug("[SDBus%02u] %s: Task @ 0x%p\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_Task));

    bus->sdcb_TaskSig = AllocSignal(-1);
    if ((bus->sdcb_MediaSig = AllocSignal(-1)) == -1)
    {
        D(bug("[SDBus%02u] %s: failed to allocate Media Change sigbit\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
    }

    if ((bus->sdcb_CommandSig = AllocSignal(-1)) == -1)
    {
        D(bug("[SDBus%02u] %s: failed to allocate Command Completed sigbit\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
    }

    /* Failed to get it? Use SIGBREAKB_CTRL_E instead */
    if (bus->sdcb_TaskSig < 0)
        bus->sdcb_TaskSig = SIGBREAKB_CTRL_E;

    sig = ((1L << bus->sdcb_MsgPort->mp_SigBit)|(1L << bus->sdcb_TaskSig)|(1L << bus->sdcb_MediaSig));

    DINIT(bug("[SDBus%02u] %s: TaskSig: %d, MediaSig: %d\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_TaskSig, bus->sdcb_MediaSig));

    /* Install IRQ handler (controller-specific) */
    if (bus->sdcb_BusIRQHandler)
    {
        if ((bus->sdcb_IRQHandle = KrnAddIRQHandler(bus->sdcb_BusIRQ, FNAME_SDCBUS(BusIRQEntry), bus, NULL)) != NULL)
        {
            DINIT(bug("[SDBus%02u] %s: IRQHandle @ 0x%p for IRQ#%ld\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, bus->sdcb_IRQHandle, bus->sdcb_BusIRQ));
        }
    }

    /* Controller-specific post-IRQ init (enable interrupts, detect card) */
    if (bus->sdcb_BusPostIRQInit)
        bus->sdcb_BusPostIRQInit(bus);

    if (!startupScanDone)
    {
        startupScanDone = TRUE;
        if (LIBBASE->sdcard_PendingBusTasks > 0)
            LIBBASE->sdcard_PendingBusTasks--;
    }

    for (;;)
    {
        tasksig = Wait(sig);

        if (tasksig & (1L << bus->sdcb_MediaSig))
        {
            ULONG flags;
#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_READ);
#endif
            flags = bus->sdcb_BusFlags;
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&bus->sdcb_Lock);
#endif
            if (flags & AF_Bus_MediaChanged)
            {
                D(bug("[SDBus%02u] %s: detected unit change\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__));
                D(bug("[SDBus%02u] %s: Card %s Detected!\n", bus->sdcb_BusNum, __PRETTY_FUNCTION__, (flags & AF_Bus_MediaPresent) ? "Insert" : "Eject" ));
                if (flags & AF_Bus_MediaPresent)
                {
                    FNAME_SDCBUS(RegisterUnit)(bus);
                }
                else
                {
                    FNAME_SDCBUS(ReleaseUnit)(bus);
                }
            }
        }

        if (tasksig & (1L << bus->sdcb_TaskSig))
            sdcard_TaskRecoverBus(bus);

        /* Defer IORequest processing if the bus is active */
        {
            BOOL canProcess;
#if defined(__AROSEXEC_SMP__)
            KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_READ);
#endif
            canProcess = (tasksig & (1L << bus->sdcb_MsgPort->mp_SigBit)) && (!(bus->sdcb_BusFlags & AF_Bus_Active));
#if defined(__AROSEXEC_SMP__)
            KrnSpinUnLock(&bus->sdcb_Lock);
#endif
            if (canProcess)
            {
#if defined(__AROSEXEC_SMP__)
                KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
                /* process pending requests .. */
                bus->sdcb_BusFlags |= AF_Bus_Active;
#if defined(__AROSEXEC_SMP__)
                KrnSpinUnLock(&bus->sdcb_Lock);
#endif
                while ((msg = (struct IORequest *)GetMsg(bus->sdcb_MsgPort)))
                {
                    /* And do IO's */
                    if (sdcard_TaskHandleIO(msg))
                    {
                        ReplyMsg((struct Message *)msg);
                    }
                }
#if defined(__AROSEXEC_SMP__)
                KrnSpinLock(&bus->sdcb_Lock, NULL, SPINLOCK_MODE_WRITE);
#endif
                bus->sdcb_BusFlags &= ~AF_Bus_Active;
#if defined(__AROSEXEC_SMP__)
                KrnSpinUnLock(&bus->sdcb_Lock);
#endif
            }
        }
    }
}
