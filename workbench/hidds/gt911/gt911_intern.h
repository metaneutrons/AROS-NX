/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Internals of the GT911 touch controller driver.
*/

#ifndef GT911_INTERN_H
#define GT911_INTERN_H

#include <exec/libraries.h>
#include <oop/oop.h>
#include <utility/hooks.h>
#include <hidd/gt911.h>
#include <hidd/touchscreen.h>

#define GT911_ADDR_DEFAULT      0x5D
#define GT911_ADDR_ALT          0x14

struct gt911_staticdata
{
    struct Library  *OOPBase;
    struct Library  *utilityBase;

    OOP_AttrBase     hiddI2CDeviceAB;
    OOP_AttrBase     hiddTouchControllerAB;
    OOP_MethodID     midWriteRead;

    OOP_Class       *gt911Class;
};

struct GT911Base
{
    struct Library          LibNode;
    struct gt911_staticdata gsd;
};

#define GSD(cl) (&((struct GT911Base *)cl->UserData)->gsd)

struct gt911_data
{
    OOP_Object             *bus;
    OOP_Object             *dev;        /* hidd.i2c.device at `addr` */
    struct Hook            *lines;
    UBYTE                   addr_pref, addr_alt, addr;
    BOOL                    started;
    ULONG                   res_x, res_y;
    UWORD                   firmware;
    struct HIDD_TouchFrame  last;       /* repeated while no new report */
};

#undef HiddI2CDeviceAttrBase
#undef HiddTouchControllerAttrBase
#define HiddI2CDeviceAttrBase       (GSD(cl)->hiddI2CDeviceAB)
#define HiddTouchControllerAttrBase (GSD(cl)->hiddTouchControllerAB)

#define UtilityBase                 (GSD(cl)->utilityBase)
#define OOPBase                     (GSD(cl)->OOPBase)

#endif /* GT911_INTERN_H */
