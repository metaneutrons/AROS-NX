/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * ESP32-P4 transitional Giant Exec SMP. See p4_giant.h for the model.
 *
 * Everything here that hart 1 may execute while hart 0 has the external
 * memory cache suspended is P4_SRAMCODE: the park loop and the spin loops'
 * park check. Everything else runs with the cache on.
 */

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <exec/resident.h>
#include <aros/asmcall.h>
#include <proto/exec.h>

#include <string.h>

#include <kernel_syscall.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "secondary_hw.h"
#include "p4_giant.h"

/* Software IPI: writing 1 to FROM_CPU_n raises source 79 + n, which the
   interrupt matrix routes to CLIC line 22 of hart n. E2 qualified this. */
#define P4_GIANT_IPI_FROM(h)    (0x500e5010u + (h) * 4u)
#define P4_GIANT_IPI_SOURCE(h)  (79u + (h))
#define P4_GIANT_ROUTE(c, s)    (0x500d6000u + (c) * 0x800u + (s) * 4u)
#define P4_GIANT_ROUTES         128u

/* MHCR branch predictor bits, cleared while parked (E2 CMD_PARK). */
#define P4_GIANT_MHCR           0x7c1
#define P4_GIANT_PREDICTOR      0x1030u

/* One second at CPU360; the iteration caps cover a stopped mcycle. */
#define P4_GIANT_SECOND_CYCLES  360000000u

struct p4_giant_cpu p4_giant_cpus[P4_GIANT_HARTS];

struct p4_giant_lock
{
    volatile uint32_t word;
    volatile uint32_t owner;        /* hart + 1, written by the owner */
    uint32_t depth[P4_GIANT_HARTS]; /* recursive locks only */
    volatile uint32_t waiting[P4_GIANT_HARTS]; /* spinning for F or D */
} __attribute__((aligned(64)));

/* How long a hart lets a waiting peer go first: about 55 us at CPU360. */
#define GIANT_COURTESY_CYCLES 20000u

static struct p4_giant_lock giant_f, giant_d, giant_console, giant_cache;
static uint32_t giant_console_timeouts;

P4_SRAMDATA volatile uint32_t p4_giant_park_request;
P4_SRAMDATA volatile uint32_t p4_giant_parked;

/* Hart 1 boot data, read by p4_giant_entry.S before it has a C runtime */
P4_SRAMDATA uint32_t p4_giant_boot[4];
#define GIANT_ISR_BYTES 8192
#define GIANT_ISR_PAINT 0x5aa5c33cu
static uint8_t giant_isr1[GIANT_ISR_BYTES] __attribute__((aligned(16)));
static uint8_t giant_isr0[GIANT_ISR_BYTES] __attribute__((aligned(16)));

/* Fill an ISR stack with a pattern; the deepest overwritten word shows the
   high-water mark (the stacks sit next to each other in SRAM). */
static void giant_paint(uint8_t *stack)
{
    uint32_t *w = (uint32_t *)stack;
    unsigned int i;

    for (i = 0; i < GIANT_ISR_BYTES / 4; i++)
        w[i] = GIANT_ISR_PAINT;
}

uint32_t p4_giant_isr_used(unsigned int hart)
{
    const uint32_t *w = (const uint32_t *)(hart ? giant_isr1 : giant_isr0);
    unsigned int i;

    for (i = 0; i < GIANT_ISR_BYTES / 4 && w[i] == GIANT_ISR_PAINT; i++)
        ;
    return GIANT_ISR_BYTES - i * 4;
}
uint8_t p4_giant_boot_stack1[2048] __attribute__((aligned(16)));

extern void __p4_giant_entry(void);
extern void __trap_entry(void);
void p4_giant_secondary_main(void);

P4_GIANT_INLINE uint32_t cycles(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, mcycle" : "=r"(v));
    return v;
}

P4_GIANT_INLINE void fence(void)
{
    __asm__ volatile("fence rw, rw" : : : "memory");
}

/* ------------------------------------------------------------------ park */

/*
 * p4_giant_park_request is 0, or the number + 1 of the hart that has to
 * park. That hart waits here, interrupts off, fetching nothing outside
 * internal SRAM, while the other one rewrites the flash MMU with the cache
 * suspended.
 */
