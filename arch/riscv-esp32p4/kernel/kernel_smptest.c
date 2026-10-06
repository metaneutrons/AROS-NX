/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: S4 acceptance test for the smp variant of the esp32p4-riscv
          target (P4_S4_TEST=1, diagnostic only).

    A COLDSTART resident after the second hart's start. It runs pairs of
    tasks, one bound to each hart, through exec's cross-hart paths -
    signals, a semaphore, a message port, the allocator - and two tasks
    free to run on either hart, then checks every count on hart 0. The
    tasks touch no device: drivers that guard against their own interrupt
    with Disable() are not safe on hart 1 yet (SMP.md, S5).
*/

#include <aros/kernel.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include <kernel_base.h>
#include <proto/kernel.h>

#include "kernel_intern.h"
#include "etask.h"
#include "tls.h"

#if defined(P4_S4_TEST)

#define S4_PINGPONG     2000
#define S4_SEM          2000
#define S4_MSGS         1000
#define S4_ALLOCS       2000
#define S4_SPIN         4000000

enum { W_PING, W_PONG, W_SEM1, W_SEM0, W_MSG1, W_MSG0, W_ALLOC1, W_ALLOC0,
       W_ANY1, W_ANY2, W_COUNT };

struct S4Message
{
    struct Message  m;
    ULONG           seq;
};

struct S4
{
    struct ExecBase         *sysbase;
    struct Task             *parent;
    ULONG                   donesig;
    struct Task             *task[W_COUNT];
    ULONG                   hart[W_COUNT];      /* where each one ended */
    ULONG                   seen[W_COUNT];      /* harts each one ran on */
    volatile ULONG          done[W_COUNT];
    volatile ULONG          finished;
    ULONG                   pingpong;
    struct SignalSemaphore  sem;
    ULONG                   semcount;
    struct MsgPort * volatile port;
    ULONG                   msgs;
    ULONG                   msgerr;
    ULONG                   allocerr;
    ULONG                   spin[2];
};

static inline ULONG hart(void)
{
    unsigned long h;

    asm volatile("csrr %0, mhartid" : "=r"(h));
    return h;
}

/* Every worker waits until all exist, so partners are known */
static void go(struct S4 *t)
{
    struct ExecBase *SysBase = t->sysbase;

    Wait(SIGBREAKF_CTRL_D);
}

static void finish(struct S4 *t, int w)
{
    struct ExecBase *SysBase = t->sysbase;

    t->hart[w] = hart();
    t->seen[w] |= 1UL << hart();
    t->done[w] = 1;
    __atomic_fetch_add(&t->finished, 1, __ATOMIC_SEQ_CST);
    Signal(t->parent, t->donesig);
}

static void w_ping(struct S4 *t)
{
    struct ExecBase *SysBase = t->sysbase;
    int i;

    go(t);

    for (i = 0; i < S4_PINGPONG; i++)
    {
        Wait(SIGBREAKF_CTRL_E);
        t->seen[W_PING] |= 1UL << hart();
        Signal(t->task[W_PONG], SIGBREAKF_CTRL_F);
    }
    finish(t, W_PING);
}

static void w_pong(struct S4 *t)
{
    struct ExecBase *SysBase = t->sysbase;
    int i;

    go(t);

    for (i = 0; i < S4_PINGPONG; i++)
    {
        Signal(t->task[W_PING], SIGBREAKF_CTRL_E);
        Wait(SIGBREAKF_CTRL_F);
        t->seen[W_PONG] |= 1UL << hart();
        t->pingpong++;
    }
    finish(t, W_PONG);
}

static void w_sem(struct S4 *t, int w)
{
    struct ExecBase *SysBase = t->sysbase;
    int i, j;

    go(t);

    for (i = 0; i < S4_SEM; i++)
    {
        ULONG v;

        ObtainSemaphore(&t->sem);
        v = t->semcount;
        for (j = 0; j < 50; j++)
            asm volatile("" ::: "memory");      /* widen the window */
        t->semcount = v + 1;
        ReleaseSemaphore(&t->sem);
        t->seen[w] |= 1UL << hart();
    }
    finish(t, w);
}

static void w_sem1(struct S4 *t) { w_sem(t, W_SEM1); }
static void w_sem0(struct S4 *t) { w_sem(t, W_SEM0); }

static void w_msg1(struct S4 *t)
{
    struct ExecBase *SysBase = t->sysbase;
    struct MsgPort *port = CreateMsgPort();
    ULONG expect = 0;

    go(t);

    t->port = port;
    Signal(t->task[W_MSG0], SIGBREAKF_CTRL_E);
    if (port)
    {
        while (t->msgs < S4_MSGS)
        {
            struct S4Message *m;

            WaitPort(port);
            while ((m = (struct S4Message *)GetMsg(port)))
            {
                if (m->seq != expect++)
                    t->msgerr++;
                t->msgs++;
                t->seen[W_MSG1] |= 1UL << hart();
                ReplyMsg(&m->m);
            }
        }
        t->port = NULL;
        DeleteMsgPort(port);
    }
    else
        t->msgerr++;
    finish(t, W_MSG1);
}

