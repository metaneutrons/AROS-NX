/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Register native ESP32-P4 SD/MMC slot 0 with generic sdcard.device.
*/

#define DEBUG 0
#include <aros/debug.h>
#include <aros/symbolsets.h>

#include <exec/memory.h>
#include <hardware/mmc.h>
#include <proto/exec.h>

#include "sdcard_esp32p4_intern.h"

static int FNAME_P4SD(Init)(struct SDCardBase *SDCardBase)
{
    struct sdcard_Bus *bus;
    struct sdcard_BusUnits *units;
    struct p4sd_private *priv;

    bus = AllocPooled(SDCardBase->sdcard_MemPool, sizeof(*bus));
    units = AllocPooled(SDCardBase->sdcard_MemPool, sizeof(*units));
    priv = AllocPooled(SDCardBase->sdcard_MemPool, sizeof(*priv));
    if (bus == NULL || units == NULL || priv == NULL)
        goto fail;

    bus->sdcb_DeviceBase = SDCardBase;
    bus->sdcb_IOBase = (APTR)P4_SDMMC_BASE;
    bus->sdcb_BusIRQ = 0;
    bus->sdcb_ClockMin = P4SD_CLOCK_MIN_HZ;
    bus->sdcb_ClockMax = P4SD_CLOCK_MAX_HZ;
    bus->sdcb_Power = MMC_VDD_320_330 | MMC_VDD_330_340;
    bus->sdcb_SectorShift = 9;
    bus->sdcb_BusUnits = units;
    bus->sdcb_Private = (IPTR)priv;

    /* No generic SDHCI accessor is valid for this DesignWare register set. */
    bus->sdcb_IOReadByte = NULL;
    bus->sdcb_IOReadWord = NULL;
    bus->sdcb_IOReadLong = NULL;
    bus->sdcb_IOWriteByte = NULL;
    bus->sdcb_IOWriteWord = NULL;
    bus->sdcb_IOWriteLong = NULL;

    bus->sdcb_SoftReset = FNAME_P4SDBUS(SoftReset);
    bus->sdcb_SetClock = FNAME_P4SDBUS(SetClock);
    bus->sdcb_SetPowerLevel = FNAME_P4SDBUS(SetPowerLevel);
    bus->sdcb_SendCmd = FNAME_P4SDBUS(SendCmd);
    bus->sdcb_WaitCmd = FNAME_P4SDBUS(WaitCmd);
    bus->sdcb_FinishCmd = FNAME_P4SDBUS(FinishCmd);
    bus->sdcb_FinishData = FNAME_P4SDBUS(FinishData);
    bus->sdcb_BusIRQHandler = NULL;       /* polling only */
    bus->sdcb_SetBusWidth = FNAME_P4SDBUS(SetBusWidth);
    bus->sdcb_BusInit = FNAME_P4SD(BusInit);
    bus->sdcb_BusPostIRQInit = FNAME_P4SD(BusPostIRQInit);

    ObtainSemaphore(&SDCardBase->sdcard_BusSem);
    units->sdcbu_UnitBase = SDCardBase->sdcard_TotalBusUnits;
    units->sdcbu_UnitMax = 1;
    SDCardBase->sdcard_TotalBusUnits += 1;
    bus->sdcb_BusNum = SDCardBase->sdcard_BusCnt++;
    ReleaseSemaphore(&SDCardBase->sdcard_BusSem);

    if (units->sdcbu_UnitBase != 0 || bus->sdcb_BusNum != 0)
        bug("[P4SD--] native slot expected Unit0/Bus0, got base %u bus %u\n",
            units->sdcbu_UnitBase, bus->sdcb_BusNum);

    FNAME_SDC(RegisterBus)(bus, SDCardBase);
    return TRUE;

fail:
    bug("[P4SD--] no memory for native slot 0\n");
    if (priv != NULL)
        FreePooled(SDCardBase->sdcard_MemPool, priv, sizeof(*priv));
    if (units != NULL)
        FreePooled(SDCardBase->sdcard_MemPool, units, sizeof(*units));
    if (bus != NULL)
        FreePooled(SDCardBase->sdcard_MemPool, bus, sizeof(*bus));
    return FALSE;
}

ADD2INITLIB(FNAME_P4SD(Init), SDCARD_BUSINITPRIO)
