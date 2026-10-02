/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Opt-in hart-0 FPU context-switch probe for the ESP32-P4.
 *
 * This probe does not test simultaneous execution on multiple harts.  Its
 * two same-priority Exec tasks keep distinct values in every FP register and
 * FCSR while timer interrupts schedule them in and out.  The assembly task
 * publishes progress so each task must observe its peer making progress
 * before that peer finishes; serial execution cannot pass.
 */

#define __KERNEL_NOLIBBASE__

#include <stdint.h>

#include <aros/asmcall.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include "kernel_intern.h"

#ifdef P4_E3_FPU_TEST

#if defined(__AROSEXEC_SMP__)
#error "The hart0 FPU probe uses raw shared counters; use the matched concurrent SMP harness"
#endif

#define P4_FPU_PROBE_TASKS       2U
#define P4_FPU_PROBE_STACK       8192U
#define P4_FPU_PROBE_CREATEFAIL  40U
#define P4_FPU_PROBE_RETURNED    41U

struct P4FpuProbeRecord
{
    uint32_t done;
    uint32_t passed;
    uint32_t detail;
    uint32_t iterations;
    uint32_t dispatch_delta;
    uint32_t ticks;
    uint32_t peer_interleaved;
};

static struct P4FpuProbeRecord p4FpuProbeRecords[P4_FPU_PROBE_TASKS];
static volatile uint32_t p4FpuProbeProgress[P4_FPU_PROBE_TASKS];
static struct Task *p4FpuProbeReporter;

extern int krnP4DiagnosticLine(const char *line);

static char *p4FpuProbeAppend(char *end, const char *text)
{
    while (*text)
        *end++ = *text++;
    return end;
}

