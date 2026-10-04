/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Goodix GT911 touch controller driver; installed as <hidd/gt911.h>.

    An IID_Hidd_TouchController (see <hidd/touchscreen.h>) on a hidd.i2c
    bus.  Creation attributes are the interface's: Bus (required), Address
    and AltAddress (default 0x5D and 0x14; the chip takes the one selected
    by the INT level during reset) and LineHook (optional; without it the
    chip is not reset and both addresses are tried as it is).
*/

#ifndef HIDD_GT911_H
#define HIDD_GT911_H

#define CLID_Hidd_TouchController_GT911 "hidd.touchcontroller.gt911"
#define GT911_NAME                      "gt911.hidd"

#endif
