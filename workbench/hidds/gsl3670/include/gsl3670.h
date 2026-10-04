/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Silead GSL3670 touch controller driver; installed as
          <hidd/gsl3670.h>.

    An IID_Hidd_TouchController (see <hidd/touchscreen.h>) on a hidd.i2c
    bus. The chip runs from RAM: Start needs the board's firmware image, a
    sequence of little-endian <offset, value> records of eight bytes each,
    which no part of AROS carries. Creation attributes are the interface's:
    Bus (required), Address (default 0x40), LineHook (reset; strongly
    advised, the load starts from a reset), FirmwareBytes (required, the
    size of the board's image) and RawWidth/RawHeight (the range the image
    configures).
*/

#ifndef HIDD_GSL3670_H
#define HIDD_GSL3670_H

#define CLID_Hidd_TouchController_GSL3670 "hidd.touchcontroller.gsl3670"
#define GSL3670_NAME                      "gsl3670.hidd"

#endif
