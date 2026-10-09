/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SMP coverage for what an ordinary program meets when it runs on
          any CPU instead of the one the system was started on. Processes
          pinned to each CPU, two per CPU, do the things a program does:

          1. open and close libraries and devices (the Open and Close
             vectors and lib_OpenCnt run under Forbid(), which stops only
             the calling CPU),
          2. keep several timer requests outstanding on one reply port and
             WaitIO() them in turn (WaitIO() takes a replied request out
             of the port's list while the timer interrupt on another CPU
             may be queueing the next reply),
          3. read the system time in a loop and check that it never goes
             backwards or jumps (the time is written by the VBlank
             interrupt and read under Disable(), which masks only the
             calling CPU),
          4. clear the cache for a few bytes (CacheClearE(), as LoadSeg()
             does for a loaded hunk) while the other CPU writes to the rest
             of the same cache line, as it does to the header of a
             neighbouring heap block: a clear that writes back and then
             drops the line loses what the other CPU wrote in between,
          5. load and unload a program (LoadSeg() ends in the cache
             maintenance for the instruction side, a hardware unit that
             one CPU at a time may drive).

          Each phase has its own pass criterion; a failure names what
          went wrong. The processes are created with NP_Affinity, so the
          test does not depend on the default affinity of the system.
*/

#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#include <clib/alib_protos.h>

#include <stdio.h>
#include <string.h>

#if defined(__AROSEXEC_SMP__)
#define DEBUG 1
#include <aros/debug.h>

APTR KernelBase;
struct Device *TimerBase;
static int g_NumCPUs = 1;

#define STACKSIZE       16384
#define PER_CPU         2           /* workers per CPU */
#define MAX_WORKERS     (4 * PER_CPU)
#define WAIT_TICKS      3000        /* 60 s bound per phase */

#define OPEN_ITERS      3000
#define BATCH           6           /* outstanding requests on one port */
#define BATCH_ROUNDS    150
#define TIME_SECS       10          /* the time loop runs for this long */
#define TIME_JUMP_US    500000      /* a step ahead that is no preemption */
#define LOADSEG_ITERS   60
#define LOADSEG_FILE    "SYS:C/Assign"

struct Worker
{
    ULONG           w_Phase;
    ULONG           w_Cpu;
    volatile ULONG  w_Done;
    volatile ULONG  w_Errors;
    volatile ULONG  w_Count;
    const char     *w_What;        /* what went wrong, for the report */
};

static struct Worker g_Workers[MAX_WORKERS];

/* Libraries the open/close phase uses: ones that are in memory already, so
   that nothing goes to disk (a library that is not resident makes
   lddemon search LIBS: on every call, which tests the disk, not exec). */
static const char *const g_LibCandidates[] =
{
    "keymap.library", "locale.library", "layers.library", "graphics.library",
    "utility.library", "dos.library", NULL
};
#define MAX_LIBS 3
static const char *g_Libs[MAX_LIBS];
static int g_NLibs;
static int g_Fatal;

static int lib_resident(const char *name)
{
    struct Node *n;

    Forbid();
    n = FindName(&SysBase->LibList, name);
    Permit();
    return n != NULL;
}

static struct Worker *myworker(void)
{
    return (struct Worker *)FindTask(NULL)->tc_UserData;
}

static struct Process *spawn_pinned(const char *name, APTR entry, int cpu,
                                    struct Worker *w)
{
    cpumask_t *mask = KrnAllocCPUMask();

    if (!mask)
        return NULL;
    KrnClearCPUMask(mask);
    KrnGetCPUMask(cpu % g_NumCPUs, mask);
    return CreateNewProcTags(NP_Entry,     (IPTR)entry,
                             NP_Name,      (IPTR)name,
                             NP_StackSize, STACKSIZE,
                             NP_Priority,  0,
                             NP_UserData,  (IPTR)w,
                             NP_Affinity,  (IPTR)mask,
                             TAG_DONE);
}

