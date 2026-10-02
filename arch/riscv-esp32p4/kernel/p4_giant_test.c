/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * Headless hardware self-test of the transitional Giant (P4_GIANT_TEST=1).
 *
 * Runs in exec's boot task on hart 0 after hart 1 came online and before
 * InitCode(RTF_COLDSTART), then returns so that the normal boot follows
 * with hart 1 still online. Every wait is bounded by the tick; a stuck
 * stage prints FAIL and the run goes on. Only the boot task prints.
 *
 * Output: one "[giant-test] <name> PASS|FAIL ..." line per stage, then
 * "[giant-test] SUMMARY pass=<n> fail=<n>" and the per-hart counters.
 */

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "p4_giant.h"

#define T_HART0     0
#define T_HART1     (P4_TF_HART1_OK | P4_TF_HART0_NO)
#define T_ANY       P4_TF_HART1_OK

#define STACK       8192
#define TIMEOUT     (10 * P4_TICK_HZ)   /* ten seconds per wait */

extern volatile unsigned long __esp32p4_ticks;

static unsigned int passed, failed;

struct worker
{
    struct Task *task;
    volatile uint32_t started;
    volatile uint32_t done;
    volatile uint32_t count;
    volatile uint32_t errors;
    volatile uint32_t harts;            /* bit per hart it ran on */
    volatile uint32_t stop;
    uint32_t arg;
    float fresult;
    struct MsgPort *port;
    BYTE sig;
};

static struct worker workers[4];
static struct Task *boot_task;
static BYTE boot_sig;

static inline void note_hart(struct worker *w)
{
    w->harts |= 1u << p4_giant_hart();
}

static void spin_cycles(unsigned int n)
{
    while (n--)
        __asm__ volatile("nop");
}

static void report(const char *name, int ok, const char *what, uint32_t a,
                   const char *what2, uint32_t b)
{
    krnP4PutStr("[giant-test] ");
    krnP4PutStr(name);
    krnP4PutStr(ok ? " PASS" : " FAIL");
    if (what)
    {
        krnP4PutStr(" ");
        krnP4PutStr(what);
        krnP4PutStr("=");
        krnP4PutDec(a);
    }
    if (what2)
    {
        krnP4PutStr(" ");
        krnP4PutStr(what2);
        krnP4PutStr("=");
        krnP4PutDec(b);
    }
    krnP4PutStr("\n");
    if (ok)
        passed++;
    else
        failed++;
}

/* Bounded wait for a flag, polling; the boot task keeps hart 0 busy, but
   equal-priority hart 0 tasks still get their quantum. */
static int wait_flag(volatile uint32_t *flag, uint32_t value)
{
    unsigned long until = __esp32p4_ticks + TIMEOUT;

    while (*flag != value)
        if ((long)(__esp32p4_ticks - until) > 0)
            return 0;
    return 1;
}

/*
 * NewAddTask() clears tc_Flags, so a worker starts at the lowest priority
 * on hart 0 only, gets its hart bits under Disable(), and then its real
 * priority; SetTaskPri() reinserts it into TaskReady and wakes hart 1.
 */
static struct Task *spawn(struct worker *w, const char *name, BYTE pri,
                          UBYTE harts, void (*entry)(void))
{
    struct Task *t = AllocMem(sizeof(struct Task), MEMF_CLEAR | MEMF_PUBLIC);
    APTR stack = AllocMem(STACK, MEMF_CLEAR);
    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry),
                                  MEMF_CLEAR | MEMF_PUBLIC);

    if (!t || !stack || !ml)
        return NULL;
    /* Exec's housekeeper frees this list once the task has removed
       itself; the list's own node is freed with it. */
    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Addr = t;
    ml->ml_ME[0].me_Length = sizeof(struct Task);
    ml->ml_ME[1].me_Addr = stack;
    ml->ml_ME[1].me_Length = STACK;
    NEWLIST(&t->tc_MemEntry);
    AddHead(&t->tc_MemEntry, &ml->ml_Node);
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri  = -128;
    t->tc_Node.ln_Name = (char *)name;
    t->tc_SPLower      = stack;
    t->tc_SPUpper      = (APTR)((IPTR)stack + STACK);
    t->tc_UserData     = w;
    w->task = t;
    w->started = w->done = w->count = w->errors = w->harts = w->stop = 0;
    if (!AddTask(t, (APTR)entry, NULL))
        return NULL;
    Disable();
    t->tc_Flags |= harts;
    Enable();
    SetTaskPri(t, pri);
    return t;
}

