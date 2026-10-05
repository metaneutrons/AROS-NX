/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: per-hart data for the smp variant of the esp32p4-riscv target.
*/

#ifndef KERNEL_TLS_ESP32P4_H_
#define KERNEL_TLS_ESP32P4_H_

#include <exec/types.h>

/*
 * One record per HP core, in internal SRAM, found through mhartid.
 *
 * Not through tp, as the other SMP ports use their thread register: tp is
 * an ordinary x register here and part of every saved task context, so a
 * task that moved to the other hart would bring the old hart's pointer
 * with it. mhartid cannot go stale.
 *
 * Every access runs with interrupts masked on this hart. A task can only
 * change harts at a reschedule, so with interrupts masked it cannot read
 * one hart's id and then update the other hart's record. Each record is
 * written only by its own hart, so no atomic operations are needed.
 *
 * All code runs in machine mode on this machine, tasks included, so the
 * mask is available wherever these accessors are used.
 */

#define P4_TLS_HARTS    2

typedef struct tls
{
    struct Task *ThisTask;
    ULONG       ScheduleFlags;
    BYTE        IDNestCnt;
    BYTE        TDNestCnt;
    UWORD       Quantum;
    UWORD       Elapsed;
    ULONG       CPUNumber;
} __attribute__((aligned(64))) tls_t;   /* one cache line per hart */

#define TLSSF_Quantum   (1 << 0)
#define TLSSF_Switch    (1 << 1)
#define TLSSF_Dispatch  (1 << 2)

extern tls_t __p4_tls[P4_TLS_HARTS];

static inline unsigned long p4_tls_mask(void)
{
    unsigned long status;

    asm volatile("csrrci %0, mstatus, 8" : "=r"(status) :: "memory");
    return status;
}

static inline void p4_tls_unmask(unsigned long status)
{
    asm volatile("csrs mstatus, %0" :: "r"(status & 8) : "memory");
}

/* The caller has interrupts masked. */
static inline tls_t *p4_tls_self(void)
{
    unsigned long hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));
    return &__p4_tls[hart];
}

#define TLS_GET(name) \
    ({ \
        unsigned long __s = p4_tls_mask(); \
        __typeof__(__p4_tls[0].name) __v = p4_tls_self()->name; \
        p4_tls_unmask(__s); \
        __v; \
    })

#define TLS_SET(name, val) \
    do { \
        unsigned long __s = p4_tls_mask(); \
        p4_tls_self()->name = (val); \
        p4_tls_unmask(__s); \
    } while (0)

/* An update in place, e.g. TLS_UPDATE(IDNestCnt, ++) */
#define TLS_UPDATE(name, op) \
    do { \
        unsigned long __s = p4_tls_mask(); \
        p4_tls_self()->name op; \
        p4_tls_unmask(__s); \
    } while (0)

#endif /* KERNEL_TLS_ESP32P4_H_ */
