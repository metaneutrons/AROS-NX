/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Touchscreens: the generic driver and the controller interface.

    Installed as <hidd/touchscreen.h>.  Three parts meet here.

    A touch controller driver (GT911, GSL3670, ...) implements
    IID_Hidd_TouchController: it starts the chip and reads one coherent
    frame of contacts on request.  It knows the chip and nothing about the
    machine: the bus, the address and the reset lines come in as
    attributes from whoever knows the board.

    touchscreen.hidd provides CLID_Hidd_Mouse_TouchScreen, a pointing
    device driver (a CLID_Hidd_Mouse subclass) that polls one controller,
    maps contacts through a calibration, turns them into mouse events by
    gesture rules (tap, hold to drag, double-tap and drag, two fingers for
    the menu button) and applies ENV:Sys/touchscreen.prefs at start and
    whenever it changes.

    The machine's board code creates the bus and the controller object and
    installs the driver with HW_AddDriver() on the CLID_HW_Mouse subsystem,
    passing the controller, a name, the screen size and the measured
    default calibration.  A preferences editor opens touchscreen.hidd,
    finds the driver in the list at the start of its base (the subsystem
    keeps drivers behind private proxies) and reads and sets the
    attributes below.

        OOP_Object *hw = OOP_NewObject(NULL, CLID_HW_Mouse, NULL);
        struct TagItem tags[] =
        {
            { aHidd_TouchScreen_Controller,  (IPTR)controller },
            { aHidd_TouchScreen_Name,        (IPTR)"myboard"  },
            { aHidd_TouchScreen_Width,       1024             },
            { aHidd_TouchScreen_Height,      600              },
            { aHidd_TouchScreen_DefaultCalibration, (IPTR)&cal },
            { TAG_DONE,                      0                }
        };
        HW_AddDriver(hw, OOP_FindClass(CLID_Hidd_Mouse_TouchScreen), tags);
*/

#ifndef HIDD_TOUCHSCREEN_H
#define HIDD_TOUCHSCREEN_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#ifndef OOP_OOP_H
#include <oop/oop.h>
#endif

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

#ifndef EXEC_SEMAPHORES_H
#include <exec/semaphores.h>
#endif

#define TOUCHSCREEN_NAME            "touchscreen.hidd"

#define CLID_Hidd_Mouse_TouchScreen "hidd.mouse.touchscreen"
#define IID_Hidd_TouchScreen        "hidd.touchscreen"
#define IID_Hidd_TouchController    "hidd.touchcontroller"

/* ---------------------------------------------------------------------
   The library base

   OpenLibrary(TOUCHSCREEN_NAME, 1) returns a base that begins with this.
   tsb_Drivers holds one node per driver object, oldest first; walk it
   with tsb_Lock obtained shared. A driver lives until its subsystem
   removes it, which in practice is never; keep the library open while
   using one.
   --------------------------------------------------------------------- */

struct TouchScreenDriverNode
{
    struct MinNode  tdn_Node;
    OOP_Object     *tdn_Driver;
};

struct TouchScreenBase
{
    struct Library          tsb_Library;
    struct SignalSemaphore  tsb_Lock;
    struct MinList          tsb_Drivers;
};

/* ---------------------------------------------------------------------
   Contacts and calibration
   --------------------------------------------------------------------- */

#define HIDD_TOUCH_MAX_CONTACTS 10

struct HIDD_TouchContact
{
    ULONG id;               /* as the controller numbers it; not stable */
    ULONG x, y;             /* raw, in the controller's axes */
};

struct HIDD_TouchFrame
{
    /* reported_count is what the controller said, for diagnosis; count is
       how many contacts follow in contact[]. count 0 is a release. */
    ULONG reported_count;
    ULONG count;
    struct HIDD_TouchContact contact[HIDD_TOUCH_MAX_CONTACTS];
};

/* Raw contact to screen: swap first, then each axis maps its inclusive
   raw bounds onto the screen, mirrored if its flag is set. */
#define vHidd_TouchCal_SwapXY   (1UL << 0)
#define vHidd_TouchCal_MirrorX  (1UL << 1)
#define vHidd_TouchCal_MirrorY  (1UL << 2)

struct HIDD_TouchCalibration
{
    ULONG x_min, x_max;
    ULONG y_min, y_max;
    ULONG flags;
};

/* ---------------------------------------------------------------------
   The controller interface
   --------------------------------------------------------------------- */

#define HiddTouchControllerAttrBase __IHidd_TouchController
#ifndef __OOP_NOATTRBASES__
extern OOP_AttrBase HiddTouchControllerAttrBase;
#endif