static int run_phase(const char *name, APTR entry, ULONG phase, int *nworkers)
{
    int n = (g_NumCPUs < 4 ? g_NumCPUs : 4) * PER_CPU;
    int k, t;

    memset(g_Workers, 0, sizeof(g_Workers));
    for (k = 0; k < n; k++)
    {
        g_Workers[k].w_Phase = phase;
        g_Workers[k].w_Cpu = k / PER_CPU;
        if (!spawn_pinned(name, entry, k / PER_CPU, &g_Workers[k]))
        {
            bug("[smpmigrate] %s: INVALID (process create failed)\n", name);
            return -1;
        }
    }

    for (t = 0; t < WAIT_TICKS; t++)
    {
        int done = 0;

        for (k = 0; k < n; k++)
            if (g_Workers[k].w_Done)
                done++;
        if (done == n)
        {
            *nworkers = n;
            return 1;
        }
        Delay(1);
    }
    bug("[smpmigrate] %s: *** TIMEOUT *** (workers stuck)\n", name);
    g_Fatal = 1;
    return 0;
}

static int check_errors(const char *name, int n)
{
    int k, ok = 1;

    for (k = 0; k < n; k++)
        if (g_Workers[k].w_Errors)
        {
            bug("[smpmigrate] %s: *** FAIL *** worker %d (cpu %lu): %lu errors, %s\n",
                name, k, (unsigned long)g_Workers[k].w_Cpu,
                (unsigned long)g_Workers[k].w_Errors,
                g_Workers[k].w_What ? g_Workers[k].w_What : "?");
            ok = 0;
        }
    return ok;
}

/******************************************************************************/
/*  Phase 1: libraries and devices                                            */
/******************************************************************************/

static void OpenWorker(void)
{
    struct Worker *w = myworker();
    ULONG i;
    int k;

    for (i = 0; i < OPEN_ITERS; i++)
    {
        struct Library *l[MAX_LIBS];
        struct timerequest io;
        BOOL dev;

        for (k = 0; k < g_NLibs; k++)
        {
            l[k] = OpenLibrary(g_Libs[k], 0);
            if (!l[k])
            {
                w->w_What = "OpenLibrary failed";
                w->w_Errors++;
            }
        }

        memset(&io, 0, sizeof(io));
        io.tr_node.io_Message.mn_Length = sizeof(io);
        dev = !OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)&io, 0);
        if (dev)
            CloseDevice((struct IORequest *)&io);
        else
        {
            w->w_What = "OpenDevice(timer) failed";
            w->w_Errors++;
        }

        for (k = g_NLibs - 1; k >= 0; k--)
            if (l[k])
                CloseLibrary(l[k]);
        w->w_Count++;
    }
    w->w_Done = 1;
}

static ULONG open_count(const char *name)
{
    struct Library *l;
    ULONG n = 0xffffffff;

    Forbid();
    l = (struct Library *)FindName(&SysBase->LibList, name);
    if (l)
        n = l->lib_OpenCnt;
    Permit();
    return n;
}

static ULONG device_open_count(const char *name)
{
    struct Library *l;
    ULONG n = 0xffffffff;

    Forbid();
    l = (struct Library *)FindName(&SysBase->DeviceList, name);
    if (l)
        n = l->lib_OpenCnt;
    Permit();
    return n;
}

static int TestOpen(void)
{
    ULONG before[MAX_LIBS], after[MAX_LIBS];
    ULONG t0 = device_open_count("timer.device"), t1;
    int n = 0, r, ok, k;

    g_NLibs = 0;
    for (k = 0; g_LibCandidates[k] && g_NLibs < MAX_LIBS; k++)
        if (lib_resident(g_LibCandidates[k]))
            g_Libs[g_NLibs++] = g_LibCandidates[k];
    if (!g_NLibs)
    {
        bug("[smpmigrate] phase 1: INVALID (no resident library to open)\n");
        return -1;
    }
    for (k = 0; k < g_NLibs; k++)
        before[k] = open_count(g_Libs[k]);

    bug("[smpmigrate] phase 1: %d processes x %d open/close of %d libraries "
        "(%s ...) and timer.device...\n",
        (g_NumCPUs < 4 ? g_NumCPUs : 4) * PER_CPU, OPEN_ITERS, g_NLibs,
        g_Libs[0]);

    r = run_phase("smpmigrate.open", OpenWorker, 1, &n);
    if (r <= 0)
        return r;
    ok = check_errors("phase 1", n);

    /* The workers set their done flag before their process ends, and an
       ending process still holds dos.library: let them finish first */
    Delay(100);

    t1 = device_open_count("timer.device");
    if (t1 != t0)
    {
        bug("[smpmigrate] phase 1: *** FAIL *** timer.device open count "
            "%lu -> %lu\n", (unsigned long)t0, (unsigned long)t1);
        ok = 0;
    }
    for (k = 0; k < g_NLibs; k++)
    {
        after[k] = open_count(g_Libs[k]);
        if (after[k] != before[k])
        {
            bug("[smpmigrate] phase 1: *** FAIL *** %s open count %lu -> %lu\n",
                g_Libs[k], (unsigned long)before[k], (unsigned long)after[k]);
            ok = 0;
        }
    }
    if (ok)
        bug("[smpmigrate] phase 1: OK\n");
    return ok;
}