static char *p4FpuProbeAppendDecimal(char *end, uint32_t value)
{
    char digits[10];
    unsigned int count = 0;

    do
    {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (count)
        *end++ = digits[--count];
    return end;
}

extern void krnP4FpuProbeTask(uint32_t task_index,
                             volatile uint32_t *dispatch_count,
                             volatile uint32_t *progress,
                             volatile uint32_t *tick_count);

static void p4FpuProbeBlock(void) __attribute__((noreturn));
static void p4FpuProbeTask0Returned(void) __attribute__((noreturn));
static void p4FpuProbeTask1Returned(void) __attribute__((noreturn));
void krnP4FpuProbeFinish(uint32_t task_index, uint32_t passed,
                         uint32_t detail, uint32_t iterations,
                         uint32_t dispatches, uint32_t peer_interleaved,
                         uint32_t ticks) __attribute__((noreturn));

static void p4FpuProbeRecord(uint32_t task_index, uint32_t passed,
                             uint32_t detail, uint32_t iterations,
                             uint32_t dispatch_delta, uint32_t peer_interleaved,
                             uint32_t ticks)
{
    struct P4FpuProbeRecord *record = &p4FpuProbeRecords[task_index];

    record->passed = passed;
    record->detail = detail;
    record->iterations = iterations;
    record->dispatch_delta = dispatch_delta;
    record->peer_interleaved = peer_interleaved;
    record->ticks = ticks;
    __atomic_store_n(&record->done, 1U, __ATOMIC_RELEASE);
}

void krnP4FpuProbeFinish(uint32_t task_index, uint32_t passed,
                         uint32_t detail, uint32_t iterations,
                         uint32_t dispatch_delta, uint32_t peer_interleaved,
                         uint32_t ticks)
{
    if (task_index < P4_FPU_PROBE_TASKS)
    {
        p4FpuProbeRecord(task_index, passed, detail, iterations,
                         dispatch_delta, peer_interleaved, ticks);
        if (p4FpuProbeReporter)
            Signal(p4FpuProbeReporter, SIGBREAKF_CTRL_F);
    }

    /* A worker is one-shot.  It must never return to the FP test loop. */
    RemTask(NULL);
    p4FpuProbeBlock();
}

static void p4FpuProbeTask0Returned(void)
{
    krnP4FpuProbeFinish(0, 0, P4_FPU_PROBE_RETURNED, 0, 0, 0, 0);
}

static void p4FpuProbeTask1Returned(void)
{
    krnP4FpuProbeFinish(1, 0, P4_FPU_PROBE_RETURNED, 0, 0, 0, 0);
}

static void p4FpuProbeBlock(void)
{
    for (;;)
        (void)Wait(0);
}

static char *p4FpuProbeAppendTask(char *end, uint32_t task_index)
{
    const struct P4FpuProbeRecord *record = &p4FpuProbeRecords[task_index];

    end = p4FpuProbeAppend(end, " task");
    end = p4FpuProbeAppendDecimal(end, task_index);
    end = p4FpuProbeAppend(end, "_iterations=");
    end = p4FpuProbeAppendDecimal(end, record->iterations);
    end = p4FpuProbeAppend(end, " task");
    end = p4FpuProbeAppendDecimal(end, task_index);
    end = p4FpuProbeAppend(end, "_ticks=");
    end = p4FpuProbeAppendDecimal(end, record->ticks);
    end = p4FpuProbeAppend(end, " task");
    end = p4FpuProbeAppendDecimal(end, task_index);
    end = p4FpuProbeAppend(end, "_dispatch_delta=");
    end = p4FpuProbeAppendDecimal(end, record->dispatch_delta);
    end = p4FpuProbeAppend(end, " task");
    end = p4FpuProbeAppendDecimal(end, task_index);
    end = p4FpuProbeAppend(end, "_peer_interleaved=");
    end = p4FpuProbeAppendDecimal(end, record->peer_interleaved);
    end = p4FpuProbeAppend(end, " task");
    end = p4FpuProbeAppendDecimal(end, task_index);
    end = p4FpuProbeAppend(end, "_detail=");
    return p4FpuProbeAppendDecimal(end, record->detail);
}

static void p4FpuProbeReporterTask(void)
{
    uint32_t task_index;
    int passed;
    /* Fixed fields plus ten uint32 decimals require fewer than400 bytes. */
    char line[512];
    char *end;

    for (;;)
    {
        (void)Wait(SIGBREAKF_CTRL_F);
        if (__atomic_load_n(&p4FpuProbeRecords[0].done, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&p4FpuProbeRecords[1].done, __ATOMIC_ACQUIRE))
            break;
    }

    passed = 1;
    for (task_index = 0; task_index < P4_FPU_PROBE_TASKS; task_index++)
    {
        const struct P4FpuProbeRecord *record = &p4FpuProbeRecords[task_index];

        if (!record->passed || !record->peer_interleaved ||
            !record->dispatch_delta || !record->ticks)
            passed = 0;
    }

    end = p4FpuProbeAppend(line, "P4_E3_FPU_PROBE=");
    end = p4FpuProbeAppend(end, passed ? "PASS" : "FAIL");
    end = p4FpuProbeAppend(end, " scope=hart0-context-switch-only");
    for (task_index = 0; task_index < P4_FPU_PROBE_TASKS; task_index++)
        end = p4FpuProbeAppendTask(end, task_index);
    end = p4FpuProbeAppend(end, " smp_concurrency=not-tested\n");
    *end = '\0';
    /* No competing task writer; IRQs stay enabled and transport is bounded. */
    Forbid();
    (void)krnP4DiagnosticLine(line);
    Permit();

    RemTask(NULL);
    p4FpuProbeBlock();
}

static struct Task *p4FpuProbeCreateTask(const char *name, APTR entry,
                                         APTR finalizer, IPTR index,
                                         struct ExecBase *SysBase)
{
    struct TagItem tags[] =
    {
        { TASKTAG_NAME,      (IPTR)name },
        { TASKTAG_PC,        (IPTR)entry },
        { TASKTAG_FINALPC,   (IPTR)finalizer },
        { TASKTAG_STACKSIZE, P4_FPU_PROBE_STACK },
        { TASKTAG_PRI,       0 },
        { TASKTAG_ARG1,      index },
        { TASKTAG_ARG2,      (IPTR)&SysBase->DispCount },
        { TASKTAG_ARG3,      (IPTR)p4FpuProbeProgress },
        { TASKTAG_ARG4,      (IPTR)&__esp32p4_ticks },
        { TAG_DONE,          0 }
    };

    return NewCreateTaskA(tags);
}

static struct Task *p4FpuProbeCreateReporter(struct ExecBase *SysBase)
{
    struct TagItem tags[] =
    {
        { TASKTAG_NAME,      (IPTR)"P4 FPU reporter" },
        { TASKTAG_PC,        (IPTR)p4FpuProbeReporterTask },
        { TASKTAG_FINALPC,   (IPTR)p4FpuProbeReporterTask },
        { TASKTAG_STACKSIZE, P4_FPU_PROBE_STACK },
        { TASKTAG_PRI,       0 },
        { TAG_DONE,          0 }
    };

    (void)SysBase;
    return NewCreateTaskA(tags);
}

static void p4FpuProbeStart(struct ExecBase *SysBase)
{
    struct Task *tasks[P4_FPU_PROBE_TASKS];
    uint32_t task_index;
    int create_failed;

    p4FpuProbeReporter = NULL;
    for (task_index = 0; task_index < P4_FPU_PROBE_TASKS; task_index++)
    {
        p4FpuProbeProgress[task_index] = 0;
        p4FpuProbeRecords[task_index].passed = 0;
        p4FpuProbeRecords[task_index].detail = 0;
        p4FpuProbeRecords[task_index].iterations = 0;
        p4FpuProbeRecords[task_index].dispatch_delta = 0;
        p4FpuProbeRecords[task_index].ticks = 0;
        p4FpuProbeRecords[task_index].peer_interleaved = 0;
        __atomic_store_n(&p4FpuProbeRecords[task_index].done, 0U,
                         __ATOMIC_RELAXED);
    }

    /* Do not let a worker run until both worker allocations have succeeded. */
    Forbid();
    p4FpuProbeReporter = p4FpuProbeCreateReporter(SysBase);
    if (!p4FpuProbeReporter)
    {
        Permit();
        krnP4PutStr("P4_E3_FPU_PROBE=FAIL scope=hart0-context-switch-only reason=reporter-create\n");
        return;
    }

    tasks[0] = p4FpuProbeCreateTask("P4 FPU task 0",
                                    (APTR)krnP4FpuProbeTask,
                                    (APTR)p4FpuProbeTask0Returned, 0, SysBase);
    tasks[1] = p4FpuProbeCreateTask("P4 FPU task 1",
                                    (APTR)krnP4FpuProbeTask,
                                    (APTR)p4FpuProbeTask1Returned, 1, SysBase);
    create_failed = !tasks[0] || !tasks[1];

    if (create_failed)
    {
        if (tasks[0])
            RemTask(tasks[0]);
        if (tasks[1])
            RemTask(tasks[1]);

        p4FpuProbeRecord(0, 0, P4_FPU_PROBE_CREATEFAIL, 0, 0, 0, 0);
        p4FpuProbeRecord(1, 0, P4_FPU_PROBE_CREATEFAIL, 0, 0, 0, 0);
        Signal(p4FpuProbeReporter, SIGBREAKF_CTRL_F);
    }
    Permit();
}

extern const struct Resident krnP4FpuProbeResident;

AROS_UFH3(static APTR, krnP4FpuProbeInit,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    (void)dummy;
    (void)segList;
    p4FpuProbeStart(SysBase);
    return NULL;

    AROS_USERFUNC_EXIT
}

const struct Resident krnP4FpuProbeResident =
{
    RTC_MATCHWORD,
    (struct Resident *)&krnP4FpuProbeResident,
    (APTR)((const char *)&krnP4FpuProbeResident + sizeof(struct Resident)),
    RTF_COLDSTART,
    1,
    NT_TASK,
    -39,
    (CONST_STRPTR)"esp32p4 FPU probe",
    (CONST_STRPTR)"esp32p4 FPU context probe 1.0",
    &krnP4FpuProbeInit,
    0,
    NULL
};

#endif /* P4_E3_FPU_TEST */