static void w_msg0(struct S4 *t)
{
    struct ExecBase *SysBase = t->sysbase;
    struct MsgPort *reply = CreateMsgPort();
    struct S4Message m;
    ULONG i;

    go(t);

    Wait(SIGBREAKF_CTRL_E);
    if (reply && t->port)
    {
        m.m.mn_Node.ln_Type = NT_MESSAGE;
        m.m.mn_ReplyPort = reply;
        m.m.mn_Length = sizeof(m);
        for (i = 0; i < S4_MSGS; i++)
        {
            m.seq = i;
            PutMsg(t->port, &m.m);
            WaitPort(reply);
            GetMsg(reply);
            t->seen[W_MSG0] |= 1UL << hart();
        }
    }
    else
        t->msgerr++;
    if (reply)
        DeleteMsgPort(reply);
    finish(t, W_MSG0);
}

static void w_alloc(struct S4 *t, int w)
{
    struct ExecBase *SysBase = t->sysbase;
    int i;

    go(t);

    for (i = 0; i < S4_ALLOCS; i++)
    {
        ULONG size = 16 + (i * 37) % 1000, k;
        UBYTE *p = AllocMem(size, MEMF_ANY);

        if (!p)
        {
            t->allocerr++;
            continue;
        }
        for (k = 0; k < size; k++)
            p[k] = (UBYTE)(i + w);
        for (k = 0; k < size; k++)
            if (p[k] != (UBYTE)(i + w))
                t->allocerr++;
        FreeMem(p, size);
        t->seen[w] |= 1UL << hart();
    }
    finish(t, w);
}

static void w_alloc1(struct S4 *t) { w_alloc(t, W_ALLOC1); }
static void w_alloc0(struct S4 *t) { w_alloc(t, W_ALLOC0); }

static void w_any(struct S4 *t, int w, int slot)
{
    ULONG i, x = 1;

    go(t);

    for (i = 0; i < S4_SPIN; i++)
    {
        x = x * 1103515245 + 12345;
        if (!(i & 1023))
            t->seen[w] |= 1UL << hart();
    }
    t->spin[slot] = x;
    finish(t, w);
}

static void w_any1(struct S4 *t) { w_any(t, W_ANY1, 0); }
static void w_any2(struct S4 *t) { w_any(t, W_ANY2, 1); }

static void *mask_for(ULONG which)
{
    void *aff;

    if (which > 1)
        return (void *)TASKAFFINITY_ANY;
    aff = KrnAllocCPUMask();
    if (aff)
        KrnGetCPUMask(which, aff);
    return aff;
}

static struct Task *spawn(struct S4 *t, const char *name, void (*fn)(struct S4 *),
                          ULONG which)
{
    struct ExecBase *SysBase = t->sysbase;

    return NewCreateTask(TASKTAG_NAME, (IPTR)name,
                         TASKTAG_AFFINITY, (IPTR)mask_for(which),
                         TASKTAG_PRI, 0,
                         TASKTAG_PC, (IPTR)fn,
                         TASKTAG_ARG1, (IPTR)t,
                         TAG_DONE);
}

static void put(const char *label, ULONG v)
{
    krnP4PutStr(label);
    krnP4PutDec(v);
}

