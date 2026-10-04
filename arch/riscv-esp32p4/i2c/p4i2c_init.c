#define DEBUG 0
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Library housekeeping for the ESP32-P4 i2c bus driver.

    The two controllers are fixed and known, so there is nothing to
    discover: init prepares their locks and the timebase, and the class
    in p4i2c_class.c makes bus objects for them on request.  The
    hardware is touched only by the first transfer, because the base
    class opens timer.device in its New() and the board code decides
    which controller is wired where.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>

#include <aros/symbolsets.h>

#include <exec/types.h>
#include <hidd/hidd.h>
#include <hidd/i2c.h>

#include LC_LIBDEFS_FILE
#include "p4i2c_intern.h"

#undef OOPBase
#undef UtilityBase
#undef HiddAttrBase
#undef HiddI2CAttrBase
#undef HiddI2CDeviceAttrBase
#undef HiddI2CESP32P4AttrBase

/* The base class must exist before the class is built on it, and this
   runs before the class is created. */
ADD2LIBS((STRPTR)"i2c.hidd", 0, static struct Library *, __p4i2c_i2cbase);

static int P4I2C_Init(LIBBASETYPEPTR LIBBASE)
{
    struct p4i2c_staticdata *psd = &LIBBASE->psd;
    struct Library *OOPBase = psd->OOPBase;
    ULONG i;

    D(bug("[P4I2C] %s()\n", __func__);)

    psd->i2cBase = __p4i2c_i2cbase;
    psd->utilityBase = TaggedOpenLibrary(TAGGEDOPEN_UTILITY);
    if (!psd->utilityBase)
        return FALSE;

    if (!p4i2c_TimePrepare())
    {
        bug("[P4I2C] no platform timer for this kernel; not loading\n");
        return FALSE;
    }

    psd->hiddAB = OOP_ObtainAttrBase(IID_Hidd);
    psd->hiddI2CAB = OOP_ObtainAttrBase(IID_Hidd_I2C);
    psd->hiddI2CDeviceAB = OOP_ObtainAttrBase(IID_Hidd_I2CDevice);
    psd->hiddI2CESP32P4AB = OOP_ObtainAttrBase(IID_Hidd_I2C_ESP32P4);
    if (!psd->hiddAB || !psd->hiddI2CAB || !psd->hiddI2CDeviceAB
        || !psd->hiddI2CESP32P4AB)
    {
        bug("[P4I2C] %s: ObtainAttrBases failed\n", __func__);
        return FALSE;
    }

    InitSemaphore(&psd->objLock);
    for (i = 0; i < P4I2C_PORTS; i++)
    {
        struct p4i2c_ctrl *ctrl = &psd->ctrl[i];

        InitSemaphore(&ctrl->lock);
        ctrl->sda = P4I2C_NO_PAD;
        ctrl->scl = P4I2C_NO_PAD;
        ctrl->name[0] = 'i';
        ctrl->name[1] = '2';
        ctrl->name[2] = 'c';
        ctrl->name[3] = (char)('0' + i);
        ctrl->name[4] = '\0';
    }

    return TRUE;
}

ADD2INITLIB(P4I2C_Init, 0)