static struct worker *self(void)
{
    return (struct worker *)FindTask(NULL)->tc_UserData;
}

static void finish(struct worker *w)
{
    w->done = 1;
    RemTask(NULL);
    for (;;)
        ;
}

/* ------------------------------------------------------- concurrency */

static void w_count(void)
{
    struct worker *w = self();

    w->started = 1;
    while (!w->stop)
    {
        w->count++;
        if (p4_giant_hart() != 1)
            w->errors++;
        note_hart(w);
    }
    finish(w);
}

static void t_concurrency(void)
{
    struct worker *w = &workers[0];
    uint32_t c0 = 0, before, during = 0, samples = 0;
    unsigned long until;

    if (!spawn(w, "giant count h1", 0, T_HART1, w_count) ||
        !wait_flag(&w->started, 1))
    {
        report("concurrency", 0, "started", w->started, NULL, 0);
        return;
    }
    /* The boot task runs on hart 0 throughout. Any progress of a task
       that may not run on hart 0 during this window is hart 1's. */
    before = w->count;
    until = __esp32p4_ticks + P4_TICK_HZ / 2;
    while ((long)(__esp32p4_ticks - until) < 0)
    {
        if ((++c0 & 0x3ff) == 0)
        {
            uint32_t now = w->count;
            if (now != before)
                during++;
            before = now;
            samples++;
        }
    }
    w->stop = 1;
    if (!wait_flag(&w->done, 1))
    {
        report("concurrency", 0, "done", 0, NULL, 0);
        return;
    }
    report("concurrency", w->errors == 0 && w->harts == 2 &&
           samples > 100 && during * 10 >= samples * 9,
           "samples_advancing", during, "of", samples);
    krnP4PutStr("[giant-test] concurrency hart0_loops=");
    krnP4PutDec(c0);
    krnP4PutStr(" hart1_count=");
    krnP4PutDec(w->count);
    krnP4PutStr(" wrong_hart=");
    krnP4PutDec(w->errors);
    krnP4PutStr("\n");
}

/* --------------------------------------------------- Forbid / Disable */

static volatile uint32_t shared, inside;

#define EXCL_ROUNDS 20000

static void excl_body(struct worker *w, int use_disable)
{
    unsigned int i;

    for (i = 0; i < EXCL_ROUNDS; i++)
    {
        uint32_t v;

        if (use_disable)
            Disable();
        else
            Forbid();
        if (__atomic_exchange_n(&inside, 1u, __ATOMIC_SEQ_CST))
            w->errors++;
        v = shared;
        spin_cycles(20);
        shared = v + 1;
        __atomic_store_n(&inside, 0u, __ATOMIC_SEQ_CST);
        if (use_disable)
            Enable();
        else
            Permit();
        note_hart(w);
    }
}

static void w_excl(void)
{
    struct worker *w = self();

    w->started = 1;
    excl_body(w, (int)w->arg);
    finish(w);
}

static void t_exclusion(const char *name, int use_disable)
{
    struct worker *w = &workers[0];
    struct worker me = { 0 };

    shared = 0;
    inside = 0;
    w->arg = (uint32_t)use_disable;
    if (!spawn(w, use_disable ? "giant disable h1" : "giant forbid h1", 0,
               T_HART1, w_excl) || !wait_flag(&w->started, 1))
    {
        report(name, 0, "started", 0, NULL, 0);
        return;
    }
    excl_body(&me, use_disable);
    if (!wait_flag(&w->done, 1))
    {
        report(name, 0, "done", 0, NULL, 0);
        return;
    }
    report(name, shared == 2 * EXCL_ROUNDS && w->errors == 0 &&
           me.errors == 0 && w->harts == 2,
           "total", shared, "overlaps", w->errors + me.errors);
}

/* --------------------------------------------------------- signals */

#define PING_ROUNDS 2000

static void w_ping(void)
{
    struct worker *w = self();
    ULONG mask;
    unsigned int i;

    w->sig = AllocSignal(-1);
    w->started = 1;
    if (w->sig < 0)
    {
        w->errors++;
        finish(w);
    }
    mask = 1UL << w->sig;
    for (i = 0; i < PING_ROUNDS; i++)
    {
        if (Wait(mask) != mask)
            w->errors++;
        note_hart(w);
        w->count++;
        Signal(boot_task, 1UL << boot_sig);
    }
    finish(w);
}

