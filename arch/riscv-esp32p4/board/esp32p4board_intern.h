#ifndef ESP32P4BOARD_INTERN_H
#define ESP32P4BOARD_INTERN_H

#include <exec/libraries.h>
#include <oop/oop.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

struct P4BoardBase
{
    struct Library   pb_Library;
    struct ExecBase *pb_SysBase;
};

/*
 * The board's wiring, supplied by the board's own file (board.mk selects
 * it as P4_BOARD_SETUP, as it selects the kernel's panel table). The
 * common part, esp32p4board.c, makes the touch bus from the profile,
 * creates the controller with Bus, Address and LineHook plus the board's
 * extra tags, and installs the touchscreen driver.
 */
struct BoardTouch
{
    CONST_STRPTR        module;         /* controller driver module */
    CONST_STRPTR        class_id;       /* its class */
    CONST_STRPTR        chip;           /* for the log */
    struct Hook        *lines;          /* reset and interrupt lines */
    struct TagItem     *controller_tags;/* more creation tags, or NULL */
    CONST_STRPTR const *firmware;       /* firmware paths, or NULL */
};

/* The board file's part: wire up whatever the lines need (an expander,
   say) and fill *t. Runs with the attribute bases below obtained. */
BOOL board_touch_prepare(struct BoardTouch *t);

/* Common helpers for the board files (esp32p4board.c). */
extern OOP_AttrBase board_I2CP4AB, board_I2CDeviceAB;
extern OOP_AttrBase board_TouchControllerAB, board_TouchScreenAB;

OOP_Object *board_bus(ULONG port, ULONG sda, ULONG scl, ULONG hz);
OOP_Object *board_i2c_device(OOP_Object *bus, ULONG addr7, CONST_STRPTR name);
BOOL board_i2c_xfer(OOP_Object *dev, UBYTE *w, ULONG wlen,
                    UBYTE *r, ULONG rlen);
/* Drive a pad from the GPIO output register (level 0 or 1), or let it
   float as an input (vHidd_TouchLine_Release). */
void board_gpio(unsigned int gpio, LONG level);

#endif
