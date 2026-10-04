/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Seeed reTerminal D1001 wiring for esp32p4board.resource.
*/

/*
 * The GSL3670's reset is output 12 of the PCA9535 port expander on I2C1,
 * which also holds the panel's power and reset lines; the kernel drives
 * those before Exec, this file only output 12. The chip runs from RAM, so
 * the board names its firmware image. Bus, address, image size, raw range
 * and calibration are in d1001.h.
 */

#include <aros/asmcall.h>
#include <aros/debug.h>
#include <exec/types.h>
#include <hidd/gsl3670.h>
#include <hidd/touchscreen.h>
#include <utility/hooks.h>

#include "../kernel/hardware.h"
#include "esp32p4board_intern.h"

static OOP_Object *d1001_expander;

static BOOL d1001_expander_read16(UBYTE reg, UWORD *value)
{
    UBYTE data[2] = {0, 0};

    if (!board_i2c_xfer(d1001_expander, &reg, 1, data, 2))
        return FALSE;
    *value = (UWORD)(data[0] | ((UWORD)data[1] << 8));
    return TRUE;
}

/* Write, then read back: a latch that does not hold the value is a
   failure, not something to carry on from. */
static BOOL d1001_expander_write16(UBYTE reg, UWORD value)
{
    UBYTE data[3];
    UWORD readback = 0;

    data[0] = reg;
    data[1] = (UBYTE)value;
    data[2] = (UBYTE)(value >> 8);
    return board_i2c_xfer(d1001_expander, data, 3, NULL, 0)
        && d1001_expander_read16(reg, &readback)
        && readback == value;
}

/*
 * Output 12 only, every other latch and direction bit preserved. Low goes
 * into the latch before the pin is made an output, so claiming it cannot
 * glitch high; afterwards it stays a driven output, high being the
 * controller's running state.
 */
static BOOL d1001_touch_reset(LONG level)
{
    UWORD output = 0, config = 0;

    if (!d1001_expander_read16(P4_PCA9535_OUTPUT, &output)
        || !d1001_expander_read16(P4_PCA9535_CONFIG, &config))
        return FALSE;
    if (level)
        return d1001_expander_write16(P4_PCA9535_OUTPUT,
                                      (UWORD)(output | P4_EXP_TOUCH_RST));
    return d1001_expander_write16(P4_PCA9535_OUTPUT,
                                  (UWORD)(output & ~P4_EXP_TOUCH_RST))
        && d1001_expander_write16(P4_PCA9535_CONFIG,
                                  (UWORD)(config & ~P4_EXP_TOUCH_RST));
}

/* Reset through the expander; INT is not used by the driver. */
AROS_UFH3S(IPTR, d1001_touch_lines,
           AROS_UFHA(struct Hook *, hook, A0),
           AROS_UFHA(OOP_Object *, controller, A2),
           AROS_UFHA(struct HIDD_TouchLineMsg *, msg, A1))
{
    AROS_USERFUNC_INIT

    (void)hook;
    (void)controller;
    if (msg->line == vHidd_TouchLine_Reset)
        return d1001_touch_reset(msg->level);
    return msg->line == vHidd_TouchLine_Int;

    AROS_USERFUNC_EXIT
}

static struct Hook d1001_touch_hook;
static struct TagItem d1001_touch_tags[4];
static CONST_STRPTR const d1001_touch_firmware[] =
{
    P4_BOARD_TOUCH_FW_PATH,
    P4_BOARD_TOUCH_FW_FALLBACK,
    NULL
};

BOOL board_touch_prepare(struct BoardTouch *t)
{
    OOP_Object *bus = board_bus(1, P4_BOARD_I2C1_SDA_GPIO,
                                P4_BOARD_I2C1_SCL_GPIO, 100000);

    d1001_expander = board_i2c_device(bus, P4_PCA9535_ADDR, "PCA9535");
    if (!d1001_expander)
    {
        bug("[Board] touch: no port expander on I2C1\n");
        return FALSE;
    }

    d1001_touch_hook.h_Entry = (HOOKFUNC)d1001_touch_lines;
    d1001_touch_tags[0].ti_Tag =
        board_TouchControllerAB + aoHidd_TouchController_FirmwareBytes;
    d1001_touch_tags[0].ti_Data = P4_BOARD_TOUCH_FW_BYTES;
    d1001_touch_tags[1].ti_Tag =
        board_TouchControllerAB + aoHidd_TouchController_RawWidth;
    d1001_touch_tags[1].ti_Data = P4_BOARD_TOUCH_RAW_W;
    d1001_touch_tags[2].ti_Tag =
        board_TouchControllerAB + aoHidd_TouchController_RawHeight;
    d1001_touch_tags[2].ti_Data = P4_BOARD_TOUCH_RAW_H;
    d1001_touch_tags[3].ti_Tag = TAG_DONE;

    t->module = GSL3670_NAME;
    t->class_id = CLID_Hidd_TouchController_GSL3670;
    t->chip = "GSL3670";
    t->lines = &d1001_touch_hook;
    t->controller_tags = d1001_touch_tags;
    t->firmware = d1001_touch_firmware;
    return TRUE;
}