P4_SRAMCODE static void giant_park_here(void)
{
    uint32_t predictor;

    __asm__ volatile("csrr %0, 0x7c1\n csrc 0x7c1, %1"
                     : "=&r"(predictor) : "r"(P4_GIANT_PREDICTOR) : "memory");
    __asm__ volatile("fence rw, rw" : : : "memory");
    p4_giant_parked = 1;
    __asm__ volatile("fence rw, rw" : : : "memory");
    while (p4_giant_park_request)
        ;
    __asm__ volatile("fence rw, rw" : : : "memory");
    p4_giant_parked = 0;
    __asm__ volatile("fence rw, rw" : : : "memory");
    __asm__ volatile("csrs 0x7c1, %0\n fence.i"
                     : : "r"(predictor & P4_GIANT_PREDICTOR) : "memory");
}

P4_SRAMCODE static void giant_service_park(void)
{
    if (p4_giant_park_request && p4_giant_park_request == p4_giant_hart() + 1)
    {
        uint32_t s = p4_giant_mask();
        giant_park_here();
        p4_giant_unmask(s);
    }
}

/*
 * Called with MIE clear, immediately before the cache goes off. Returns 0
 * if the other hart did not park within a second; the caller must then
 * leave the cache alone. Either hart may be the caller.
 */
P4_SRAMCODE int p4_giant_park_others(void)
{
    uint32_t start, left, other = p4_giant_hart() ^ 1;

    if (!p4_giant_cpus[1].online)
        return 1;
    /* If the other hart is asking at the same moment, park for it first;
       exactly one request is outstanding at any time. */
    for (;;)
    {
        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(&p4_giant_park_request, &expected,
                                        other + 1, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            break;
        giant_service_park();
    }
    *(volatile uint32_t *)P4_GIANT_IPI_FROM(other) = 1;
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    start = cycles();
    left = 100000000u;
    while (!p4_giant_parked)
    {
        if (!--left || (uint32_t)(cycles() - start) > P4_GIANT_SECOND_CYCLES)
        {
            p4_giant_park_request = 0;
            __asm__ volatile("fence rw, rw" : : : "memory");
            return 0;
        }
    }
    p4_giant_cpus[other].n_park++;
    return 1;
}

P4_SRAMCODE void p4_giant_unpark_others(void)
{
    if (!p4_giant_park_request)
        return;
    __asm__ volatile("fence rw, rw" : : : "memory");
    p4_giant_park_request = 0;
    __asm__ volatile("fence rw, rw" : : : "memory");
    while (p4_giant_parked)
        ;
}

/* ----------------------------------------------------------------- locks */

static inline int lock_try(struct p4_giant_lock *l, uint32_t me)
{
    if (__atomic_exchange_n(&l->word, 1u, __ATOMIC_ACQUIRE) != 0)
        return 0;
    l->owner = me + 1;
    return 1;
}

static inline int lock_own(struct p4_giant_lock *l, uint32_t me)
{
    return l->owner == me + 1;
}

static inline void lock_release(struct p4_giant_lock *l)
{
    l->owner = 0;
    __atomic_store_n(&l->word, 0u, __ATOMIC_RELEASE);
}

/*
 * Plain test-and-set is unfair: a hart that releases F and takes it again a
 * few instructions later, as every Exec call does, can starve the other
 * hart indefinitely (observed: hart 1 stuck for 30 minutes under load). So
 * a waiter announces itself, and a hart about to take the lock first lets
 * an announced peer go, for a bounded time so a stale flag costs only that.
 */
static void giant_acquire(struct p4_giant_lock *l, uint32_t me,
                          uint32_t *spins)
{
    volatile uint32_t *peer = &l->waiting[me ^ 1];

    if (*peer)
    {
        uint32_t start = cycles();

        while (*peer && (uint32_t)(cycles() - start) < GIANT_COURTESY_CYCLES)
            giant_service_park();
    }
    if (lock_try(l, me))
        return;
    l->waiting[me] = 1;
    __asm__ volatile("fence rw, rw" : : : "memory");
    while (!lock_try(l, me))
    {
        (*spins)++;
        while (l->word)
            giant_service_park();
    }
    l->waiting[me] = 0;
}

/* Interrupts as the caller has them; normally on, so this hart keeps
   taking IPIs and its own device interrupts while it waits. */
static void giant_spin_f(struct p4_giant_cpu *cpu, uint32_t me)
{
    giant_acquire(&giant_f, me, &cpu->n_fspin);
}

/* Interrupts off. The holder of D is a trap handler or a Disable()
   section; both are short and neither waits for this hart. */
static void giant_spin_d(struct p4_giant_cpu *cpu, uint32_t me)
{
    giant_acquire(&giant_d, me, &cpu->n_dspin);
}

int p4_giant_own_f(void)
{
    return lock_own(&giant_f, p4_giant_hart());
}

/* The dispatcher's try-lock yields to a peer that is already waiting. */
int p4_giant_try_f(void)
{
    uint32_t me = p4_giant_hart();

    if (lock_own(&giant_f, me))
        return 1;
    if (giant_f.waiting[me ^ 1])
        return 0;
    return lock_try(&giant_f, me);
}

void p4_giant_release_f(void)
{
    uint32_t me = p4_giant_hart();
    uint32_t other = me ^ 1;

    if (!lock_own(&giant_f, me))
        return;
    lock_release(&giant_f);
    /* Store-to-load order: the peer sets want_f, fences, then tries. */
    __asm__ volatile("fence rw, rw" : : : "memory");
    if (p4_giant_cpus[other].want_f && p4_giant_cpus[1].online)
    {
        p4_giant_cpus[other].want_f = 0;
        p4_giant_ipi(other, P4_GIANT_WORK_RESCHED);
    }
}

void p4_giant_acquire_d(void)
{
    uint32_t me = p4_giant_hart();

    if (!lock_own(&giant_d, me))
        giant_spin_d(&p4_giant_cpus[me], me);
}

void p4_giant_release_d(void)
{
    if (lock_own(&giant_d, p4_giant_hart()))
        lock_release(&giant_d);
}

/* ------------------------------------------------------------ Exec side */

int p4_giant_is_super(void)
{
    return P4_GIANT_GET(trap_depth) > 0;
}

void p4_giant_set_current(struct ExecBase *sysbase, struct Task *task)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();

    if (task == NULL && me == 0)
    {
        krnP4PutStr("[giant] hart 0 ThisTask set to NULL by ");
        krnP4PutHex32((uint32_t)(uintptr_t)__builtin_return_address(0));
        krnP4PutStr("\n");
    }
    p4_giant_cpus[me].current = task;
    /* SysBase->ThisTask stays hart 0's task for code that reads it. */
    if (me == 0 && sysbase)
        sysbase->ThisTask = task;
    p4_giant_unmask(s);
}

