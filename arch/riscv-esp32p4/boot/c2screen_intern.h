#ifndef ESP32P4_C2SCREEN_INTERN_H
#define ESP32P4_C2SCREEN_INTERN_H

#include <exec/libraries.h>

struct C2ScreenBase
{
    struct Library c2s_Library;
    struct ExecBase *c2s_SysBase;
};

#endif
