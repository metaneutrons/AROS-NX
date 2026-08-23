/*
    Copyright (C) 1995-2001, The AROS Development Team. All rights reserved.

    Desc: autoinit library - functions sets handling
*/

/*
   Guarded so a build can raise it without editing this file; see
   mmakefile.src for the AUTOINIT_DEBUG shorthand.  These functions run
   between a loaded program's entry point and its main(), a stretch that is
   otherwise silent, so a program that hangs in it produces no output at all.
*/
#ifndef DEBUG
#define DEBUG 0
#endif

#include <aros/symbolsets.h>
#include <aros/debug.h>

int _set_call_funcs(const void * const set[], int direction, int test_fail, struct ExecBase *SysBase)
{
    int pos, (*func)(struct ExecBase *SysBase);

    D(bug("entering set_call_funcs() - %p\n", set));
    
    ForeachElementInSet(set, direction, pos, func)
    {
        D(bug("  %p[%d] %p()", set, pos, func));

        if (test_fail)
        {
            int ret = (*func)(SysBase);
            D(bug(" => %d", ret));
            if (!ret)
                return 0;
        }
        else
        {
            (void)(*func)(SysBase);
        }
        D(bug("\n"));
    }
    
    return 1;
}