/*
 * TDNestCnt is raised before F is taken: from then on the task cannot be
 * preempted, so it cannot migrate while it spins, and a trap taken during
 * the spin does not switch it away holding a half-acquired lock.
 */
void p4_giant_forbid(void)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];
    int take;

    cpu->td_nest++;
    take = !cpu->trap_depth && cpu->td_nest == 0 && cpu->id_nest < 0;
    p4_giant_unmask(s);
    if (take && !lock_own(&giant_f, me))
        giant_spin_f(cpu, me);
}

void p4_giant_permit(void)
{
    uint32_t s = p4_giant_mask();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[p4_giant_hart()];

    /* Release while TDNestCnt still pins the task to this hart. */
    if (!cpu->trap_depth && cpu->td_nest == 0 && cpu->id_nest < 0)
        p4_giant_release_f();
    cpu->td_nest--;
    p4_giant_unmask(s);
}

void p4_giant_disable(void)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];
    int take_f;

    if (cpu->trap_depth || cpu->id_nest >= 0)
    {
        /* Nested, or interrupt code: D is already held. MIE stays off. */
        cpu->id_nest++;
        return;
    }
    /* Pin the task first, as in Forbid(), then take F with interrupts as
       they were; only then mask and take D. F before D, always. */
    cpu->td_nest++;
    take_f = cpu->td_nest == 0;
    p4_giant_unmask(s);
    if (take_f && !lock_own(&giant_f, me))
        giant_spin_f(cpu, me);
    (void)p4_giant_mask();
    giant_spin_d(cpu, me);
    cpu->id_nest++;
    cpu->td_nest--;
}

int p4_giant_enable(void)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];

    (void)s;
    cpu->id_nest--;
    if (cpu->trap_depth || cpu->id_nest >= 0)
        return 0;
    if (lock_own(&giant_d, me))
        lock_release(&giant_d);
    if (cpu->td_nest < 0)
        p4_giant_release_f();
    __asm__ volatile("csrsi mstatus, 8" : : : "memory");
    return 1;
}

/* -------------------------------------------------------- traps and IPIs */

void p4_giant_trap_enter(struct p4_giant_trap *t)
{
    uint32_t me = p4_giant_hart();

    if (p4_giant_park_request == me + 1)
        giant_park_here();
    t->had_d = lock_own(&giant_d, me);
    if (!t->had_d)
        giant_spin_d(&p4_giant_cpus[me], me);
}

/*
 * Leave the trap holding exactly what the task we return to needs. After a
 * dispatch that is the new task's need: D if it resumes inside Disable(),
 * F as well if it resumes inside Disable() or Forbid(). The dispatcher has
 * already obtained F for it, by try-lock, or refused the task.
 */