/******************************************************************************/
/*  Phase 2: several replies on one port                                      */
/******************************************************************************/

static void BatchWorker(void)
{
    struct Worker *w = myworker();
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr[BATCH];
    ULONG round;
    int k;

    if (!port)
    {
        w->w_What = "CreateMsgPort failed";
        w->w_Errors++;
        w->w_Done = 1;
        return;
    }
    for (k = 0; k < BATCH; k++)
    {
        tr[k] = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
        if (!tr[k] || OpenDevice("timer.device", UNIT_MICROHZ,
                                 (struct IORequest *)tr[k], 0))
        {
            w->w_What = "request or OpenDevice failed";
            w->w_Errors++;
            w->w_Done = 1;
            return;
        }
    }

    for (round = 0; round < BATCH_ROUNDS; round++)
    {
        /* Staggered so replies arrive while earlier ones are being taken
           out of the port's list */
        for (k = 0; k < BATCH; k++)
        {
            tr[k]->tr_node.io_Command = TR_ADDREQUEST;
            tr[k]->tr_time.tv_secs = 0;
            tr[k]->tr_time.tv_micro = 300 + 400 * k;
            SendIO((struct IORequest *)tr[k]);
        }
        for (k = 0; k < BATCH; k++)
        {
            if (WaitIO((struct IORequest *)tr[k]))
            {
                w->w_What = "WaitIO returned an error";
                w->w_Errors++;
            }
            else
                w->w_Count++;
        }
        /* Nothing may be left on the port */
        if (GetMsg(port))
        {
            w->w_What = "a message was left on the reply port";
            w->w_Errors++;
        }
    }

    for (k = 0; k < BATCH; k++)
    {
        CloseDevice((struct IORequest *)tr[k]);
        DeleteIORequest((struct IORequest *)tr[k]);
    }
    DeleteMsgPort(port);
    w->w_Done = 1;
}

static int TestBatch(void)
{
    int n = 0, r, ok, k;

    bug("[smpmigrate] phase 2: %d processes x %d rounds of %d outstanding "
        "requests on one reply port...\n",
        (g_NumCPUs < 4 ? g_NumCPUs : 4) * PER_CPU, BATCH_ROUNDS, BATCH);

    r = run_phase("smpmigrate.batch", BatchWorker, 2, &n);
    if (r <= 0)
        return r;
    ok = check_errors("phase 2", n);
    for (k = 0; k < n; k++)
        if (g_Workers[k].w_Count != BATCH * BATCH_ROUNDS && !g_Workers[k].w_Errors)
        {
            bug("[smpmigrate] phase 2: *** FAIL *** worker %d completed %lu of %lu\n",
                k, (unsigned long)g_Workers[k].w_Count,
                (unsigned long)(BATCH * BATCH_ROUNDS));
            ok = 0;
        }
    if (ok)
        bug("[smpmigrate] phase 2: OK\n");
    return ok;
}

/******************************************************************************/
/*  Phase 3: the system time                                                  */
/******************************************************************************/

static void TimeWorker(void)
{
    struct Worker *w = myworker();
    struct timeval begin, prev, now;

    GetSysTime(&begin);
    prev = begin;

    for (;;)
    {
        LONG diff;

        GetSysTime(&now);
        /* Seconds and microseconds as one signed difference in
           microseconds, wide enough for a step of a second. A read that
           takes the seconds before a carry and the microseconds after it
           is a second behind; the other way round it is a second ahead. */
        diff = (LONG)(now.tv_secs - prev.tv_secs) * 1000000
             + ((LONG)now.tv_micro - (LONG)prev.tv_micro);
        if (diff < 0)
        {
            w->w_What = "the time went backwards";
            w->w_Errors++;
        }
        else if (diff > TIME_JUMP_US)
        {
            w->w_What = "the time jumped ahead between two reads";
            w->w_Errors++;
        }
        prev = now;
        w->w_Count++;

        if ((LONG)(now.tv_secs - begin.tv_secs) >= TIME_SECS)
            break;
    }
    w->w_Done = 1;
}

