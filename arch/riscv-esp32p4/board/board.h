#ifndef ESP32P4_BOARD_H
#define ESP32P4_BOARD_H

/* A SoC image must have exactly one board profile at compile time. */
#if defined(P4_BOARD_D1001) && P4_BOARD_D1001 == 1
#include "d1001.h"
#elif defined(P4_BOARD_JC1060P470C) && P4_BOARD_JC1060P470C == 1
#include "jc1060p470c.h"
#else
#error No supported ESP32-P4 board profile was selected
#endif

/*
 * Optional capabilities default to absent, so a profile only names what its
 * board has. Every profile must state its panel, touch controller and SD
 * wiring explicitly; the checks below refuse a profile that does not.
 */
#ifndef P4_BOARD_PANEL_EXPANDER
#define P4_BOARD_PANEL_EXPANDER         0
#endif
#if !defined(P4_BOARD_PANEL_JD9365) && !defined(P4_BOARD_PANEL_JD9165)
#error "board profile names no panel controller"
#endif
#if !defined(P4_BOARD_TOUCH_GSL3670) && !defined(P4_BOARD_TOUCH_GT911)
#error "board profile names no touch controller"
#endif
#if !defined(P4_BOARD_SD_HAS_DETECT) || !defined(P4_BOARD_SD_HAS_POWER_GPIO)
#error "board profile must state its SD detect and power wiring"
#endif
#if P4_BOARD_PANEL_ROTATE != 0 && P4_BOARD_PANEL_ROTATE != 90
#error "P4_BOARD_PANEL_ROTATE must be 0 or 90"
#endif

#endif
