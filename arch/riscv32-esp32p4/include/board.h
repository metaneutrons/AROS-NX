/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Modular Board Support Package (BSP) Interface.
*/

#ifndef ESP32P4_BOARD_H
#define ESP32P4_BOARD_H

#include <exec/types.h>

struct ESP32P4_Board
{
    CONST_STRPTR name;
    CONST_STRPTR description;

    /* Display capabilities */
    UWORD display_width;
    UWORD display_height;
    UBYTE display_bpp;
    UBYTE default_orientation;  /* 0=Portrait, 1=Landscape 90 */

    /* Subsystem initialization callbacks */
    int (*init_board)(void);
    int (*init_display)(void);
    int (*init_touch)(void);
    int (*init_audio)(void);
    int (*init_camera)(void);
    int (*init_rtc)(void);
};

/* Get active board descriptor */
const struct ESP32P4_Board *get_active_board(void);

#endif /* ESP32P4_BOARD_H */
