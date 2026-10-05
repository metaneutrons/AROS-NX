/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Internals of the ESP32-P4 i2c bus driver.
*/

#ifndef P4I2C_INTERN_H
#define P4I2C_INTERN_H

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <oop/oop.h>
#include <hidd/i2c-esp32p4.h>

#include "p4i2c_hw.h"

#define P4I2C_PORTS     2
#define P4I2C_NO_PAD    0xFF
#define P4I2C_MAX_GPIO  54

/*
 * One controller.  Bus objects for it share this: the lock makes their
 * transfers one at a time, and the recorded configuration tells a transfer
 * whether the controller has to be reprogrammed for the object it comes
 * from.  `ready` in hw is clear until the first transfer, because the
 * kernel may have left the controller in any state before Exec.
 */
struct p4i2c_ctrl
{
    struct SignalSemaphore  lock;
    struct P4I2CPort        hw;
    UBYTE                   sda, scl;   /* pads of the live objects */
    ULONG                   users;      /* live bus objects */
    ULONG                   hz;         /* rate hw is programmed for */
    char                    name[8];
};

struct p4i2c_staticdata
{
    struct Library      *OOPBase;
    struct Library      *utilityBase;
    struct Library      *i2cBase;

    OOP_AttrBase         hiddAB;
    OOP_AttrBase         hiddI2CAB;
    OOP_AttrBase         hiddI2CDeviceAB;
    OOP_AttrBase         hiddI2CESP32P4AB;

    OOP_Class           *busClass;

    struct SignalSemaphore  objLock;    /* guards users/sda/scl */
    struct p4i2c_ctrl    ctrl[P4I2C_PORTS];
};

struct P4I2CBase
{
    struct Library          LibNode;
    struct p4i2c_staticdata psd;
};

#define PSD(cl) (&((struct P4I2CBase *)cl->UserData)->psd)

/* Per bus object */
struct p4i2c_busdata
{
    struct p4i2c_ctrl  *ctrl;
    ULONG               port;
    ULONG               hz;
    LONG                last;           /* P4_I2C_* of the last transfer */
};

#undef HiddAttrBase
#undef HiddI2CAttrBase
#undef HiddI2CDeviceAttrBase
#undef HiddI2CESP32P4AttrBase

#define HiddAttrBase            (PSD(cl)->hiddAB)
#define HiddI2CAttrBase         (PSD(cl)->hiddI2CAB)
#define HiddI2CDeviceAttrBase   (PSD(cl)->hiddI2CDeviceAB)
#define HiddI2CESP32P4AttrBase  (PSD(cl)->hiddI2CESP32P4AB)

/* Resolve through `cl`; the init code #undefs them. */
#define UtilityBase             (PSD(cl)->utilityBase)
#define OOPBase                 (PSD(cl)->OOPBase)

/* p4i2c_time.c: FALSE when the platform timer this build needs is absent. */
BOOL p4i2c_TimePrepare(void);

#endif /* P4I2C_INTERN_H */