void p4_giant_trap_leave(struct p4_giant_trap *t)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];

    if (cpu->trap_depth == 1 && cpu->dispatched)
    {
        int need_d = cpu->id_nest >= 0;
        int need_f = need_d || cpu->td_nest >= 0;

        cpu->dispatched = 0;
        if (need_f && !lock_own(&giant_f, me))
            p4_giant_report("dispatched task lacks F");
        if (!need_f)
            p4_giant_release_f();
        if (!need_d && lock_own(&giant_d, me))
            lock_release(&giant_d);
    }
    else if (!t->had_d && lock_own(&giant_d, me))
        lock_release(&giant_d);
}

void p4_giant_ipi(uint32_t hart, uint32_t work)
{
    if (hart >= P4_GIANT_HARTS || !p4_giant_cpus[1].online)
        return;
    if (work)
        (void)__atomic_fetch_or(&p4_giant_cpus[hart].work, work,
                                __ATOMIC_SEQ_CST);
    fence();
    p4_w32(P4_GIANT_IPI_FROM(hart), 1);
}

/* Trap context, MIE clear. Deassert first, then read the posted work: a
   sender posts work before it raises the line, so nothing is lost. */
uint32_t p4_giant_ipi_take(void)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];
    uint32_t work;

    p4_w32(P4_GIANT_IPI_FROM(me), 0);
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    krnCLICClear(P4_GIANT_IPI_LINE);
    /* The deassert may have swallowed the raise of a park request, which
       carries no work bit: look at the request itself. */
    if (p4_giant_park_request == me + 1)
        giant_park_here();
    work = __atomic_exchange_n(&cpu->work, 0u, __ATOMIC_SEQ_CST);
    cpu->n_ipi++;
    if (work & P4_GIANT_WORK_TICK)
    {
        cpu->n_tick++;
        if (!cpu->idle && cpu->elapsed && --cpu->elapsed == 0)
            (void)__atomic_fetch_or(&cpu->sched,
                                    P4_GIANT_QUANTUM | P4_GIANT_SWITCH,
                                    __ATOMIC_SEQ_CST);
    }
    if (work & P4_GIANT_WORK_RESCHED)
        (void)__atomic_fetch_or(&cpu->sched, P4_GIANT_SWITCH,
                                __ATOMIC_SEQ_CST);
    return work;
}

/* The dispatcher found nothing to run (D held, MIE clear). */
void p4_giant_idle_enter(void)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];

    cpu->idle = 1;
    cpu->n_idle++;
    /* Hart 0 keeps its last task as ThisTask, as single-hart Exec always
       has; nothing on hart 1 looks at it while idle. */
    if (me == 1)
        cpu->current = NULL;
    p4_giant_release_f();
    if (lock_own(&giant_d, me))
        lock_release(&giant_d);
    /* A request posted from now on raises the line and ends the wfi. */
    giant_service_park();
}

void p4_giant_idle_leave(void)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];

    giant_spin_d(cpu, me);
    cpu->idle = 0;
}

#if defined(P4_GIANT_TEST)
/* Distinct tasks hart 1 has run, for the diagnostics (D held). */
#define GIANT_SEEN 48
static struct Task *giant_seen[GIANT_SEEN];
static uint32_t giant_seen_count[GIANT_SEEN];
static unsigned int giant_seen_n;

static void giant_note_hart1(struct Task *t)
{
    unsigned int i;

    for (i = 0; i < giant_seen_n; i++)
        if (giant_seen[i] == t)
        {
            giant_seen_count[i]++;
            return;
        }
    if (giant_seen_n < GIANT_SEEN)
    {
        giant_seen[giant_seen_n] = t;
        giant_seen_count[giant_seen_n++] = 1;
    }
}

/* Names may belong to tasks that have ended; print only live-looking ones. */
static void giant_seen_report(void)
{
    unsigned int i;

    krnP4PutStr("[giant] hart1 ran ");
    krnP4PutDec(giant_seen_n);
    krnP4PutStr(" tasks:");
    for (i = 0; i < giant_seen_n; i++)
    {
        struct Task *t = giant_seen[i];

        krnP4PutStr(" ");
        if (t->tc_State == TS_RUN || t->tc_State == TS_READY ||
            t->tc_State == TS_WAIT)
            krnP4PutStr(t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "?");
        else
            krnP4PutStr("(gone)");
        krnP4PutStr("x");
        krnP4PutDec(giant_seen_count[i]);
    }
    krnP4PutStr("\n");
}
#endif