AROS_UFH3(static APTR, s4test_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    static struct S4 t;
    BYTE sig;
    ULONG fails = 0, any = 0, w, start, cycles;

    if (KrnGetCPUCount() < 2)
    {
        krnP4PutStr("[smp-s4] FAIL one hart only\n");
        return NULL;
    }
    sig = AllocSignal(-1);
    if (sig < 0)
        return NULL;

    t.sysbase = SysBase;
    t.parent = FindTask(NULL);
    t.donesig = 1UL << sig;
    InitSemaphore(&t.sem);

    asm volatile("csrr %0, mcycle" : "=r"(start));

    /* Each partner exists before the other signals it */
    t.task[W_PING]   = spawn(&t, "s4 ping (hart 1)", w_ping, 1);
    t.task[W_PONG]   = spawn(&t, "s4 pong (hart 0)", w_pong, 0);
    t.task[W_SEM1]   = spawn(&t, "s4 sem (hart 1)", w_sem1, 1);
    t.task[W_SEM0]   = spawn(&t, "s4 sem (hart 0)", w_sem0, 0);
    t.task[W_MSG0]   = spawn(&t, "s4 msg (hart 0)", w_msg0, 0);
    t.task[W_MSG1]   = spawn(&t, "s4 msg (hart 1)", w_msg1, 1);
    t.task[W_ALLOC1] = spawn(&t, "s4 alloc (hart 1)", w_alloc1, 1);
    t.task[W_ALLOC0] = spawn(&t, "s4 alloc (hart 0)", w_alloc0, 0);
    t.task[W_ANY1]   = spawn(&t, "s4 any 1", w_any1, 2);
    t.task[W_ANY2]   = spawn(&t, "s4 any 2", w_any2, 2);

    for (w = 0; w < W_COUNT; w++)
        if (!t.task[w])
            fails |= 1UL << w;
    for (w = 0; !fails && w < W_COUNT; w++)
        Signal(t.task[w], SIGBREAKF_CTRL_D);

    /*
     * Wait by polling, below the workers' priority, with a ten-second
     * bound: a lost wakeup must show up as a report, not as a silent hang.
     */
    {
        BYTE oldpri = SetTaskPri(t.parent, -10);
        ULONG now;

        while (!fails && __atomic_load_n(&t.finished, __ATOMIC_ACQUIRE) < W_COUNT)
        {
            asm volatile("csrr %0, mcycle" : "=r"(now));
            if (now - start > 3600000000UL)
            {
                fails |= 1UL << 29;
                break;
            }
        }
        SetTaskPri(t.parent, oldpri);
    }

    asm volatile("csrr %0, mcycle" : "=r"(cycles));
    cycles -= start;

    /* Bound tasks ended where they belong; the free ones used both harts */
    for (w = 0; w < W_ANY1; w++)
        if (t.hart[w] != ((w == W_PING || w == W_SEM1 || w == W_MSG1 ||
                           w == W_ALLOC1) ? 1 : 0) ||
            t.seen[w] != (1UL << t.hart[w]))
            fails |= 1UL << (16 + w);
    any = t.seen[W_ANY1] | t.seen[W_ANY2];
    if (any != 3)
        fails |= 1UL << 30;
    if (t.pingpong != S4_PINGPONG || t.semcount != 2 * S4_SEM ||
        t.msgs != S4_MSGS || t.msgerr || t.allocerr ||
        t.spin[0] != t.spin[1])
        fails |= 1UL << 31;

    krnP4ConsoleBlocking();
    put("[smp-s4] detail pingpong=", t.pingpong);
    put(" sem=", t.semcount);
    put(" msgs=", t.msgs);
    put(" msgerr=", t.msgerr);
    put(" allocerr=", t.allocerr);
    put(" any-harts=", any);
    put(" any1=", t.seen[W_ANY1]);
    put(" any2=", t.seen[W_ANY2]);
    krnP4PutStr(" ended=");
    for (w = 0; w < W_COUNT; w++)
        krnP4PutDec(t.hart[w]);
    put(" cycles=", cycles);
    krnP4PutStr("\n");
    if (fails)
    {
        krnP4PutStr("[smp-s4] FAIL ");
        krnP4PutHex32(fails);
        krnP4PutStr("\n");
        if (fails & (1UL << 29))
        {
            ULONG h;

            /* Who did not finish, and what each hart runs. A task that
               finished has exited, so only the others can be looked at. */
            for (w = 0; w < W_COUNT; w++)
            {
                struct Task *k = t.task[w];

                if (t.done[w])
                    continue;
                krnP4PutStr("[smp-s4] task ");
                krnP4PutDec(w);
                krnP4PutStr(k ? " " : " missing\n");
                if (!k)
                    continue;
                krnP4PutStr(k->tc_Node.ln_Name);
                put(" state=", k->tc_State);
                krnP4PutStr(" wait=");
                krnP4PutHex32(k->tc_SigWait);
                krnP4PutStr(" recvd=");
                krnP4PutHex32(k->tc_SigRecvd);
                put(" cpu=", IntETask(k->tc_UnionETask.tc_ETask)->iet_CpuNumber);
                put(" seen=", t.seen[w]);
                krnP4PutStr("\n");
            }
            for (h = 0; h < P4_TLS_HARTS; h++)
            {
                struct Task *r = __p4_tls[h].ThisTask;

                put("[smp-s4] hart ", h);
                krnP4PutStr(" runs ");
                krnP4PutStr(r && r->tc_Node.ln_Name ? r->tc_Node.ln_Name : "-");
                put(" idnest=", (ULONG)(LONG)__p4_tls[h].IDNestCnt);
                put(" tdnest=", (ULONG)(LONG)__p4_tls[h].TDNestCnt);
                put(" flags=", __p4_tls[h].ScheduleFlags);
                krnP4PutStr("\n");
            }
            put("[smp-s4] finished=", t.finished);
            krnP4PutStr("\n");
        }
    }
    else
        krnP4PutStr("[smp-s4] PASS; signals/semaphore/msgport/alloc across harts, free tasks on both\n");
    krnP4ConsoleRuntime();

    FreeSignal(sig);
    return NULL;

    AROS_USERFUNC_EXIT
}

const struct Resident p4s4test_resident =
{
    RTC_MATCHWORD,
    (struct Resident *)&p4s4test_resident,
    (APTR)((const char *)&p4s4test_resident + sizeof(struct Resident)),
    RTF_COLDSTART,
    1,
    NT_UNKNOWN,
    103,
    "esp32p4 smp s4 test",
    "esp32p4 smp s4 test 1.0",
    &s4test_init
};

#endif /* P4_S4_TEST */
