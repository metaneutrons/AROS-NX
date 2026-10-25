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

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/bootloader.h>
#include <proto/expansion.h>
#include <proto/kernel.h>

#include <hardware/mmc.h>
#include <hardware/sdhc.h>

#include <string.h>
#include <stdio.h>

#include "sdcard_base.h"
#include "sdcard_bus.h"
#include "sdcard_unit.h"
#include "timer.h"

#include LC_LIBDEFS_FILE

BOOL FNAME_SDC(RegisterBus)(struct sdcard_Bus *bus, LIBBASETYPEPTR LIBBASE)
{
    DINIT(bug("[SDCard--] %s(0x%p)\n", __PRETTY_FUNCTION__, bus));

    ObtainSemaphore(&LIBBASE->sdcard_BusSem);
    AddTail(&LIBBASE->sdcard_Buses, (struct Node *)bus);
    ReleaseSemaphore(&LIBBASE->sdcard_BusSem);

    return TRUE;
}

/*
 * Libinit functions -:
 * 0:   FNAME_SDC(CommonInit) -> common libbase init.
 * 10:  FNAME_SDC(XXXInit) -> chipset/implementation specific bus init.
 * 120: FNAME_SDC(Scan) -> Scan registered buses for units.
 */
static int FNAME_SDC(Scan)(LIBBASETYPEPTR LIBBASE)
{
    struct sdcard_Bus   *busCurrent;

    DINIT(bug("[SDCard--] %s()\n", __PRETTY_FUNCTION__));

    ForeachNode(&LIBBASE->sdcard_Buses, busCurrent)
    {
        char *sdcBusTaskName;
        if ((sdcBusTaskName = AllocVec(24, MEMF_PUBLIC)) != NULL)
        {
            if (busCurrent->sdcb_LEDCtrl)
                busCurrent->sdcb_LEDCtrl(LED_OFF);

            /* Controller-specific scan-time init (interrupts, clock, power, bus width) */
            if (busCurrent->sdcb_BusInit)
                busCurrent->sdcb_BusInit(busCurrent);

            DINIT(bug("[SDCard--] %s: Launching Bus Task...\n", __PRETTY_FUNCTION__));

            sprintf(sdcBusTaskName, "SDCard bus:%02u Subsystem", busCurrent->sdcb_BusNum);

            LIBBASE->sdcard_PendingBusTasks++;

            if (NewCreateTask(
                TASKTAG_PC         , FNAME_SDCBUS(BusTask),
                TASKTAG_NAME       , sdcBusTaskName,
                TASKTAG_STACKSIZE  , SDCARD_BUSTASKSTACK,
                TASKTAG_PRI        , SDCARD_BUSTASKPRI,
                TASKTAG_TASKMSGPORT, &busCurrent->sdcb_MsgPort,
                TASKTAG_ARG1       , busCurrent,
                TAG_DONE) == NULL)
            {
                LIBBASE->sdcard_PendingBusTasks--;
            }
        }
    }

    return TRUE;
}

static int FNAME_SDC(CommonInit)(LIBBASETYPEPTR LIBBASE)
{
    DINIT(bug("[SDCard--] %s()\n", __PRETTY_FUNCTION__));

    if ((ExpansionBase = TaggedOpenLibrary(TAGGEDOPEN_EXPANSION)) == NULL)
    {
        bug("[SDCard--] %s: Failed to open expansion.library\n", __PRETTY_FUNCTION__);
        goto libinit_fail;
    }

    if ((KernelBase = OpenResource("kernel.resource")) == NULL)
    {
        bug("[SDCard--] %s: Failed to open kernel.resource\n", __PRETTY_FUNCTION__);
        goto libinit_fail;
    }

    if ((LIBBASE->sdcard_MemPool = CreatePool(MEMF_CLEAR | MEMF_PUBLIC | MEMF_SEM_PROTECTED , 8192, 4096)) == NULL)
    {
        bug("[SDCard--] %s: Failed to Allocate MemPool\n", __PRETTY_FUNCTION__);
        goto libinit_fail;
    }

    DINIT(bug("[SDCard--] %s: MemPool @ %p\n", __PRETTY_FUNCTION__, LIBBASE->sdcard_MemPool));

    InitSemaphore(&LIBBASE->sdcard_BusSem);
    NEWLIST(&LIBBASE->sdcard_Buses);

    return TRUE;

libinit_fail:
    if (ExpansionBase) CloseLibrary(ExpansionBase);

    return FALSE;
}

