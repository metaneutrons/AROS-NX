#ifndef ESP32P4_BOARD_H
#define ESP32P4_BOARD_H

/* A SoC image must have exactly one board profile at compile time. */
#if defined(P4_BOARD_D1001) && P4_BOARD_D1001 == 1
#include "d1001.h"
#else
#error No supported ESP32-P4 board profile was selected
#endif

#endif
