/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Read-only RAM block device serving malformed partition tables.

    The A2 acceptance gate requires that truncated, cyclic, overlapping,
    overflowing and bad-CRC tables all fail within a documented budget.  Those
    tables cannot be written to the test card: the medium stays read-only and
    its hash has to be unchanged.  So they are served from here instead.

    The unit number selects the case, which avoids inventing a side channel to
    tell the device what to serve.  Everything is const data; there is no
    write path in this device at all, by construction rather than by policy.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <proto/exec.h>

#include <string.h>

#include <exec/types.h>
#include <exec/exec.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>

#include "ramtest_device.h"

#include LC_LIBDEFS_FILE

#define RAMTEST_SECTOR_SIZE 512

struct RamTestUnit
{
    struct Unit unit;
    UWORD       caseIndex;
    ULONG       reads;      /* sector reads served since the unit was opened */
    ULONG       sectors;    /* sectors served, which is the real budget */
};

/* One unit per corpus case.  RamTestCaseCount is a runtime constant from the
   other translation unit, so the array cannot be sized from it; the init
   below complains loudly rather than silently serving fewer cases than the
   corpus contains, which is what happened once and made a corpus run look
   complete when three cases had never executed. */
#define RAMTEST_MAX_UNITS 32

static struct RamTestUnit RamTestUnits[RAMTEST_MAX_UNITS];
static UWORD RamTestUnitsReady;

/* Find a sector in the sparse list.  Anything not listed reads as zeroes,
   which is what an unused sector on a real medium looks like. */
static const UBYTE *RamTestFindSector(const struct RamTestCase *tc, ULONG lba)
{
    UWORD i;

    for (i = 0; i < tc->sector_count; i++)
    {
        if (tc->sectors[i].lba == lba)
            return tc->sectors[i].data;
    }
    return NULL;
}