static void t_ping(void)
{
    struct worker *w = &workers[0];
    unsigned long t0;
    unsigned int i;
    ULONG mine;

    if (!spawn(w, "giant ping h1", 1, T_HART1, w_ping) ||
        !wait_flag(&w->started, 1) || w->sig < 0)
    {
        report("signal-pingpong", 0, "started", 0, NULL, 0);
        return;
    }
    mine = 1UL << boot_sig;
    SetSignal(0, mine);
    t0 = __esp32p4_ticks;
    for (i = 0; i < PING_ROUNDS; i++)
    {
        Signal(w->task, 1UL << w->sig);
        if (Wait(mine) != mine)
            w->errors++;
    }
    if (!wait_flag(&w->done, 1))
    {
        report("signal-pingpong", 0, "done", 0, NULL, 0);
        return;
    }
    report("signal-pingpong", w->count == PING_ROUNDS && w->errors == 0 &&
           w->harts == 2, "rounds", w->count,
           "ticks", (uint32_t)(__esp32p4_ticks - t0));
}

/* ------------------------------------------------------- semaphore */

static struct SignalSemaphore sem;

#define SEM_ROUNDS 5000

static void sem_body(struct worker *w)
{
    unsigned int i;

    for (i = 0; i < SEM_ROUNDS; i++)
    {
        uint32_t v;

        ObtainSemaphore(&sem);
        if (__atomic_exchange_n(&inside, 1u, __ATOMIC_SEQ_CST))
            w->errors++;
        v = shared;
        spin_cycles(200);
        shared = v + 1;
        __atomic_store_n(&inside, 0u, __ATOMIC_SEQ_CST);
        ReleaseSemaphore(&sem);
        note_hart(w);
    }
}

static void w_sem(void)
{
    struct worker *w = self();

    w->started = 1;
    sem_body(w);
    finish(w);
}

static void t_semaphore(void)
{
    struct worker *w = &workers[0];
    struct worker me = { 0 };

    InitSemaphore(&sem);
    shared = 0;
    inside = 0;
    if (!spawn(w, "giant sem h1", 0, T_HART1, w_sem) ||
        !wait_flag(&w->started, 1))
    {
        report("semaphore", 0, "started", 0, NULL, 0);
        return;
    }
    sem_body(&me);
    if (!wait_flag(&w->done, 1))
    {
        report("semaphore", 0, "done", 0, NULL, 0);
        return;
    }
    report("semaphore", shared == 2 * SEM_ROUNDS && !w->errors && !me.errors &&
           w->harts == 2, "total", shared, "overlaps", w->errors + me.errors);
}

/* --------------------------------------------------------- messages */

#define MSG_COUNT 1000

struct test_msg
{
    struct Message msg;
    uint32_t seq;
};

static void w_msg(void)
{
    struct worker *w = self();
    uint32_t expect = 0;

    w->port = CreateMsgPort();
    w->started = 1;
    if (!w->port)
    {
        w->errors++;
        finish(w);
    }
    while (expect < MSG_COUNT)
    {
        struct test_msg *m;

        WaitPort(w->port);
        while ((m = (struct test_msg *)GetMsg(w->port)) != NULL)
        {
            if (m->seq != expect++)
                w->errors++;
            note_hart(w);
            w->count++;
            ReplyMsg(&m->msg);
        }
    }
    /* No sender remains; the port can go. */
    DeleteMsgPort(w->port);
    w->port = NULL;
    finish(w);
}

static void t_messages(void)
{
    struct worker *w = &workers[0];
    struct MsgPort *reply = CreateMsgPort();
    struct test_msg *msgs = AllocMem(sizeof(*msgs) * 8, MEMF_CLEAR | MEMF_PUBLIC);
    uint32_t sent = 0, back = 0;
    unsigned long until;

    if (!reply || !msgs ||
        !spawn(w, "giant msg h1", 0, T_HART1, w_msg) ||
        !wait_flag(&w->started, 1) || !w->port)
    {
        report("messages", 0, "started", 0, NULL, 0);
        return;
    }
    /* Eight messages in flight, each reused when it comes back. */
    for (; sent < 8; sent++)
    {
        msgs[sent].msg.mn_ReplyPort = reply;
        msgs[sent].msg.mn_Length = sizeof(struct test_msg);
        msgs[sent].seq = sent;
        PutMsg(w->port, &msgs[sent].msg);
    }
    until = __esp32p4_ticks + TIMEOUT;
    while (back < MSG_COUNT && (long)(__esp32p4_ticks - until) < 0)
    {
        struct test_msg *m;

        WaitPort(reply);
        while ((m = (struct test_msg *)GetMsg(reply)) != NULL)
        {
            back++;
            if (sent < MSG_COUNT)
            {
                m->seq = sent++;
                PutMsg(w->port, &m->msg);
            }
        }
    }
    if (back != MSG_COUNT || !wait_flag(&w->done, 1))
    {
        report("messages", 0, "replies", back, NULL, 0);
        return;
    }
    report("messages", w->count == MSG_COUNT && w->errors == 0 &&
           w->harts == 2, "received", w->count, "seq_errors", w->errors);
    FreeMem(msgs, sizeof(*msgs) * 8);
    DeleteMsgPort(reply);
}

