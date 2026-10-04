/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: esp32p4board.resource - the board's devices, from its profile.
*/

/*
 * The drivers for what sits on the board are portable: gt911.hidd and
 * gsl3670.hidd know their chips, touchscreen.hidd turns any touch
 * controller into a pointing device, hidd.i2c.esp32p4 drives the SoC's I2C
 * controllers. Which controller is wired to which pads, at which address,
 * with which reset line, firmware and default calibration is the board's.
 * The profile (board.h) says it as data; the board's own file (d1001.c,
 * jc1060p470c.c, chosen by board.mk) holds what is code, such as a reset
 * line behind a port expander. This file is the common part:
 *
 *   bus        = hidd.i2c.esp32p4 on the profile's touch port and pads
 *   controller = the board's touch controller on that bus
 *   driver     = hidd.mouse.touchscreen for the controller, installed in
 *                the pointing device subsystem
 *
 * Priority 8 runs it after the I2C, touch and controller modules and the
 * input subsystem have initialised, and before DOS. Nothing here touches
 * a bus: the controller is started by the touchscreen driver's own task,
 * which also calls the line hook. A failure leaves the desktop without
 * touch and says why in the log. RTC and audio codec belong here too when
 * they get drivers.
 */

#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <exec/types.h>
#include <hidd/hidd.h>
#include <hidd/i2c.h>
#include <hidd/i2c-esp32p4.h>
#include <hidd/mouse.h>
#include <hidd/touchscreen.h>

#include <proto/exec.h>
#include <proto/oop.h>

#include "../kernel/hardware.h"
#include "esp32p4board_intern.h"
#include LC_LIBDEFS_FILE

struct ExecBase *SysBase;
struct Library *OOPBase;

OOP_AttrBase board_I2CP4AB, board_I2CDeviceAB;
OOP_AttrBase board_TouchControllerAB, board_TouchScreenAB;
static OOP_MethodID board_midI2CDevWriteRead;

OOP_Object *board_bus(ULONG port, ULONG sda, ULONG scl, ULONG hz)
{
    struct TagItem tags[] =
    {
        {board_I2CP4AB + aoHidd_I2C_ESP32P4_Port,  port},
        {board_I2CP4AB + aoHidd_I2C_ESP32P4_SDA,   sda},
        {board_I2CP4AB + aoHidd_I2C_ESP32P4_SCL,   scl},
        {board_I2CP4AB + aoHidd_I2C_ESP32P4_Speed, hz},
        {TAG_DONE, 0}
    };

    return OOP_NewObject(NULL, CLID_Hidd_I2C_ESP32P4, tags);
}

OOP_Object *board_i2c_device(OOP_Object *bus, ULONG addr7, CONST_STRPTR name)
{
    struct TagItem tags[] =
    {
        {board_I2CDeviceAB + aoHidd_I2CDevice_Driver,  (IPTR)bus},
        {board_I2CDeviceAB + aoHidd_I2CDevice_Address, addr7 << 1},
        {board_I2CDeviceAB + aoHidd_I2CDevice_Name,    (IPTR)name},
        {TAG_DONE, 0}
    };

    return bus ? OOP_NewObject(NULL, CLID_Hidd_I2CDevice, tags) : NULL;
}

BOOL board_i2c_xfer(OOP_Object *dev, UBYTE *w, ULONG wlen,
                    UBYTE *r, ULONG rlen)
{
    struct pHidd_I2CDevice_WriteRead msg;

    msg.mID = board_midI2CDevWriteRead;
    msg.writeBuffer = w;
    msg.writeLength = wlen;
    msg.readBuffer = r;
    msg.readLength = rlen;
    return (BOOL)OOP_DoMethod(dev, (OOP_Msg)&msg);
}

void board_gpio(unsigned int gpio, LONG level)
{
    unsigned long bit = 1UL << (gpio & 31);
    unsigned long iomux = P4_IOMUX_BASE + P4_IOMUX_PIN(gpio);
    unsigned long out_set = gpio < 32 ? P4_GPIO_OUT_W1TS : P4_GPIO_OUT1_W1TS;
    unsigned long out_clr = gpio < 32 ? P4_GPIO_OUT_W1TC : P4_GPIO_OUT1_W1TC;
    unsigned long en_set = gpio < 32 ? P4_GPIO_ENABLE_W1TS
                                     : P4_GPIO_ENABLE1_W1TS;
    unsigned long en_clr = gpio < 32 ? P4_GPIO_ENABLE_W1TC
                                     : P4_GPIO_ENABLE1_W1TC;
    unsigned long v;

    if (level == vHidd_TouchLine_Release)
        p4_w32(P4_GPIO_BASE + en_clr, bit);
    else
        /* Level first, then the driver, so the pad never shows the
           other one. */
        p4_w32(P4_GPIO_BASE + (level ? out_set : out_clr), bit);

    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_PU | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
    v |= P4_IOMUX_FUN_IE;
    p4_w32(iomux, v);

    if (level != vHidd_TouchLine_Release)
    {
        v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio));
        v &= ~P4_GPIO_OUT_SEL_MASK;
        v |= P4_GPIO_OUT_SEL_GPIO | P4_GPIO_OEN_SEL;
        p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio), v);
        p4_w32(P4_GPIO_BASE + en_set, bit);
    }
}