static int TestTime(void)
{
    int n = 0, r, ok, k;
    unsigned long reads = 0;
    struct timerequest tr;

    bug("[smpmigrate] phase 3: %d processes reading the system time, "
        "looking for a step backwards or a jump...\n",
        (g_NumCPUs < 4 ? g_NumCPUs : 4) * PER_CPU);

    memset(&tr, 0, sizeof(tr));
    tr.tr_node.io_Message.mn_Length = sizeof(tr);
    if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)&tr, 0))
    {
        bug("[smpmigrate] phase 3: INVALID (timer.device does not open)\n");
        return -1;
    }
    TimerBase = tr.tr_node.io_Device;

    r = run_phase("smpmigrate.time", TimeWorker, 3, &n);
    CloseDevice((struct IORequest *)&tr);
    TimerBase = NULL;
    if (r <= 0)
        return r;
    ok = check_errors("phase 3", n);
    for (k = 0; k < n; k++)
        reads += g_Workers[k].w_Count;
    if (ok)
        bug("[smpmigrate] phase 3: OK (%lu reads)\n", reads);
    return ok;
}

/******************************************************************************/
/*  Phase 4: loading programs                                                 */
/******************************************************************************/

static void LoadWorker(void)
{
    struct Worker *w = myworker();
    ULONG i;

    for (i = 0; i < LOADSEG_ITERS; i++)
    {
        BPTR seg = LoadSeg(LOADSEG_FILE);

        if (!seg)
        {
            w->w_What = "LoadSeg(" LOADSEG_FILE ") failed";
            w->w_Errors++;
        }
        else
        {
            UnLoadSeg(seg);
            w->w_Count++;
        }
    }
    w->w_Done = 1;
}

static int TestLoad(void)
{
    int n = 0, r, ok;
    BPTR probe = LoadSeg(LOADSEG_FILE);

    if (!probe)
    {
        bug("[smpmigrate] phase 5: INVALID (%s does not load)\n", LOADSEG_FILE);
        return -1;
    }
    UnLoadSeg(probe);

    bug("[smpmigrate] phase 5: %d processes x %d LoadSeg/UnLoadSeg of %s...\n",
        (g_NumCPUs < 4 ? g_NumCPUs : 4) * PER_CPU, LOADSEG_ITERS, LOADSEG_FILE);

    r = run_phase("smpmigrate.load", LoadWorker, 4, &n);
    if (r <= 0)
        return r;
    ok = check_errors("phase 5", n);
    if (ok)
        bug("[smpmigrate] phase 5: OK\n");
    return ok;
}

/******************************************************************************/
/*  Phase 5: cache maintenance next to live data                              */
/******************************************************************************/

#define LINE_BYTES      64
#define CACHE_SECS      5

static struct
{
    volatile ULONG *line;       /* one cache line, line aligned */
    volatile ULONG  stop;
    volatile ULONG  syncs;
} g_Cache;

/* The writer keeps a counter in the line's upper half and checks every so
   often that memory still holds what it wrote last */
static void CacheWriter(void)
{
    struct Worker *w = myworker();
    volatile ULONG *counter = &g_Cache.line[LINE_BYTES / 8];  /* offset 32 */
    ULONG local = 0;

    *counter = 0;
    while (!g_Cache.stop)
    {
        ULONG i;

        for (i = 0; i < 64; i++)
            *counter = ++local;
        if (*counter != local)
        {
            w->w_What = "a write to the line was lost";
            w->w_Errors++;
            *counter = local;
        }
        w->w_Count++;
    }
    w->w_Done = 1;
}

/* The syncer clears the cache for eight bytes at the line's start, the
   way LoadSeg() does for the first bytes of a hunk next to a block header */