/* ---------------------------------------------------------- memory */

#define MEM_ROUNDS 3000
#define MEM_LIVE   16

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void mem_body(struct worker *w, uint32_t seed)
{
    UBYTE *live[MEM_LIVE] = { 0 };
    ULONG size[MEM_LIVE] = { 0 };
    UBYTE tag[MEM_LIVE] = { 0 };
    unsigned int i, j, k;

    for (i = 0; i < MEM_ROUNDS; i++)
    {
        k = lcg(&seed) % MEM_LIVE;

        if (live[k])
        {
            for (j = 0; j < size[k]; j++)
                if (live[k][j] != tag[k])
                {
                    w->errors++;
                    break;
                }
            FreeMem(live[k], size[k]);
            live[k] = NULL;
        }
        size[k] = 16 + lcg(&seed) % 4080;
        tag[k] = (UBYTE)(lcg(&seed) | 1);
        live[k] = AllocMem(size[k], MEMF_ANY);
        if (!live[k])
        {
            w->errors++;
            continue;
        }
        for (j = 0; j < size[k]; j++)
            live[k][j] = tag[k];
        note_hart(w);
    }
    for (k = 0; k < MEM_LIVE; k++)
        if (live[k])
            FreeMem(live[k], size[k]);
}

static void w_mem(void)
{
    struct worker *w = self();

    w->started = 1;
    mem_body(w, 0x1234567u);
    finish(w);
}

static void t_memory(void)
{
    struct worker *w = &workers[0];
    struct worker me = { 0 };
    ULONG avail = AvailMem(MEMF_ANY);

    if (!spawn(w, "giant mem h1", 0, T_HART1, w_mem) ||
        !wait_flag(&w->started, 1))
    {
        report("memory", 0, "started", 0, NULL, 0);
        return;
    }
    mem_body(&me, 0x89abcdefu);
    if (!wait_flag(&w->done, 1))
    {
        report("memory", 0, "done", 0, NULL, 0);
        return;
    }
    report("memory", w->errors == 0 && me.errors == 0 && w->harts == 2,
           "errors", w->errors + me.errors,
           "avail_delta", (uint32_t)(avail - AvailMem(MEMF_ANY)));
}

/* ------------------------------------------------------------- FPU */

#define FPU_ITER 20000000

static __attribute__((noinline)) float fpu_work(uint32_t k, unsigned int n)
{
    float x = 1.0f, add = (float)k * 0.25f;
    unsigned int i;

    for (i = 0; i < n; i++)
        x = x * 0.9999999f + add;
    return x;
}

static void w_fpu(void)
{
    struct worker *w = self();

    w->started = 1;
    note_hart(w);
    w->fresult = fpu_work(w->arg, FPU_ITER);
    note_hart(w);
    finish(w);
}

union fbits
{
    float f;
    uint32_t u;
};

static void t_fpu(void)
{
    static const UBYTE place[4] = { T_HART1, T_HART1, T_HART0, T_HART0 };
    static const char *const names[4] = {
        "giant fpu h1a", "giant fpu h1b", "giant fpu h0a", "giant fpu h0b"
    };
    union fbits expect[4], got;
    struct Task *me = FindTask(NULL);
    BYTE oldpri;
    unsigned int i, bad = 0, ok = 1;

    for (i = 0; i < 4; i++)
        expect[i].f = fpu_work(i + 1, FPU_ITER);
    /*
     * Two workers per hart at equal priority, so each pair shares its hart
     * by quantum and every switch saves and restores live FPU state. The
     * boot task drops below them while it polls, so the hart 0 pair runs.
     */
    for (i = 0; i < 4; i++)
    {
        workers[i].arg = i + 1;
        if (!spawn(&workers[i], names[i], -1, place[i], w_fpu))
            ok = 0;
    }
    oldpri = SetTaskPri(me, -2);
    for (i = 0; ok && i < 4; i++)
        if (!wait_flag(&workers[i].done, 1))
            ok = 0;
    SetTaskPri(me, oldpri);
    for (i = 0; ok && i < 4; i++)
    {
        got.f = workers[i].fresult;
        if (got.u != expect[i].u ||
            workers[i].harts != (place[i] == T_HART1 ? 2u : 1u))
            bad++;
    }
    report("fpu", ok && bad == 0, "mismatches", bad, "completed", ok);
}

