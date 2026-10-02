/*
 * Copyright (c) 2026, The AROS Development Team. All rights reserved.
 *
 * ESP32-P4 transitional "Giant" Exec SMP (P4_GIANT=1).
 *
 * Both HP harts run ordinary Exec tasks. Forbid() and Disable() keep their
 * Amiga meaning system wide instead of per CPU: two global locks in
 * internal SRAM make them exclusive across harts.
 *
 *   F  held by a hart while its current task has Forbid() or Disable()
 *      active (TDNestCnt >= 0 or IDNestCnt >= 0).
 *   D  held by a hart while its current task has Disable() active, and by
 *      every trap handler while it runs.
 *
 * Lock order is F before D. Disable() therefore takes F first, and a trap,
 * which already holds D, may only try-lock F (the dispatcher does, for a
 * task that resumes inside Disable()). Interrupt code takes only D, so
 * interrupts keep running while some task holds Forbid(), as on the Amiga.
 *
 * This is a transitional workaround, not the fine-grained SMP design in
 * SMP.md: all Exec list and task-state work is serialized by F/D. It needs
 * no Exec ABI change, so the normal module set runs unchanged.
 *
 * Tasks run on hart 0 only unless they opt in through the private tc_Flags
 * bits below (bits 1 and 2 are unused by exec/tasks.h).
 */
#ifndef P4_GIANT_H
#define P4_GIANT_H

#include <stdint.h>

#define P4_GIANT_HARTS          2

#define P4_TF_HART1_OK          (1 << 1)    /* may run on hart 1 */
#define P4_TF_HART0_NO          (1 << 2)    /* must not run on hart 0 */

#define P4_GIANT_QUANTUM        (1u << 0)
#define P4_GIANT_SWITCH         (1u << 1)
#define P4_GIANT_DISPATCH       (1u << 2)

/* CLIC line of the software IPI on both harts (E2 qualified routing). */
#define P4_GIANT_IPI_LINE       22u

#define P4_GIANT_WORK_TICK      (1u << 0)
#define P4_GIANT_WORK_RESCHED   (1u << 1)
#define P4_GIANT_WORK_EVICT     (1u << 2)

struct Task;
struct ExecBase;

struct p4_giant_cpu
{
    struct Task *current;           /* GET_THIS_TASK */
    int32_t id_nest;                /* IDNESTCOUNT, -1 = interrupts on */
    int32_t td_nest;                /* TDNESTCOUNT, -1 = switching on */
    volatile uint32_t sched;        /* QUANTUM/SWITCH/DISPATCH, AMO only */
    uint32_t elapsed;               /* SCHEDELAPSED */
    uint32_t trap_depth;            /* KrnIsSuper() */
    volatile uint32_t work;         /* WORK_* bits posted by the peer */
    volatile uint32_t online;
    volatile uint32_t idle;         /* in the dispatcher's wfi loop */
    volatile uint32_t want_f;       /* dispatcher refused a task for F */
    uint32_t dispatched;            /* set by cpu_Dispatch in this trap */
    struct Task * volatile evict;   /* RemTask() of our current task */
    /* statistics, read by the diagnostics */
    uint32_t n_ipi;
    uint32_t n_tick;
    uint32_t n_dispatch;
    uint32_t n_idle;
    uint32_t n_fspin;
    uint32_t n_dspin;
    uint32_t n_frefuse;
    uint32_t n_park;
    uint32_t n_evict;
} __attribute__((aligned(64)));

extern struct p4_giant_cpu p4_giant_cpus[P4_GIANT_HARTS];

#define P4_GIANT_INLINE static inline __attribute__((always_inline))

P4_GIANT_INLINE uint32_t p4_giant_hart(void)
{
    uint32_t h;
    __asm__ volatile("csrr %0, mhartid" : "=r"(h));
    return h;
}

P4_GIANT_INLINE uint32_t p4_giant_mask(void)
{
    uint32_t s;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(s) : : "memory");
    return s;
}

P4_GIANT_INLINE void p4_giant_unmask(uint32_t s)
{
    if (s & 8)
        __asm__ volatile("csrsi mstatus, 8" : : : "memory");
}

/*
 * A task can migrate between reading mhartid and touching the slot, so
 * every access to the local slot from task context is made with MIE clear.
 */
#define P4_GIANT_GET(field) \
    ({ uint32_t __s = p4_giant_mask(); \
       __typeof__(p4_giant_cpus[0].field) __v = \
           p4_giant_cpus[p4_giant_hart()].field; \
       p4_giant_unmask(__s); __v; })

#define P4_GIANT_SET(field, val) \
    do { uint32_t __s = p4_giant_mask(); \
         p4_giant_cpus[p4_giant_hart()].field = (val); \
         p4_giant_unmask(__s); } while (0)

/* The peer may set bits remotely, so local updates are AMOs too. */
#define P4_GIANT_SCHED_SET(bit) \
    do { uint32_t __s = p4_giant_mask(); \
         (void)__atomic_fetch_or(&p4_giant_cpus[p4_giant_hart()].sched, \
                                 (bit), __ATOMIC_SEQ_CST); \
         p4_giant_unmask(__s); } while (0)
#define P4_GIANT_SCHED_CLEAR(bit) \
    do { uint32_t __s = p4_giant_mask(); \
         (void)__atomic_fetch_and(&p4_giant_cpus[p4_giant_hart()].sched, \
                                  ~(uint32_t)(bit), __ATOMIC_SEQ_CST); \
         p4_giant_unmask(__s); } while (0)
#define P4_GIANT_SCHED_ISSET(bit) (P4_GIANT_GET(sched) & (bit))

/* Exec-side primitives (kernel/p4_giant.c), callable before KernelBase. */
void p4_giant_forbid(void);
void p4_giant_permit(void);
void p4_giant_disable(void);
int  p4_giant_enable(void);         /* 1 when interrupts became enabled */
int  p4_giant_is_super(void);
void p4_giant_set_current(struct ExecBase *sysbase, struct Task *task);

/* Scheduler/trap glue */
struct p4_giant_trap
{
    uint32_t had_d;
};
void p4_giant_trap_enter(struct p4_giant_trap *t);
void p4_giant_trap_leave(struct p4_giant_trap *t);
int  p4_giant_task_allowed(struct Task *task, uint32_t hart);
int  p4_giant_own_f(void);
int  p4_giant_try_f(void);
void p4_giant_release_f(void);
void p4_giant_release_d(void);
void p4_giant_acquire_d(void);
void p4_giant_task_ready(struct Task *task);
void p4_giant_idle_enter(void);
void p4_giant_idle_leave(void);
void p4_giant_dispatched(void);
void p4_giant_ipi(uint32_t hart, uint32_t work);
uint32_t p4_giant_ipi_take(void);
void p4_giant_tick(void);

/* RemTask()/FindTask() support */
int  p4_giant_evict_request(struct Task *task);
void p4_giant_evict_wait(struct Task *task);
struct Task *p4_giant_running_elsewhere(const char *name);

/* Cache-off rendezvous for the flash MMU (SRAM resident). */
int  p4_giant_park_others(void);
void p4_giant_unpark_others(void);
void p4_giant_park_report(void);

/* Serializes the shared console and cache controller across harts. */
void p4_giant_console_lock(void);
void p4_giant_console_unlock(void);
void p4_giant_cache_lock(uint32_t *state);
void p4_giant_cache_unlock(uint32_t state);

int  p4_giant_start_secondary(void);
void p4_giant_report(const char *why);

#endif /* P4_GIANT_H */
