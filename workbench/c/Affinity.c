/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Show or change the CPUs a shell or a program may run on.
*/

/**************************************************************************

    NAME
        Affinity

    FORMAT
        Affinity [<cpus>] [<command line>]

    SYNOPSIS
        CPU,COMMAND/F

    LOCATION
        C:

    FUNCTION
        Shows or changes the set of CPUs that the shell, or a program
        started from it, may run on.

        On an SMP system a new process gets the CPUs of the process that
        created it, and the shell and the programs Workbench starts stay on
        the CPU the system started on. Much of the system protects its data
        in ways that only hold on one CPU, so a program is given more CPUs
        only when it is known to cope.

        Without arguments, Affinity shows the CPUs this shell may run on
        and the one it runs on now.

        With <cpus> alone, it changes the shell's own set. Every program
        started from the shell afterwards inherits it.

        With <cpus> and a command line, it runs the command line in a new
        shell process that may use those CPUs, waits for it to end and
        returns its result. The shell's own set stays as it is.

        <cpus> is ANY for every CPU, or CPU numbers separated by commas.

    EXAMPLE
        Affinity ANY Lame song.wav song.mp3
            Encodes on whichever CPU is free.

        Affinity 1
            The shell, and what it starts, runs on CPU 1 from now on.

        Affinity
            Shows the shell's CPUs.

    NOTES
        The parts of the system that are not safe for several CPUs are
        Intuition, graphics, layers and the input devices. A program that
        opens windows should stay on CPU 0; command line programs that
        compute and read and write files are the ones to give more.

        On a system that is not SMP the command only reports that.

    SEE ALSO
        Run, ChangeTaskPri

**************************************************************************/

#include <exec/types.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

const TEXT version[] = "$VER: Affinity 1.0 (09.10.2026)";

#define ARG_TEMPLATE    "CPU,COMMAND/F"
#define ARG_CPU         0
#define ARG_COMMAND     1
#define ARG_COUNT       2

#if defined(__AROSEXEC_SMP__)

#include <resources/task.h>
#include <proto/kernel.h>
#include <proto/task.h>

APTR KernelBase;
APTR TaskResBase;

/* "ANY" in any case, without utility.library */
static BOOL is_any(CONST_STRPTR s)
{
    return s && (s[0] | 0x20) == 'a' && (s[1] | 0x20) == 'n' &&
           (s[2] | 0x20) == 'y' && s[3] == '\0';
}

/*
 * A set of CPUs from the argument: ANY, or numbers separated by commas.
 * Returns TASKAFFINITY_ANY, a fresh KrnAllocCPUMask() mask, or NULL after
 * reporting what was wrong.
 */
static APTR parse_cpus(CONST_STRPTR arg, int count)
{
    APTR mask;
    CONST_STRPTR p = arg;
    BOOL some = FALSE;

    if (is_any(arg))
        return (APTR)TASKAFFINITY_ANY;

    mask = KrnAllocCPUMask();
    if (!mask)
    {
        PutStr("Affinity: out of memory\n");
        return NULL;
    }
    KrnClearCPUMask(mask);

    while (*p)
    {
        LONG cpu;
        LONG used = StrToLong(p, &cpu);

        if (used <= 0 || cpu < 0 || cpu >= count)
        {
            Printf("Affinity: '%s' is not ANY or a list of CPUs 0 to %ld\n",
                   arg, (LONG)(count - 1));
            KrnFreeCPUMask(mask);
            return NULL;
        }
        KrnGetCPUMask(cpu, mask);
        some = TRUE;
        p += used;
        if (*p == ',')
            p++;
        else if (*p)
        {
            Printf("Affinity: '%s' is not ANY or a list of CPUs 0 to %ld\n",
                   arg, (LONG)(count - 1));
            KrnFreeCPUMask(mask);
            return NULL;
        }
    }

    if (!some)
    {
        KrnFreeCPUMask(mask);
        return NULL;
    }
    return mask;
}

static void show(int count)
{
    APTR mask = KrnAllocCPUMask();
    IPTR now = 0;
    struct TagItem tags[] =
    {
        { TaskTag_CPUAffinity, (IPTR)mask },
        { TaskTag_CPUNumber,   (IPTR)&now },
        { TAG_DONE,            0          }
    };
    int cpu, shown = 0;

    if (!mask)
        return;
    KrnClearCPUMask(mask);
    QueryTaskTagList(FindTask(NULL), tags);

    PutStr("CPUs ");
    for (cpu = 0; cpu < count; cpu++)
    {
        if (KrnCPUInMask(cpu, mask))
        {
            Printf(shown ? ",%ld" : "%ld", (LONG)cpu);
            shown++;
        }
    }
    if (shown == count)
        PutStr(" (all)");
    Printf(", running on CPU %ld\n", (LONG)now);
    KrnFreeCPUMask(mask);
}

int main(void)
{
    IPTR args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    int count, rc = RETURN_OK;
    APTR mask;

    rda = ReadArgs(ARG_TEMPLATE, args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "Affinity");
        return RETURN_FAIL;
    }

    KernelBase = OpenResource("kernel.resource");
    TaskResBase = OpenResource("task.resource");
    if (!KernelBase || !TaskResBase)
    {
        PutStr("Affinity: kernel.resource or task.resource missing\n");
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    count = KrnGetCPUCount();

    if (!args[ARG_CPU])
    {
        show(count);
        FreeArgs(rda);
        return RETURN_OK;
    }

    mask = parse_cpus((CONST_STRPTR)args[ARG_CPU], count);
    if (!mask)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (!args[ARG_COMMAND])
    {
        /* The shell's own set; SetTaskAffinity() copies the mask */
        if (!SetTaskAffinity(NULL, mask))
        {
            PutStr("Affinity: the set was not accepted\n");
            rc = RETURN_ERROR;
        }
        if (mask != (APTR)TASKAFFINITY_ANY)
            KrnFreeCPUMask(mask);
    }
    else
    {
        /* NP_Affinity hands the mask to the new process, which frees it */
        LONG result = SystemTags((CONST_STRPTR)args[ARG_COMMAND],
                                 SYS_Input,   (IPTR)Input(),
                                 SYS_Output,  (IPTR)Output(),
                                 NP_Affinity, (IPTR)mask,
                                 TAG_DONE);

        if (result == -1)
        {
            /* Whether the mask was handed over before the failure is not
               known here; a leaked mask is better than one freed twice */
            PrintFault(IoErr(), "Affinity");
            rc = RETURN_FAIL;
        }
        else
            rc = result;
    }

    FreeArgs(rda);
    return rc;
}

#else /* !__AROSEXEC_SMP__ */

int main(void)
{
    IPTR args[ARG_COUNT] = { 0 };
    struct RDArgs *rda = ReadArgs(ARG_TEMPLATE, args, NULL);

    if (rda)
        FreeArgs(rda);
    PutStr("Affinity: this is not an SMP system; everything runs on CPU 0\n");
    return RETURN_WARN;
}

#endif /* __AROSEXEC_SMP__ */
