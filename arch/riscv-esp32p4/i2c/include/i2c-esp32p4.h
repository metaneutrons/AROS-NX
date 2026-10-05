/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Public interface to the ESP32-P4 i2c bus driver.

    What a driver for a chip on an ESP32-P4 i2c bus needs to see; installed
    as <hidd/i2c-esp32p4.h>.  The driver is an ordinary hidd.i2c subclass:
    make a bus object here, hang a hidd.i2c.device off it, and use the
    device interface.  The chip driver itself stays portable; only the code
    that knows the board (which controller, which pads) creates the bus.

        struct Library *drv = OpenLibrary(P4I2C_NAME, 0);

        struct TagItem busTags[] =
        {
            { aHidd_I2C_ESP32P4_Port,  1      },
            { aHidd_I2C_ESP32P4_SDA,   7      },
            { aHidd_I2C_ESP32P4_SCL,   8      },
            { aHidd_I2C_ESP32P4_Speed, 100000 },
            { TAG_DONE,                0      }
        };
        OOP_Object *bus = OOP_NewObject(NULL, CLID_Hidd_I2C_ESP32P4, busTags);

    Device addresses are in hidd.i2c's wire form, the 7-bit address shifted
    left by one.

    The controller takes whole transactions (a command list over a 32-byte
    FIFO), so the bus implements WriteRead and ProbeAddress and refuses the
    byte-level methods.  Every hidd.i2c.device method except WriteBytes and
    WriteVec reduces to WriteRead; those two drive the byte-level methods
    and therefore fail here.  A write carries at most 30 bytes, a read at
    most 255.  Transfers are polled from the caller's task.
*/

#ifndef HIDD_I2C_ESP32P4_H
#define HIDD_I2C_ESP32P4_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#ifndef OOP_OOP_H
#include <oop/oop.h>
#endif

#define CLID_Hidd_I2C_ESP32P4   "hidd.i2c.esp32p4"
#define IID_Hidd_I2C_ESP32P4    "hidd.i2c.esp32p4"

#define HiddI2CESP32P4AttrBase  __IHidd_I2C_ESP32P4

#ifndef __OOP_NOATTRBASES__
extern OOP_AttrBase HiddI2CESP32P4AttrBase;
#endif

enum
{
    /* Controller 0 or 1.  Required. [I.G] */
    aoHidd_I2C_ESP32P4_Port,
    /* GPIO numbers of the data and clock pads.  Required.  A controller
       is on one pair of pads at a time; a second bus object for the same
       controller with other pads is refused. [I.G] */
    aoHidd_I2C_ESP32P4_SDA,
    aoHidd_I2C_ESP32P4_SCL,
    /* Bus clock in Hz, default 100000.  Objects for one controller may
       differ; the controller is reprogrammed when the next transfer comes
       from an object with another rate. [I.G] */
    aoHidd_I2C_ESP32P4_Speed,
    /* The transport result of this object's last transfer, a P4_I2C_*
       code (0 ok, -1 no acknowledge, -2 timeout, -3 arbitration lost,
       -4 stuck, -5 bus busy, -6 too long, -7 not ready), since the
       hidd.i2c methods return only TRUE or FALSE. [..G] */
    aoHidd_I2C_ESP32P4_LastResult,

    num_Hidd_I2C_ESP32P4_Attrs
};

#define aHidd_I2C_ESP32P4_Port \
    (HiddI2CESP32P4AttrBase + aoHidd_I2C_ESP32P4_Port)
#define aHidd_I2C_ESP32P4_SDA \
    (HiddI2CESP32P4AttrBase + aoHidd_I2C_ESP32P4_SDA)
#define aHidd_I2C_ESP32P4_SCL \
    (HiddI2CESP32P4AttrBase + aoHidd_I2C_ESP32P4_SCL)
#define aHidd_I2C_ESP32P4_Speed \
    (HiddI2CESP32P4AttrBase + aoHidd_I2C_ESP32P4_Speed)
#define aHidd_I2C_ESP32P4_LastResult \
    (HiddI2CESP32P4AttrBase + aoHidd_I2C_ESP32P4_LastResult)

#define IS_I2CESP32P4_ATTR(attr, idx) \
    (((idx) = (attr) - HiddI2CESP32P4AttrBase) < num_Hidd_I2C_ESP32P4_Attrs)

/* The module to open before the class exists. */
#define P4I2C_NAME              "i2c-esp32p4.hidd"

#endif /* HIDD_I2C_ESP32P4_H */