static const struct HIDD_TouchCalibration board_touch_cal =
{
    P4_BOARD_TOUCH_X_MIN, P4_BOARD_TOUCH_X_MAX,
    P4_BOARD_TOUCH_Y_MIN, P4_BOARD_TOUCH_Y_MAX,
    P4_BOARD_TOUCH_MIRROR_Y ? vHidd_TouchCal_MirrorY : 0
};

static BOOL board_touch(void)
{
    struct BoardTouch t = {0};
    OOP_Object *bus, *controller, *hw, *driver;
    OOP_Class *touch_class;

    if (!board_touch_prepare(&t))
    {
        bug("[Board] touch: board wiring not set up\n");
        return FALSE;
    }
    if (!OpenLibrary(P4I2C_NAME, 0) || !OpenLibrary(t.module, 0)
        || !OpenLibrary(TOUCHSCREEN_NAME, 0))
    {
        bug("[Board] touch: a driver module is missing (%s, %s, %s)\n",
            P4I2C_NAME, t.module, TOUCHSCREEN_NAME);
        return FALSE;
    }

    bus = board_bus(P4_BOARD_TOUCH_I2C_PORT, P4_BOARD_TOUCH_SDA_GPIO,
                    P4_BOARD_TOUCH_SCL_GPIO, P4_BOARD_TOUCH_I2C_HZ);
    if (!bus)
    {
        bug("[Board] touch: no I2C bus\n");
        return FALSE;
    }
    {
        struct TagItem tags[] =
        {
            {board_TouchControllerAB + aoHidd_TouchController_Bus, (IPTR)bus},
            {board_TouchControllerAB + aoHidd_TouchController_Address,
             P4_BOARD_TOUCH_ADDR},
            {board_TouchControllerAB + aoHidd_TouchController_LineHook,
             (IPTR)t.lines},
            {t.controller_tags ? TAG_MORE : TAG_DONE,
             (IPTR)t.controller_tags}
        };

        controller = OOP_NewObject(NULL, t.class_id, tags);
    }
    if (!controller)
    {
        bug("[Board] touch: no %s object\n", t.chip);
        return FALSE;
    }

    hw = OOP_NewObject(NULL, CLID_HW_Mouse, NULL);
    touch_class = OOP_FindClass(CLID_Hidd_Mouse_TouchScreen);
    driver = NULL;
    if (hw && touch_class)
    {
        struct TagItem tags[] =
        {
            {board_TouchScreenAB + aoHidd_TouchScreen_Controller,
             (IPTR)controller},
            {board_TouchScreenAB + aoHidd_TouchScreen_Name,
             (IPTR)P4_BOARD_NAME},
            {board_TouchScreenAB + aoHidd_TouchScreen_Width, P4_LOGICAL_W},
            {board_TouchScreenAB + aoHidd_TouchScreen_Height, P4_LOGICAL_H},
            {board_TouchScreenAB + aoHidd_TouchScreen_DefaultCalibration,
             (IPTR)&board_touch_cal},
            {t.firmware ? board_TouchScreenAB
                          + aoHidd_TouchScreen_FirmwarePaths : TAG_DONE,
             (IPTR)t.firmware},
            {TAG_DONE, 0}
        };

        driver = HW_AddDriver(hw, touch_class, tags);
    }
    if (!driver)
    {
        /* The buses stay: neither is worth tearing down at boot for a
           driver that will not come. */
        bug("[Board] touch: driver not installed (subsystem %p, class %p)\n",
            hw, touch_class);
        OOP_DisposeObject(controller);
        return FALSE;
    }
    bug("[Board] touch: %s on i2c%u pads %u/%u at %u Hz, %ux%u, driver %p\n",
        t.chip, P4_BOARD_TOUCH_I2C_PORT, P4_BOARD_TOUCH_SDA_GPIO,
        P4_BOARD_TOUCH_SCL_GPIO, P4_BOARD_TOUCH_I2C_HZ, P4_LOGICAL_W,
        P4_LOGICAL_H, driver);
    return TRUE;
}

static int P4Board_Init(LIBBASETYPEPTR P4BoardBase)
{
    SysBase = P4BoardBase->pb_SysBase;
    OOPBase = OpenLibrary("oop.library", 0);
    if (!OOPBase)
        return FALSE;
    bug("[Board] " P4_BOARD_NAME "\n");

    board_I2CP4AB = OOP_ObtainAttrBase(IID_Hidd_I2C_ESP32P4);
    board_I2CDeviceAB = OOP_ObtainAttrBase(IID_Hidd_I2CDevice);
    board_TouchControllerAB = OOP_ObtainAttrBase(IID_Hidd_TouchController);
    board_TouchScreenAB = OOP_ObtainAttrBase(IID_Hidd_TouchScreen);
    board_midI2CDevWriteRead = OOP_GetMethodID(IID_Hidd_I2CDevice,
                                               moHidd_I2CDevice_WriteRead);
    if (board_I2CP4AB && board_I2CDeviceAB && board_TouchControllerAB
        && board_TouchScreenAB)
        board_touch();
    else
        bug("[Board] the I2C and touch interfaces are missing\n");

    /* Stay resident whatever happened: the hooks and the calibration are
       referenced by the objects made above. */
    return TRUE;
}

ADD2INITLIB(P4Board_Init, 0)
