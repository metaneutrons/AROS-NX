/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: S5 test runner for the smp variant of the esp32p4-riscv target
          (P4_S5_RUNNER=1, diagnostic only).

    Runs upstream's SMP tests (developer/debug/test/smp) from the flash
    development volume after the boot, without a person at the board and
    without a new card. The list is FLASHDISK0P0:S5/tests, one program per
    line with its arguments, '#' for a comment; the programs sit beside it
    (image/mmakefile.src, P4_S5_TESTS=1). Changing what runs needs only a
    new development volume.

    The tests report through bug(), which reaches the console; the runner
    adds a line per test with its return code and duration. A test that
    hangs stops the list there, and its last line on the console says
    where.
*/

#define __NOLIBBASE__

#include <aros/asmcall.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/resident.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "tls.h"

#if defined(P4_S5_RUNNER)

#define S5_LIST         "FLASHDISK0P0:S5/tests"
#define S5_DIR          "FLASHDISK0P0:S5/"
#define S5_SETTLE_SECS  20
#define S5_STACK        65536

static struct DosLibrary *DOSBase;

static void put_secs(uint64_t ticks)
{
    uint64_t ms = ticks / (P4_SYSTIMER_HZ / 1000);

    krnP4PutDec((uint32_t)(ms / 1000));
    krnP4PutStr(".");
    krnP4PutDec((uint32_t)((ms % 1000) / 100));
    krnP4PutStr(" s");
}

/* One line of the list: the program, then its arguments. FALSE at the end */
static BOOL next_test(BPTR list, char *line, ULONG size, char **args)
{
    char *p;

    for (;;)
    {
        if (!FGets(list, (STRPTR)line, size))
            return FALSE;
        for (p = line; *p == ' ' || *p == '\t'; p++)
            ;
        if (*p && *p != '\n' && *p != '#')
            break;
    }
    if (p != line)
        memmove(line, p, strlen(p) + 1);

    /* The argument string keeps its newline, which ReadArgs() needs */
    for (p = line; *p && *p != ' ' && *p != '\t' && *p != '\n'; p++)
        ;
    if (*p == '\n' || !*p)
    {
        *p = '\0';
        *args = "\n";
    }
    else
    {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        *args = p;
    }
    /* A last line without its newline still needs one */
    if (!strchr(*args, '\n'))
    {
        if (**args && strlen(line) + 1 + strlen(*args) + 1 < size)
            strcat(*args, "\n");
        else
            *args = "\n";
    }
    return TRUE;
}

static void run_one(const char *name, char *args, ULONG *runs, ULONG *bad)
{
    char path[96];
    BPTR seg;
    LONG rc;
    uint64_t start;

    if (strlen(S5_DIR) + strlen(name) >= sizeof(path))
        return;
    strcpy(path, S5_DIR);
    strcat(path, name);

    krnP4PutStr("[smp-s5] run ");
    krnP4PutStr(name);
    krnP4PutStr(" ");
    krnP4PutStr(args);              /* ends in its newline */

    seg = LoadSeg((CONST_STRPTR)path);
    if (!seg)
    {
        krnP4PutStr("[smp-s5] ");
        krnP4PutStr(name);
        krnP4PutStr(" did not load, IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        (*bad)++;
        return;
    }

    start = krnTimerCount();
    rc = RunCommand(seg, S5_STACK, (STRPTR)args, strlen(args));
    (*runs)++;

    krnP4PutStr("[smp-s5] ");
    krnP4PutStr(name);
    krnP4PutStr(" rc=");
    krnP4PutDecS((int32_t)rc);
    krnP4PutStr(" after ");
    put_secs(krnTimerCount() - start);
    krnP4PutStr("\n");
    if (rc != RETURN_OK)
        (*bad)++;

    UnLoadSeg(seg);
}

AROS_UFH3(static ULONG, s5_runner,
          AROS_UFHA(STRPTR, argstr, A0),
          AROS_UFHA(ULONG, arglen, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    char line[128];
    char *args;
    ULONG runs = 0, bad = 0;
    BPTR list;

    (void)argstr;
    (void)arglen;

    /* Let the boot finish: Wanderer, IPrefs and the drivers settle first */
    Delay(S5_SETTLE_SECS * TICKS_PER_SECOND);

    list = Open((CONST_STRPTR)S5_LIST, MODE_OLDFILE);
    if (!list)
    {
        krnP4PutStr("[smp-s5] no list " S5_LIST ", IoErr ");
        krnP4PutDecS((int32_t)IoErr());
        krnP4PutStr("\n");
        return RETURN_FAIL;
    }

    krnP4PutStr("[smp-s5] start, cpus ");
    krnP4PutDec((uint32_t)__builtin_popcount(
        __atomic_load_n(&__p4_harts_online, __ATOMIC_ACQUIRE)));
    krnP4PutStr("\n");

    while (next_test(list, line, sizeof(line), &args))
        run_one(line, args, &runs, &bad);
    Close(list);

    krnP4PutStr("[smp-s5] wdt: ");
    krnWdtReport();
    krnP4PutStr("[smp-s5] done, ");
    krnP4PutDec(runs);
    krnP4PutStr(" run, ");
    krnP4PutDec(bad);
    krnP4PutStr(bad ? " not RETURN_OK\n" : " not RETURN_OK; all returned 0\n");
    return RETURN_OK;

    AROS_USERFUNC_EXIT
}

extern const struct Resident krnP4S5Resident;

/*
 * RTF_AFTERDOS inside dos.library's boot process, which must not wait: the
 * runner is a process of its own, with a CLI structure, so that the C
 * startup of the tests takes the Shell path rather than waiting for a
 * Workbench message (kernel_startup.c, the A5 probe).
 */
AROS_UFH3(static APTR, s5_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    struct Process *proc;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (!DOSBase)
    {
        krnP4PutStr("[smp-s5] dos.library unavailable\n");
        return NULL;
    }

    proc = CreateNewProcTags(NP_Entry, (IPTR)s5_runner,
                             NP_Name, (IPTR)"SMP-S5 runner",
                             NP_Cli, TRUE,
                             NP_StackSize, S5_STACK,
                             NP_Priority, 0,
                             TAG_DONE);
    if (!proc)
        krnP4PutStr("[smp-s5] runner process not created\n");

    /* DOSBase stays open: the runner uses it */
    return NULL;

    AROS_USERFUNC_EXIT
}

/* After lddemon, shell and shellcommands at -123, as the A5 probe */
const struct Resident krnP4S5Resident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4S5Resident,
    (APTR)((const char *)&krnP4S5Resident + sizeof(struct Resident)),
    RTF_AFTERDOS,
    1,
    NT_TASK,
    -126,
    "esp32p4 S5 runner",
    "esp32p4 S5 runner 1.0",
    &s5_init
};

#endif /* P4_S5_RUNNER */