void p4_giant_dispatched(void)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];

    cpu->idle = 0;
    cpu->dispatched = 1;
    cpu->n_dispatch++;
    /* An evicted task may have been waiting for F; nobody waits now. */
    giant_f.waiting[me] = 0;
#if defined(P4_GIANT_TEST)
    if (me == 1)
        giant_note_hart1(cpu->current);
#endif
}

#if defined(P4_GIANT_TEST)
#if defined(P4_GIANT_STRESS)
void p4_giant_stress_report(void);
#endif
/* Every ten seconds, from hart 0's tick: what each hart has been doing. */
static void giant_stats_line(void)
{
    static uint32_t ticks;
    unsigned int h;
    int mode;

    if (++ticks % (10 * P4_TICK_HZ))
        return;
    /* Diagnostics only: lossless while the host logger reads. */
    mode = krnP4ConsoleSetRuntime(0);
    krnP4PutStr("\n[giant] t=");
    krnP4PutDec(ticks / P4_TICK_HZ);
    for (h = 0; h < P4_GIANT_HARTS; h++)
    {
        struct p4_giant_cpu *c = &p4_giant_cpus[h];
        struct Task *t = c->idle ? NULL : c->current;

        krnP4PutStr(h ? " | h1 " : " | h0 ");
        krnP4PutStr("disp=");
        krnP4PutDec(c->n_dispatch);
        krnP4PutStr(" idle=");
        krnP4PutDec(c->n_idle);
        krnP4PutStr(" park=");
        krnP4PutDec(c->n_park);
        krnP4PutStr(" fref=");
        krnP4PutDec(c->n_frefuse);
        krnP4PutStr(" fspin=");
        krnP4PutDec(c->n_fspin);
        krnP4PutStr(" now=");
        krnP4PutStr(t && t->tc_Node.ln_Name ? t->tc_Node.ln_Name : "-");
    }
    krnP4PutStr(" | contimeout=");
    krnP4PutDec(giant_console_timeouts);
    krnP4PutStr(" isr0=");
    krnP4PutDec(p4_giant_isr_used(0));
    krnP4PutStr(" isr1=");
    krnP4PutDec(p4_giant_isr_used(1));
    krnP4PutStr("\n");
    if (ticks % (60 * P4_TICK_HZ) == 0)
        giant_seen_report();
#if defined(P4_GIANT_STRESS)
    p4_giant_stress_report();
#endif
    (void)krnP4ConsoleSetRuntime(mode);
}
#endif

/* Hart 0 timer interrupt: hart 1 has no tick of its own. */
void p4_giant_tick(void)
{
    if (p4_giant_cpus[1].online)
        p4_giant_ipi(1, P4_GIANT_WORK_TICK);
#if defined(P4_GIANT_TEST)
    giant_stats_line();
#endif
}

int p4_giant_task_allowed(struct Task *task, uint32_t hart)
{
    if (hart == 0)
        return !(task->tc_Flags & P4_TF_HART0_NO);
    return (task->tc_Flags & P4_TF_HART1_OK) && p4_giant_cpus[1].online;
}

/*
 * A task has just entered TaskReady (D held). Wake the peer if it may run
 * the task and is idle or running something of lower priority. The local
 * hart's own decision is Exec's usual Reschedule() logic.
 */
void p4_giant_task_ready(struct Task *task)
{
    uint32_t other = p4_giant_hart() ^ 1;
    struct p4_giant_cpu *oc = &p4_giant_cpus[other];

    if (!p4_giant_cpus[1].online || !p4_giant_task_allowed(task, other))
        return;
    if (oc->idle || !oc->current ||
        task->tc_Node.ln_Pri > oc->current->tc_Node.ln_Pri)
        p4_giant_ipi(other, P4_GIANT_WORK_RESCHED);
}

/*
 * RemTask() of a task that is running on the other hart. Called with D
 * held, so the other hart cannot switch tasks meanwhile; Exec's
 * core_Schedule() never switches a TS_REMOVED task, so from here on only
 * the eviction in krnTrapHandler takes it off that hart, without saving it.
 */
int p4_giant_evict_request(struct Task *task)
{
    uint32_t other = p4_giant_hart() ^ 1;
    struct p4_giant_cpu *oc = &p4_giant_cpus[other];

    if (!p4_giant_cpus[1].online || oc->idle || oc->current != task ||
        task->tc_State != TS_RUN)
        return 0;
    task->tc_State = TS_REMOVED;
    oc->evict = task;
    p4_giant_ipi(other, P4_GIANT_WORK_EVICT | P4_GIANT_WORK_RESCHED);
    return 1;
}