static int FNAME_SDC(Open)
(
    LIBBASETYPEPTR LIBBASE,
    struct IORequest *iorq,
    ULONG unitnum,
    ULONG flags
)
{
    struct sdcard_Bus   *busCurrent;
    struct sdcard_Unit *unit;
    ULONG ordinal;

    DDEV(bug("[SDCard--] %s()\n", __PRETTY_FUNCTION__));

    /* Assume it failed */
    iorq->io_Error = IOERR_OPENFAIL;
    iorq->io_Unit = NULL;

    ForeachNode(&LIBBASE->sdcard_Buses, busCurrent)
    {
        SDCBUS_LockUnit(busCurrent);
        if (unitnum < busCurrent->sdcb_BusUnits->sdcbu_UnitBase)
        {
            SDCBUS_UnlockUnit(busCurrent);
            continue;
        }
        ordinal = unitnum - busCurrent->sdcb_BusUnits->sdcbu_UnitBase;
        unit = ordinal < busCurrent->sdcb_BusUnits->sdcbu_UnitCnt ?
                (&busCurrent->sdcb_BusUnits->sdcbu_Units)[ordinal] : NULL;
        if (!unit)
        {
            SDCBUS_UnlockUnit(busCurrent);
            continue;
        }
        if (unit->sdcu_Unit.unit_OpenCnt == 0xFFFF ||
            (unit->sdcu_Flags & AF_Card_Configuring))
        {
            SDCBUS_UnlockUnit(busCurrent);
            break;
        }
        unit->sdcu_Unit.unit_OpenCnt++;
        iorq->io_Unit = (struct Unit *)unit;
        /* An already active unit needs no host acquisition. Recovery clears
         * Active under this same guard before touching controller state. */
        if (unit->sdcu_Flags & AF_Card_Active)
        {
            iorq->io_Error = 0;
            SDCBUS_UnlockUnit(busCurrent);
            break;
        }
        SDCBUS_UnlockUnit(busCurrent);

        if (FNAME_SDCBUS(StartUnit)(unit))
            iorq->io_Error = 0;
        else
        {
            SDCBUS_LockUnit(busCurrent);
            unit->sdcu_Unit.unit_OpenCnt--;
            SDCBUS_UnlockUnit(busCurrent);
            iorq->io_Unit = NULL;
        }
        break;
    }
    return iorq->io_Error ? FALSE : TRUE;
}

/* Close given device */
static int FNAME_SDC(Close)
(
    LIBBASETYPEPTR LIBBASE,
    struct IORequest *iorq
)
{
    struct sdcard_Unit *unit = (struct sdcard_Unit *)iorq->io_Unit;

    DDEV(bug("[SDCard--] %s()\n", __PRETTY_FUNCTION__));

    /* First of all make the important fields of struct IORequest invalid! */
    iorq->io_Unit = (struct Unit *)~0;

    /* Decrease use counters of unit */
    SDCBUS_LockUnit(unit->sdcu_Bus);
    if (unit->sdcu_Unit.unit_OpenCnt)
        unit->sdcu_Unit.unit_OpenCnt--;
    SDCBUS_UnlockUnit(unit->sdcu_Bus);

    return TRUE;
}

ADD2INITLIB(FNAME_SDC(CommonInit), 0)
ADD2INITLIB(FNAME_SDC(Scan), 127)
ADD2OPENDEV(FNAME_SDC(Open), 0)
ADD2CLOSEDEV(FNAME_SDC(Close), 0)