/* --------------------------------------------------------- RemTask */

static void w_victim(void)
{
    struct worker *w = self();

    w->started = 1;
    for (;;)
    {
        w->count++;
        note_hart(w);
    }
}

static void t_remtask(void)
{
    struct worker *w = &workers[0];
    uint32_t evict0 = p4_giant_cpus[1].n_evict, c1, c2;
    unsigned long until;

    if (!spawn(w, "giant victim h1", 0, T_HART1, w_victim) ||
        !wait_flag(&w->started, 1))
    {
        report("remtask-running", 0, "started", 0, NULL, 0);
        return;
    }
    RemTask(w->task);
    c1 = w->count;
    until = __esp32p4_ticks + P4_TICK_HZ / 5;
    while ((long)(__esp32p4_ticks - until) < 0)
        ;
    c2 = w->count;
    report("remtask-running", c1 == c2 && w->harts == 2 &&
           p4_giant_cpus[1].n_evict == evict0 + 1,
           "evictions", p4_giant_cpus[1].n_evict - evict0,
           "count_after", c2 - c1);
}

/* ------------------------------------------------------------- park */

/* The flash MMU rendezvous without the cache-off in between. */
static void t_park(void)
{
    unsigned int i, ok = 0;
    uint32_t worst = 0;

    for (i = 0; i < 200; i++)
    {
        uint32_t s = p4_giant_mask(), t0, dt;

        __asm__ volatile("csrr %0, mcycle" : "=r"(t0));
        if (p4_giant_park_others())
        {
            __asm__ volatile("csrr %0, mcycle" : "=r"(dt));
            dt -= t0;
            if (dt > worst)
                worst = dt;
            ok++;
            p4_giant_unpark_others();
        }
        p4_giant_unmask(s);
        if (ok != i + 1)
        {
            p4_giant_park_report();
            break;
        }
        spin_cycles(5000 + (i * 7919) % 20000);
    }
    report("park", ok == 200, "rendezvous", ok, "worst_cycles", worst);
}

/* ----------------------------------------------------------- driver */

static void stats(void)
{
    unsigned int h;

    for (h = 0; h < P4_GIANT_HARTS; h++)
    {
        struct p4_giant_cpu *c = &p4_giant_cpus[h];

        krnP4PutStr("[giant-test] hart");
        krnP4PutDec(h);
        krnP4PutStr(" ipi=");      krnP4PutDec(c->n_ipi);
        krnP4PutStr(" tick=");     krnP4PutDec(c->n_tick);
        krnP4PutStr(" dispatch="); krnP4PutDec(c->n_dispatch);
        krnP4PutStr(" idle=");     krnP4PutDec(c->n_idle);
        krnP4PutStr(" fspin=");    krnP4PutDec(c->n_fspin);
        krnP4PutStr(" dspin=");    krnP4PutDec(c->n_dspin);
        krnP4PutStr(" frefuse=");  krnP4PutDec(c->n_frefuse);
        krnP4PutStr(" park=");     krnP4PutDec(c->n_park);
        krnP4PutStr(" evict=");    krnP4PutDec(c->n_evict);
        krnP4PutStr("\n");
    }
}

void p4_giant_selftest(void)
{
    boot_task = FindTask(NULL);
    boot_sig = AllocSignal(-1);
    krnP4PutStr("[giant-test] start, boot task on hart ");
    krnP4PutDec(p4_giant_hart());
    krnP4PutStr("\n");
    if (boot_sig < 0 || !p4_giant_cpus[1].online)
    {
        report("setup", 0, "online", p4_giant_cpus[1].online, NULL, 0);
        return;
    }
    t_concurrency();
    t_exclusion("forbid", 0);
    t_exclusion("disable", 1);
    t_ping();
    t_semaphore();
    t_messages();
    t_memory();
    t_fpu();
    t_remtask();
    t_park();
    krnP4PutStr("[giant-test] SUMMARY pass=");
    krnP4PutDec(passed);
    krnP4PutStr(" fail=");
    krnP4PutDec(failed);
    krnP4PutStr("\n");
    stats();
    FreeSignal(boot_sig);
}
