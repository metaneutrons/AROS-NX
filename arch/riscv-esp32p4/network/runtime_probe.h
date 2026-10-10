/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef P4_WIFI_RUNTIME_PROBE_H
#define P4_WIFI_RUNTIME_PROBE_H
#include <exec/libraries.h>
struct P4WifiBase
{
    struct Library library;
    struct ExecBase *sysbase;
};
#endif
