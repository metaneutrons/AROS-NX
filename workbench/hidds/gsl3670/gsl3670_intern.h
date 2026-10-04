/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Internals of the GSL3670 touch controller driver.
*/

#ifndef GSL3670_INTERN_H
#define GSL3670_INTERN_H

#include <exec/libraries.h>
#include <oop/oop.h>
#include <utility/hooks.h>
#include <hidd/gsl3670.h>
#include <hidd/touchscreen.h>

#define GSL_ADDR_DEFAULT        0x40

struct gsl_staticdata
{
    struct Library  *OOPBase;
    struct Library  *utilityBase;

    OOP_AttrBase     hiddI2CDeviceAB;
    OOP_AttrBase     hiddTouchControllerAB;
    OOP_MethodID     midWriteRead;

    OOP_Class       *gslClass;
};

struct GSLBase
{
    struct Library          LibNode;
    struct gsl_staticdata   gsd;
};

#define GSD(cl) (&((struct GSLBase *)cl->UserData)->gsd)

struct gsl_data
{
    OOP_Object             *bus;
    OOP_Object             *dev;        /* hidd.i2c.device at addr */
    struct Hook            *lines;
    UBYTE                   addr;
    BOOL                    started;
    ULONG                   firmware_bytes;
    ULONG                   raw_x, raw_y;
};

#undef HiddI2CDeviceAttrBase
#undef HiddTouchControllerAttrBase
#define HiddI2CDeviceAttrBase       (GSD(cl)->hiddI2CDeviceAB)
#define HiddTouchControllerAttrBase (GSD(cl)->hiddTouchControllerAB)

#define UtilityBase                 (GSD(cl)->utilityBase)
#define OOPBase                     (GSD(cl)->OOPBase)

#endif /* GSL3670_INTERN_H */