static void CacheSyncer(void)
{
    struct Worker *w = myworker();

    while (!g_Cache.stop)
    {
        CacheClearE((APTR)g_Cache.line, 8, CACRF_ClearI | CACRF_ClearD);
        g_Cache.syncs++;
    }
    w->w_Done = 1;
}

static int TestCacheLine(void)
{
    UBYTE *raw = AllocMem(4 * LINE_BYTES, MEMF_ANY | MEMF_CLEAR);
    int dir, ok = 1;

    if (!raw)
    {
        bug("[smpmigrate] phase 4: INVALID (no memory)\n");
        return -1;
    }
    g_Cache.line = (volatile ULONG *)(((IPTR)raw + LINE_BYTES - 1) & ~(IPTR)(LINE_BYTES - 1));

    bug("[smpmigrate] phase 4: CacheClearE next to another CPU's writes, "
        "%d s each way...\n", CACHE_SECS);

    for (dir = 0; dir < 2; dir++)
    {
        struct Worker *writer = &g_Workers[0], *syncer = &g_Workers[1];
        ULONG t;

        memset(g_Workers, 0, sizeof(g_Workers));
        g_Cache.stop = 0;
        g_Cache.syncs = 0;
        writer->w_Cpu = dir;
        syncer->w_Cpu = 1 - dir;
        if (!spawn_pinned("smpmigrate.cwrite", CacheWriter, dir, writer) ||
            !spawn_pinned("smpmigrate.csync", CacheSyncer, 1 - dir, syncer))
        {
            bug("[smpmigrate] phase 4: INVALID (process create failed)\n");
            g_Cache.stop = 1;
            FreeMem(raw, 4 * LINE_BYTES);
            return -1;
        }
        for (t = 0; t < CACHE_SECS * 50; t++)
            Delay(1);
        g_Cache.stop = 1;
        for (t = 0; t < 500 && !(writer->w_Done && syncer->w_Done); t++)
            Delay(1);
        if (!(writer->w_Done && syncer->w_Done))
        {
            bug("[smpmigrate] phase 4: *** TIMEOUT *** (workers stuck)\n");
            g_Fatal = 1;
            return 0;
        }
        if (writer->w_Errors)
        {
            bug("[smpmigrate] phase 4: *** FAIL *** writer on cpu %d, syncer on cpu %d: "
                "%lu lost writes in %lu clears\n", dir, 1 - dir,
                (unsigned long)writer->w_Errors, (unsigned long)g_Cache.syncs);
            ok = 0;
        }
        else
            bug("[smpmigrate] phase 4: writer on cpu %d, syncer on cpu %d: "
                "no write lost in %lu clears\n", dir, 1 - dir,
                (unsigned long)g_Cache.syncs);
        Delay(50);
    }

    FreeMem(raw, 4 * LINE_BYTES);
    if (ok)
        bug("[smpmigrate] phase 4: OK\n");
    return ok;
}

int main(void)
{
    int pass = 0, fail = 0, inval = 0;
    int r;

    KernelBase = OpenResource("kernel.resource");
    if (KernelBase)
        g_NumCPUs = KrnGetCPUCount();

    bug("[smpmigrate] start: cpus=%ld\n", (LONG)g_NumCPUs);
    if (g_NumCPUs < 2)
    {
        bug("[smpmigrate] DONE: needs two CPUs, 0 PASS, 0 FAIL, 1 INVALID\n");
        return RETURN_OK;
    }

    /* A phase that times out leaves its workers running; going on would
       mix their failure into the next phase's */
    r = TestOpen();   if (r > 0) pass++; else if (!r) fail++; else inval++;
    if (!g_Fatal) { r = TestBatch(); if (r > 0) pass++; else if (!r) fail++; else inval++; }
    if (!g_Fatal) { r = TestTime();  if (r > 0) pass++; else if (!r) fail++; else inval++; }
    if (!g_Fatal) { r = TestCacheLine(); if (r > 0) pass++; else if (!r) fail++; else inval++; }
    if (!g_Fatal) { r = TestLoad();  if (r > 0) pass++; else if (!r) fail++; else inval++; }

    bug("[smpmigrate] DONE: %d PASS, %d FAIL, %d INVALID\n", pass, fail, inval);
    return fail ? RETURN_FAIL : RETURN_OK;
}
#else
int main(void)
{
    return RETURN_FAIL;
}
#endif
