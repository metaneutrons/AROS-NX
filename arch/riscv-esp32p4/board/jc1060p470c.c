/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Guition JC1060P470C wiring for esp32p4board.resource.
*/

/*
 * The GT911's RST and INT are plain GPIOs (P4_BOARD_TOUCH_RST_GPIO and
 * P4_BOARD_TOUCH_IRQ_GPIO); the driver sequences them to select its
 * address. Bus, address and calibration are in jc1060p470c.h.
 */

#include <aros/asmcall.h>
#include <exec/types.h>
#include <hidd/gt911.h>
#include <hidd/touchscreen.h>
#include <utility/hooks.h>

#include "board.h"
#include "esp32p4board_intern.h"

AROS_UFH3S(IPTR, jc1060_touch_lines,
           AROS_UFHA(struct Hook *, hook, A0),
           AROS_UFHA(OOP_Object *, controller, A2),
           AROS_UFHA(struct HIDD_TouchLineMsg *, msg, A1))
{
    AROS_USERFUNC_INIT

    (void)hook;
    (void)controller;
    if (msg->line == vHidd_TouchLine_Reset)
        board_gpio(P4_BOARD_TOUCH_RST_GPIO, msg->level);
    else if (msg->line == vHidd_TouchLine_Int)
        board_gpio(P4_BOARD_TOUCH_IRQ_GPIO, msg->level);
    else
        return FALSE;
    return TRUE;

    AROS_USERFUNC_EXIT
}

static struct Hook jc1060_touch_hook;
static struct TagItem jc1060_touch_tags[2];

BOOL board_touch_prepare(struct BoardTouch *t)
{
    jc1060_touch_hook.h_Entry = (HOOKFUNC)jc1060_touch_lines;
    jc1060_touch_tags[0].ti_Tag =
        board_TouchControllerAB + aoHidd_TouchController_AltAddress;
    jc1060_touch_tags[0].ti_Data = P4_BOARD_TOUCH_ADDR_ALT;
    jc1060_touch_tags[1].ti_Tag = TAG_DONE;

    t->module = GT911_NAME;
    t->class_id = CLID_Hidd_TouchController_GT911;
    t->chip = "GT911";
    t->lines = &jc1060_touch_hook;
    t->controller_tags = jc1060_touch_tags;
    t->firmware = NULL;
    return TRUE;
}
