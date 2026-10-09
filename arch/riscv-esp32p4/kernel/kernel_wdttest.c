/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Watchdog failure tests for the esp32p4-riscv target
          (P4_WDT_TEST=<case>, diagnostic only).

    After the boot has settled, one task provokes one failure on one hart.
    The board is expected to reset by itself, the ROM to report
    HP_SYS_HP_WDT_RESET, and the next boot to print that cause. The same
    core then does it again, so a capture sees the cycle repeat.

        1  hart 0: Disable(), then spin
        2  hart 1: Disable(), then spin
        3  hart 0: a store to an address that traps (fatal trap)
        4  hart 1: the same
        5  hart 0: the system alert's dead end, called from a task
        6  hart 1: the same
        7  hart 0: a task above the canary that spins with interrupts on
        8  hart 1: the same
        9  hart 0: Forbid(), then spin, interrupts on
       10  hart 1: the same
       11  a spinlock left held (its owner waits for good), and a task on
           hart 1 that wants it

    Cases 1 to 6 are what the tick alone catches (interrupts masked, the
    hart stopped); 7 to 10 only the canary (kernel_wdtcanary.c) does; 11
    neither: the waiter holds nothing and can be preempted, so the canary
    runs, and only the spinlock's own limit (spinlock.c, krnWdtStuck())
    stops the feeding.
*/

#define __NOLIBBASE__

#include <aros/asmcall.h>
#include <exec/alerts.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include <aros/types/spinlock_s.h>
#include <proto/kernel.h>

#include "hardware.h"
#include "kernel_intern.h"

#if defined(P4_WDT_TEST)

/* _displayalert.c */
void krnDisplayAlert(const char *text, struct KernelBase *KernelBase);

#define WDTEST_SETTLE_SECS      20

#if P4_WDT_TEST == 1 || P4_WDT_TEST == 3 || P4_WDT_TEST == 5 || \
    P4_WDT_TEST == 7 || P4_WDT_TEST == 9 || P4_WDT_TEST == 11
#define WDTEST_HART             0
#else
#define WDTEST_HART             1
#endif

/* Above the canaries' 100 for the cases that must starve them */
#if P4_WDT_TEST >= 7 && P4_WDT_TEST <= 10
#define WDTEST_PRI              120
#else
#define WDTEST_PRI              0
#endif

static struct DosLibrary *DOSBase;
static struct Library *KernelBase;

#if P4_WDT_TEST == 11
static spinlock_t leaked;

/* Takes the lock on hart 1 once the owner on hart 0 has it, and waits */
static void wdtest_waiter(struct ExecBase *SysBase)
{
    krnP4PutStr("[wdtest] waiter on hart 1 asks for the leaked lock\n");
    KrnSpinLock(&leaked, NULL, SPINLOCK_MODE_WRITE);
    krnP4PutStr("[wdtest] the leaked lock came free\n");
}
#endif

static void wdtest_victim(struct ExecBase *SysBase)
{
    krnP4PutStr("[wdtest] case " );
    krnP4PutDec(P4_WDT_TEST);
    krnP4PutStr(" on hart ");
    {
        unsigned long hart;

        asm volatile("csrr %0, mhartid" : "=r"(hart));
        krnP4PutDec((uint32_t)hart);
    }
    krnP4PutStr("\n");

#if P4_WDT_TEST == 1 || P4_WDT_TEST == 2
    Disable();
    for (;;)
        ;
#elif P4_WDT_TEST == 3 || P4_WDT_TEST == 4
    *(volatile ULONG *)1 = 0;
    krnP4PutStr("[wdtest] the store did not trap\n");
#elif P4_WDT_TEST == 7 || P4_WDT_TEST == 8
    for (;;)
        ;
#elif P4_WDT_TEST == 9 || P4_WDT_TEST == 10
    Forbid();
    for (;;)
        ;
#elif P4_WDT_TEST == 11
    {
        void *aff = KrnAllocCPUMask();

        KrnSpinInit(&leaked);
        KrnSpinLock(&leaked, NULL, SPINLOCK_MODE_WRITE);
        if (aff)
        {
            KrnGetCPUMask(1, aff);
            NewCreateTask(TASKTAG_NAME, (IPTR)"wdt waiter",
                          TASKTAG_AFFINITY, (IPTR)aff,
                          TASKTAG_PRI, 0,
                          TASKTAG_PC, (IPTR)wdtest_waiter,
                          TASKTAG_ARG1, (IPTR)SysBase,
                          TAG_DONE);
        }
        Wait(0);                    /* the owner never lets go */
    }
#else
    /* Not Alert(): a task's alert goes to Intuition first and waits for a
       person to answer the requester. This is where the system alert ends. */
    krnDisplayAlert("watchdog test, dead end", NULL);
    krnP4PutStr("[wdtest] krnDisplayAlert() returned\n");
#endif
}

AROS_UFH3(static ULONG, wdtest_runner,
          AROS_UFHA(STRPTR, argstr, A0),
          AROS_UFHA(ULONG, arglen, D0),
          AROS_UFHA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    void *aff;

    (void)argstr;
    (void)arglen;

    KernelBase = OpenResource("kernel.resource");
    aff = KernelBase ? KrnAllocCPUMask() : NULL;

    Delay(WDTEST_SETTLE_SECS * TICKS_PER_SECOND);

    krnP4PutStr("[wdtest] before the failure: ");
    krnWdtReport();

    if (!aff)
        return RETURN_FAIL;
    KrnGetCPUMask(WDTEST_HART, aff);
    if (!NewCreateTask(TASKTAG_NAME, (IPTR)"wdt victim",
                       TASKTAG_AFFINITY, (IPTR)aff,
                       TASKTAG_PRI, WDTEST_PRI,
                       TASKTAG_PC, (IPTR)wdtest_victim,
                       TASKTAG_ARG1, (IPTR)SysBase,
                       TAG_DONE))
        krnP4PutStr("[wdtest] victim not created\n");
    return RETURN_OK;

    AROS_USERFUNC_EXIT
}

extern const struct Resident krnP4WdtTestResident;

AROS_UFH3(static APTR, wdtest_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (!DOSBase)
        return NULL;
    if (!CreateNewProcTags(NP_Entry, (IPTR)wdtest_runner,
                           NP_Name, (IPTR)"WDT test runner",
                           NP_StackSize, 16384,
                           NP_Priority, 0,
                           TAG_DONE))
        krnP4PutStr("[wdtest] runner process not created\n");
    return NULL;

    AROS_USERFUNC_EXIT
}

const struct Resident krnP4WdtTestResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4WdtTestResident,
    (APTR)((const char *)&krnP4WdtTestResident + sizeof(struct Resident)),
    RTF_AFTERDOS,
    1,
    NT_TASK,
    -125,
    "esp32p4 watchdog test",
    "esp32p4 watchdog test 1.0",
    &wdtest_init
};

#endif /* P4_WDT_TEST */