/* Forbid() held, D not: the other hart needs only D to evict. */
void p4_giant_evict_wait(struct Task *task)
{
    struct p4_giant_cpu *oc = &p4_giant_cpus[p4_giant_hart() ^ 1];
    uint32_t start = cycles();

    /* current is the peer's own field, not volatile: load it afresh on
       every pass, or an optimized loop never sees the switch. */
    while (__atomic_load_n(&oc->current, __ATOMIC_RELAXED) == task &&
           !oc->idle)
    {
        giant_service_park();
        if ((uint32_t)(cycles() - start) > P4_GIANT_SECOND_CYCLES)
        {
            p4_giant_report("RemTask eviction still pending");
            start = cycles();
        }
    }
}

/* FindTask(name) under Disable(): the peer's running task is in no list. */
struct Task *p4_giant_running_elsewhere(const char *name)
{
    struct p4_giant_cpu *oc = &p4_giant_cpus[p4_giant_hart() ^ 1];
    struct Task *t;

    if (!p4_giant_cpus[1].online || oc->idle)
        return NULL;
    t = oc->current;
    if (t && t->tc_Node.ln_Name && !strcmp(t->tc_Node.ln_Name, name))
        return t;
    return NULL;
}

/* ------------------------------------------------- console and caches */

/* Bounded and recursive per hart. Runtime console output is lossy anyway,
   so after the bound the character goes out unlocked rather than hang. */
void p4_giant_console_lock(void)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();
    unsigned int i;

    if (lock_own(&giant_console, me))
        giant_console.depth[me]++;
    else
    {
        for (i = 0; i < 200000; i++)
            if (lock_try(&giant_console, me))
            {
                giant_console.depth[me] = 1;
                break;
            }
        if (i == 200000)
            giant_console_timeouts++;
    }
    p4_giant_unmask(s);
}

void p4_giant_console_unlock(void)
{
    uint32_t s = p4_giant_mask();
    uint32_t me = p4_giant_hart();

    if (lock_own(&giant_console, me) && --giant_console.depth[me] == 0)
        lock_release(&giant_console);
    p4_giant_unmask(s);
}

/* The ROM cache routines program one shared controller. Callers are
   P4_SRAMCODE, so these are too. Interrupts stay off while held. */
P4_SRAMCODE void p4_giant_cache_lock(uint32_t *state)
{
    uint32_t me = p4_giant_hart();

    *state = p4_giant_mask();
    if (giant_cache.owner == me + 1)
    {
        giant_cache.depth[me]++;
        return;
    }
    while (__atomic_exchange_n(&giant_cache.word, 1u, __ATOMIC_ACQUIRE))
        while (giant_cache.word)
            giant_service_park();
    giant_cache.owner = me + 1;
    giant_cache.depth[me] = 1;
}

P4_SRAMCODE void p4_giant_cache_unlock(uint32_t state)
{
    uint32_t me = p4_giant_hart();

    if (giant_cache.owner == me + 1 && --giant_cache.depth[me] == 0)
    {
        giant_cache.owner = 0;
        __atomic_store_n(&giant_cache.word, 0u, __ATOMIC_RELEASE);
    }
    p4_giant_unmask(state);
}

/* ------------------------------------------------------- diagnostics */

void p4_giant_park_report(void)
{
    struct p4_giant_cpu *c1 = &p4_giant_cpus[1];

    krnP4PutStr("[giant] park refused: hart1 idle=");
    krnP4PutDec(c1->idle);
    krnP4PutStr(" depth=");
    krnP4PutDec(c1->trap_depth);
    krnP4PutStr(" ipi=");
    krnP4PutDec(c1->n_ipi);
    krnP4PutStr(" idle_loops=");
    krnP4PutDec(c1->n_idle);
    krnP4PutStr(" work=");
    krnP4PutHex32(c1->work);
    krnP4PutStr(" from1=");
    krnP4PutHex32(p4_r32(P4_GIANT_IPI_FROM(1)));
    krnP4PutStr(" clic1_22=");
    krnP4PutHex32(p4_r32(P4_CLIC_CTRL(P4_GIANT_IPI_LINE) + P4_CLIC_DUALCORE_OFF));
    krnP4PutStr(" request=");
    krnP4PutDec(p4_giant_park_request);
    krnP4PutStr(" parked=");
    krnP4PutDec(p4_giant_parked);
    krnP4PutStr(" D=");
    krnP4PutHex32(giant_d.owner);
    krnP4PutStr(" F=");
    krnP4PutHex32(giant_f.owner);
    krnP4PutStr("\n");
}

