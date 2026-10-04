/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Library housekeeping for the GT911 touch controller driver.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>

#include <aros/symbolsets.h>

#include <hidd/i2c.h>

#include LC_LIBDEFS_FILE
#include "gt911_intern.h"

#undef OOPBase
#undef UtilityBase
#undef HiddI2CDeviceAttrBase
#undef HiddTouchControllerAttrBase

/* hidd.i2c.device must exist for the bus transfers this driver makes. */
ADD2LIBS((STRPTR)"i2c.hidd", 0, static struct Library *, __gt911_i2cbase);

static int GT911_Init(LIBBASETYPEPTR LIBBASE)
{
    struct gt911_staticdata *gsd = &LIBBASE->gsd;
    struct Library *OOPBase = gsd->OOPBase;

    gsd->utilityBase = TaggedOpenLibrary(TAGGEDOPEN_UTILITY);
    if (!gsd->utilityBase)
        return FALSE;
    gsd->hiddI2CDeviceAB = OOP_ObtainAttrBase(IID_Hidd_I2CDevice);
    gsd->hiddTouchControllerAB = OOP_ObtainAttrBase(IID_Hidd_TouchController);
    gsd->midWriteRead = OOP_GetMethodID(IID_Hidd_I2CDevice,
                                        moHidd_I2CDevice_WriteRead);
    if (!gsd->hiddI2CDeviceAB || !gsd->hiddTouchControllerAB
        || gsd->midWriteRead == (OOP_MethodID)-1)
    {
        bug("[GT911] cannot obtain the i2c and touch interfaces\n");
        return FALSE;
    }
    return TRUE;
}

ADD2INITLIB(GT911_Init, 0)