static void RamTestRead(struct IOStdReq *io, const struct RamTestCase *tc)
{
    ULONG offset = io->io_Offset;
    ULONG length = io->io_Length;
    UBYTE *dest  = io->io_Data;
    ULONG done   = 0;

    io->io_Actual = 0;

    if ((offset % RAMTEST_SECTOR_SIZE) || (length % RAMTEST_SECTOR_SIZE) ||
        dest == NULL)
    {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    while (done < length)
    {
        ULONG lba = (offset + done) / RAMTEST_SECTOR_SIZE;
        const UBYTE *src;

        /* Truncation: the geometry claims more than the medium will hand
           over.  A parser that trusts the geometry has to cope with a read
           that fails part way through a table. */
        if (lba >= tc->readable_sectors)
        {
            io->io_Error = TDERR_NotSpecified;
            return;
        }

        src = RamTestFindSector(tc, lba);
        if (src)
            CopyMem((APTR)src, dest + done, RAMTEST_SECTOR_SIZE);
        else
            memset(dest + done, 0, RAMTEST_SECTOR_SIZE);

        done += RAMTEST_SECTOR_SIZE;
    }

    io->io_Actual = length;
    io->io_Error  = 0;
}

/* The gate asks for a documented read budget per case.  Counting here is the
   only place that can see every read, whatever the parser decides to do. */

static void RamTestGeometry(struct IOStdReq *io, const struct RamTestCase *tc)
{
    struct DriveGeometry *dg = io->io_Data;

    if (io->io_Length < sizeof(struct DriveGeometry))
    {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    memset(dg, 0, sizeof(struct DriveGeometry));
    dg->dg_SectorSize   = RAMTEST_SECTOR_SIZE;
    dg->dg_TotalSectors = tc->total_sectors;
    dg->dg_Cylinders    = tc->total_sectors;
    dg->dg_CylSectors   = 1;
    dg->dg_Heads        = 1;
    dg->dg_TrackSectors = 1;
    dg->dg_BufMemType   = MEMF_PUBLIC;
    dg->dg_DeviceType   = DG_DIRECT_ACCESS;
    dg->dg_Flags        = DGF_REMOVABLE;

    io->io_Actual = sizeof(struct DriveGeometry);
    io->io_Error  = 0;
}

AROS_LH1(void, BeginIO,
    AROS_LHA(struct IORequest *, io, A1),
    LIBBASETYPEPTR, LIBBASE, 5, RamTest)
{
    AROS_LIBFUNC_INIT

    struct RamTestUnit *unit = (struct RamTestUnit *)io->io_Unit;
    const struct RamTestCase *tc;

    io->io_Error = 0;

    if (unit == NULL || unit->caseIndex >= RamTestCaseCount)
    {
        io->io_Error = IOERR_OPENFAIL;
        goto done;
    }
    tc = &RamTestCases[unit->caseIndex];

    switch (io->io_Command)
    {
    case CMD_READ:
        unit->reads++;
        unit->sectors += ((struct IOStdReq *)io)->io_Length / RAMTEST_SECTOR_SIZE;
        RamTestRead((struct IOStdReq *)io, tc);
        break;

    case TD_GETGEOMETRY:
        RamTestGeometry((struct IOStdReq *)io, tc);
        break;

    case CMD_CLEAR:
    case CMD_UPDATE:
    case TD_MOTOR:
    case TD_PROTSTATUS:
        ((struct IOStdReq *)io)->io_Actual = 0;
        break;

    /* Everything else, and every write in particular, is refused.  There is
       no code below this switch that could write anything. */
    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }

done:
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, AbortIO,
    AROS_LHA(struct IORequest *, io, A1),
    LIBBASETYPEPTR, LIBBASE, 6, RamTest)
{
    AROS_LIBFUNC_INIT

    /* Every request completes inside BeginIO, so there is never one to
       abort. */
    return 0;

    AROS_LIBFUNC_EXIT
}

static int RamTestInit(LIBBASETYPEPTR LIBBASE)
{
    UWORD i;

    if (RamTestCaseCount > RAMTEST_MAX_UNITS)
        bug("[RamTest] corpus has %u cases but only %u units exist\n",
            RamTestCaseCount, RAMTEST_MAX_UNITS);

    for (i = 0; i < RamTestCaseCount && i < RAMTEST_MAX_UNITS; i++)
    {
        NEWLIST(&RamTestUnits[i].unit.unit_MsgPort.mp_MsgList);
        RamTestUnits[i].unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        RamTestUnits[i].unit.unit_MsgPort.mp_Flags        = PA_IGNORE;
        RamTestUnits[i].unit.unit_OpenCnt                 = 0;
        RamTestUnits[i].caseIndex                         = i;
        RamTestUnits[i].reads                             = 0;
        RamTestUnits[i].sectors                           = 0;
    }
    RamTestUnitsReady = i;

    D(bug("[RamTest] %u corpus units ready\n", RamTestUnitsReady));
    return TRUE;
}

static int RamTestOpen(LIBBASETYPEPTR LIBBASE, struct IORequest *iorq,
                       ULONG unitnum, ULONG flags)
{
    if (unitnum >= RamTestUnitsReady)
    {
        iorq->io_Error = IOERR_OPENFAIL;
        return FALSE;
    }

    iorq->io_Unit = (struct Unit *)&RamTestUnits[unitnum];
    RamTestUnits[unitnum].unit.unit_OpenCnt++;
    RamTestUnits[unitnum].reads   = 0;
    RamTestUnits[unitnum].sectors = 0;
    iorq->io_Error = 0;
    return TRUE;
}

static int RamTestClose(LIBBASETYPEPTR LIBBASE, struct IORequest *iorq)
{
    struct RamTestUnit *unit = (struct RamTestUnit *)iorq->io_Unit;

    iorq->io_Unit = (struct Unit *)~0;
    if (unit != NULL && unit != (struct RamTestUnit *)~0)
    {
        /* Report the budget this case actually consumed.  Printed here rather
           than returned through some side channel, because a close always
           happens and needs no new API. */
        bug("[RamTest] case %u '%s': %u requests, %u sectors read\n",
            unit->caseIndex, RamTestCases[unit->caseIndex].name,
            unit->reads, unit->sectors);
        unit->unit.unit_OpenCnt--;
    }
    return TRUE;
}

ADD2INITLIB(RamTestInit, 0)
ADD2OPENDEV(RamTestOpen, 0)
ADD2CLOSEDEV(RamTestClose, 0)