void p4_giant_report(const char *why)
{
    uint32_t me = p4_giant_hart();
    struct p4_giant_cpu *cpu = &p4_giant_cpus[me];
    unsigned int h;

    krnP4PutStr("[giant] hart ");
    krnP4PutDec(me);
    krnP4PutStr(": ");
    krnP4PutStr(why);
    krnP4PutStr(" id=");
    krnP4PutHex32((uint32_t)cpu->id_nest);
    krnP4PutStr(" td=");
    krnP4PutHex32((uint32_t)cpu->td_nest);
    krnP4PutStr(" F=");
    krnP4PutHex32(giant_f.owner);
    krnP4PutStr(" D=");
    krnP4PutHex32(giant_d.owner);
    krnP4PutStr("\n");
    for (h = 0; h < P4_GIANT_HARTS; h++)
    {
        struct p4_giant_cpu *c = &p4_giant_cpus[h];

        krnP4PutStr("[giant]   slot ");
        krnP4PutDec(h);
        krnP4PutStr(" current=");
        krnP4PutHex32((uint32_t)(uintptr_t)c->current);
        krnP4PutStr(" depth=");
        krnP4PutDec(c->trap_depth);
        krnP4PutStr(" idle=");
        krnP4PutDec(c->idle);
        krnP4PutStr(" online=");
        krnP4PutDec(c->online);
        krnP4PutStr(" id=");
        krnP4PutHex32((uint32_t)c->id_nest);
        krnP4PutStr(" td=");
        krnP4PutHex32((uint32_t)c->td_nest);
        krnP4PutStr(" dispatch=");
        krnP4PutDec(c->n_dispatch);
        krnP4PutStr("\n");
    }
}

/* ------------------------------------------------------- bring-up */

/* Install hart 0's private ISR stack. Called once, before interrupts. */
void p4_giant_init_primary(void)
{
    uint32_t top = (uint32_t)(uintptr_t)(giant_isr0 + sizeof(giant_isr0));

    giant_paint(giant_isr0);
    giant_paint(giant_isr1);
    p4_giant_cpus[0].online = 1;
    __asm__ volatile("csrw mscratch, %0" : : "r"(top) : "memory");
}

static void route_ipis(void)
{
    uint32_t c, s, v;

    for (c = 0; c < P4_GIANT_HARTS; c++)
        for (s = 0; s < P4_GIANT_ROUTES; s++)
        {
            v = p4_r32(P4_GIANT_ROUTE(c, s));
            if (s == P4_GIANT_IPI_SOURCE(c))
                v = P4_GIANT_IPI_LINE;
            else if (c == 1 || (v & 63u) == P4_GIANT_IPI_LINE ||
                     s == P4_GIANT_IPI_SOURCE(c ^ 1))
                v = 0;      /* hart 1 takes nothing but its IPI */
            p4_w32(P4_GIANT_ROUTE(c, s), v);
        }
    p4_w32(P4_GIANT_IPI_FROM(0), 0);
    p4_w32(P4_GIANT_IPI_FROM(1), 0);
    fence();
}

static void secondary_quiesce(void)
{
    p4_w32(P4_SECONDARY_RESET_REG,
           p4_r32(P4_SECONDARY_RESET_REG) | P4_SECONDARY_RESET_BIT);
    p4_w32(P4_SECONDARY_CLOCK_REG,
           p4_r32(P4_SECONDARY_CLOCK_REG) & ~P4_SECONDARY_CLOCK_BIT);
    p4_w32(P4_SECONDARY_STALL_REG,
           (p4_r32(P4_SECONDARY_STALL_REG) & ~P4_SECONDARY_STALL_MASK) |
           P4_SECONDARY_STALL);
    p4_w32(P4_SECONDARY_BOOT_REG, 0);
    __asm__ volatile("fence iorw, iorw" : : : "memory");
}

/*
 * Start hart 1 into Exec. Called by the boot task on hart 0 with Exec and
 * the tick running. Forbid() is held across the start, so no hart 0 task
 * can open a cache-off window (flash MMU update) while hart 1 still runs
 * its one-shot SRAM entry. Fails closed: on any timeout hart 1 goes back
 * to reset with its clock gated and Exec stays single-hart.
 */