enum
{
    /* The hidd.i2c bus object the chip is on (I2C controllers). [I..] */
    aoHidd_TouchController_Bus,
    /* 7-bit address, and the alternative the chip may answer at; 0 lets
       the driver use the chip's defaults. [I.G] */
    aoHidd_TouchController_Address,
    aoHidd_TouchController_AltAddress,
    /* struct Hook * that drives the chip's reset and interrupt lines,
       called with a struct HIDD_TouchLineMsg; NULL if the board wires
       neither. The driver knows the sequence, the board the pins. [I..] */
    aoHidd_TouchController_LineHook,
    /* The raw range ReadFrame reports in: known after Start for a chip
       that says it, given at creation for one whose range is set by its
       firmware. [I.G] */
    aoHidd_TouchController_RawWidth,
    aoHidd_TouchController_RawHeight,
    /* Size of the firmware image Start needs, 0 for none. A chip that runs
       from RAM takes the size of the board's image at creation, so a file
       of any other size is refused before it is sent. [I.G] */
    aoHidd_TouchController_FirmwareBytes,
    /* "GT911" and the like. [..G] */
    aoHidd_TouchController_ChipName,

    num_Hidd_TouchController_Attrs
};

#define aHidd_TouchController_Bus \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_Bus)
#define aHidd_TouchController_Address \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_Address)
#define aHidd_TouchController_AltAddress \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_AltAddress)
#define aHidd_TouchController_LineHook \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_LineHook)
#define aHidd_TouchController_RawWidth \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_RawWidth)
#define aHidd_TouchController_RawHeight \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_RawHeight)
#define aHidd_TouchController_FirmwareBytes \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_FirmwareBytes)
#define aHidd_TouchController_ChipName \
    (HiddTouchControllerAttrBase + aoHidd_TouchController_ChipName)

#define IS_TOUCHCONTROLLER_ATTR(attr, idx) \
    (((idx) = (attr) - HiddTouchControllerAttrBase) \
     < num_Hidd_TouchController_Attrs)

/* What the line hook is asked to do. */
enum
{
    vHidd_TouchLine_Reset,
    vHidd_TouchLine_Int
};
#define vHidd_TouchLine_Release (-1)    /* stop driving: input, high-Z */

struct HIDD_TouchLineMsg
{
    ULONG line;             /* vHidd_TouchLine_* */
    LONG  level;            /* 0, 1 or vHidd_TouchLine_Release */
};

enum
{
    moHidd_TouchController_Start,
    moHidd_TouchController_Stop,
    moHidd_TouchController_ReadFrame,

    NUM_TOUCHCONTROLLER_METHODS
};

/*
 * All three run on the touch driver's task, never at interrupt time, and
 * may sleep.  Start resets the chip (through the line hook, if any), finds
 * it, loads the firmware if it needs one (firmware and bytes are NULL and
 * 0 otherwise) and leaves it reporting; a second Start after an error
 * starts over.  ReadFrame fills one coherent frame; FALSE is a transport
 * error, a frame with count 0 a release.
 */
struct pHidd_TouchController_Start
{
    OOP_MethodID mID;
    CONST_APTR   firmware;
    ULONG        bytes;
};

struct pHidd_TouchController_Stop
{
    OOP_MethodID mID;
};

struct pHidd_TouchController_ReadFrame
{
    OOP_MethodID            mID;
    struct HIDD_TouchFrame *frame;
};

/* ---------------------------------------------------------------------
   The touchscreen driver's attributes
   --------------------------------------------------------------------- */

#define HiddTouchScreenAttrBase __IHidd_TouchScreen
#ifndef __OOP_NOATTRBASES__
extern OOP_AttrBase HiddTouchScreenAttrBase;
#endif

enum
{
    /* The controller object; the driver takes it over and disposes of it
       with itself. [I.G] */
    aoHidd_TouchScreen_Controller,
    /* Names the panel and controller combination; calibration is stored
       under it. Copied. [I.G] */
    aoHidd_TouchScreen_Name,
    /* The screen the touch surface covers, in pixels. [I.G] */
    aoHidd_TouchScreen_Width,
    aoHidd_TouchScreen_Height,
    /* struct HIDD_TouchCalibration *: the measured default (copied at
       creation), and the one in effect. Setting the latter applies it at
       the next poll; the preferences replace it again when they change.
       Get fills the caller's structure. [I.G] and [.SG] */
    aoHidd_TouchScreen_DefaultCalibration,
    aoHidd_TouchScreen_Calibration,
    /* CONST_STRPTR const *, NULL-terminated: where to look for the
       controller's firmware, in order. Must stay valid. [I..] */
    aoHidd_TouchScreen_FirmwarePaths,

    num_Hidd_TouchScreen_Attrs
};

#define aHidd_TouchScreen_Controller \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_Controller)
#define aHidd_TouchScreen_Name \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_Name)
#define aHidd_TouchScreen_Width \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_Width)
#define aHidd_TouchScreen_Height \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_Height)
#define aHidd_TouchScreen_DefaultCalibration \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_DefaultCalibration)
#define aHidd_TouchScreen_Calibration \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_Calibration)
#define aHidd_TouchScreen_FirmwarePaths \
    (HiddTouchScreenAttrBase + aoHidd_TouchScreen_FirmwarePaths)

#define IS_TOUCHSCREEN_ATTR(attr, idx) \
    (((idx) = (attr) - HiddTouchScreenAttrBase) < num_Hidd_TouchScreen_Attrs)

#endif /* HIDD_TOUCHSCREEN_H */