int p4_giant_start_secondary(void)
{
    struct p4_giant_cpu *c1 = &p4_giant_cpus[1];
    uint32_t start, mtvec;
    unsigned int left;
    int ok;

    Forbid();
    secondary_quiesce();
    if (!(p4_r32(P4_SECONDARY_RESET_REG) & P4_SECONDARY_RESET_BIT) ||
        (p4_r32(P4_SECONDARY_CLOCK_REG) & P4_SECONDARY_CLOCK_BIT))
    {
        Permit();
        krnP4PutStr("[giant] hart 1 not isolated; staying single-hart\n");
        return 0;
    }

    memset(c1, 0, sizeof(*c1));
    c1->id_nest = -1;
    c1->td_nest = -1;

    route_ipis();
    krnCLICEnable(P4_GIANT_IPI_LINE, 0);

    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    p4_giant_boot[0] = mtvec;
    p4_giant_boot[1] = (uint32_t)(uintptr_t)(giant_isr1 + sizeof(giant_isr1));
    p4_giant_boot[2] = (uint32_t)(uintptr_t)p4_giant_secondary_main;
    p4_giant_boot[3] = 0;
    krnP4CacheWritebackData(p4_giant_boot, sizeof(p4_giant_boot));
    krnP4SyncCode((void *)__p4_giant_entry, 64);

    p4_w32(P4_SECONDARY_STALL_REG,
           (p4_r32(P4_SECONDARY_STALL_REG) & ~P4_SECONDARY_STALL_MASK) |
           P4_SECONDARY_UNSTALL);
    p4_w32(P4_SECONDARY_CLOCK_REG,
           p4_r32(P4_SECONDARY_CLOCK_REG) | P4_SECONDARY_CLOCK_BIT);
    p4_w32(P4_SECONDARY_RESET_REG,
           p4_r32(P4_SECONDARY_RESET_REG) & ~P4_SECONDARY_RESET_BIT);
    p4_w32(P4_SECONDARY_BOOT_REG, (uint32_t)(uintptr_t)__p4_giant_entry);
    __asm__ volatile("fence iorw, iorw" : : : "memory");

    start = cycles();
    left = 100000000u;
    while (!c1->online && --left &&
           (uint32_t)(cycles() - start) < P4_GIANT_SECOND_CYCLES / 10)
        ;
    ok = c1->online != 0;
    if (!ok)
    {
        secondary_quiesce();
        krnP4PutStr("[giant] hart 1 did not come online; boot word ");
        krnP4PutHex32(p4_giant_boot[3]);
        krnP4PutStr(", reset held, single-hart\n");
    }
    Permit();
    if (ok)
    {
        krnP4PutStr("[giant] hart 1 online, entry ");
        krnP4PutHex32((uint32_t)(uintptr_t)__p4_giant_entry);
        krnP4PutStr(" isr ");
        krnP4PutHex32(p4_giant_boot[1]);
        krnP4PutStr("\n");
    }
    return ok;
}

/*
 * Hart 1, first C code and first code fetched from flash. Its CLIC is
 * hart-local: only the IPI line is enabled. It then enters the ordinary
 * dispatcher through the KrnDispatch() system call, which never returns
 * to this boot stack.
 */
void p4_giant_secondary_main(void)
{
    struct p4_giant_cpu *c1 = &p4_giant_cpus[1];

    p4_giant_boot[3] = 2;
    krnCLICInit();
    krnCLICEnable(P4_GIANT_IPI_LINE, 0);
    c1->current = NULL;
    c1->idle = 1;
    fence();
    c1->online = 1;
    fence();
    __asm__ volatile(
        "li a7, %0\n"
        "ecall\n"
        : : "i"(SC_DISPATCH) : "a7", "memory");
    for (;;)
        __asm__ volatile("wfi");
}

/*
 * Start hart 1 from a COLDSTART resident at priority 104: after exec.library
 * (120), which creates the boot task and turns on multitasking, and before
 * everything else that the cold start brings up. The console is kept
 * blocking until the start, and the self-test if built, have reported.
 */
#if defined(P4_GIANT_TEST)
void p4_giant_selftest(void);
#endif

AROS_UFH3(static APTR, p4_giant_init,
          AROS_UFPA(void *, dummy, D0),
          AROS_UFPA(BPTR, segList, A0),
          AROS_UFPA(struct ExecBase *, SysBase, A6))
{
    AROS_USERFUNC_INIT

    krnP4ConsoleBlocking();
    if (p4_giant_start_secondary())
    {
#if defined(P4_GIANT_TEST)
        p4_giant_selftest();
#endif
    }
    krnP4ConsoleRuntime();
    return NULL;

    AROS_USERFUNC_EXIT
}

const struct Resident p4_giant_resident =
{
    RTC_MATCHWORD,
    (struct Resident *)&p4_giant_resident,
    (APTR)((const char *)&p4_giant_resident + sizeof(struct Resident)),
    RTF_COLDSTART,
    1,
    NT_UNKNOWN,
    104,
    "esp32p4 giant smp",
    "esp32p4 giant smp 1.0",
    &p4_giant_init
};
