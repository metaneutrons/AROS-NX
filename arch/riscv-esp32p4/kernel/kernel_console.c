/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Early and queued runtime debug console for the esp32p4-riscv target.

    There is no firmware console call to borrow here, so output goes
    straight at the hardware. Two channels are implemented and one is
    compiled in (see P4_CONSOLE_USB in hardware.h):

      - the USB serial/JTAG peripheral, which is what a board with only a
        USB-C socket brings out, and

      - UART0, for boards that route it to pins. It is left exactly as
        the first stage ROM loader configured it to print its own
        messages: reprogramming the divisor before the clock tree is
        understood would lose the one channel that can report what went
        wrong.

    Both waits are bounded. A console that stops draining - an unclocked
    UART, or a USB host that never attached - must not take the boot with
    it, and a dropped character is the lesser loss.
*/

#include <inttypes.h>

#include <aros/config.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "tls.h"
#if !defined(P4_CONSOLE_BACKEND_ONLY)
#include <proto/exec.h>
#include "console_ring.h"
#include "console_tap.h"
#endif

#define SPIN_LIMIT      100000

/* Record metadata is a diagnostic transport format, not ordinary console
 * output. Keep payload bytes unchanged by default; overflow reports below
 * remain unconditional in either mode. */
#ifndef P4_CONSOLE_RECORD_PREFIX
#define P4_CONSOLE_RECORD_PREFIX 0
#endif
#if P4_CONSOLE_RECORD_PREFIX != 0 && P4_CONSOLE_RECORD_PREFIX != 1
#error P4_CONSOLE_RECORD_PREFIX must be 0 or 1
#endif

/* Runtime mode starts before Exec can schedule callers. Debug must never wait
 * for a USB reader: callers include input publication and locked redraws.
 * Before the sender is ready, saturated output is deliberately lossy.
 * Early USB waits share one boot
 * budget as well: a bound per byte still stalls a large boot report for
 * minutes without a reader. Mode changes are atomic across both harts;
 * the separate hardware lock serializes ordinary direct and queued writes. */
static volatile unsigned int console_runtime;
static volatile unsigned long console_dropped;
/* The sender publishes readiness only after opening its timer. Before that,
 * and if task creation fails, retain the bounded legacy output path. */
#if !defined(P4_CONSOLE_BACKEND_ONLY) || P4_CONSOLE_USB
static unsigned int console_sender_ready;
#endif

void krnP4ConsoleRuntime(void)
{
#if !defined(P4_CONSOLE_WAIT)
    __atomic_store_n(&console_runtime, 1, __ATOMIC_RELEASE);
#endif
}

/* Direct, bounded output for a stuck hart and secondary startup. This path
 * does not require the sender task, but cannot guarantee lossless output. */
void krnP4ConsoleBlocking(void)
{
    __atomic_store_n(&console_runtime, 0, __ATOMIC_RELEASE);
}

/*
 * The backends below write one character of a finished line each;
 * krnP4PutC() at the end of this file assembles the lines of both harts.
 * The host test (tests/console_nonblocking_test.py) builds the backends
 * alone, as krnP4PutC() itself, with P4_CONSOLE_BACKEND_ONLY.
 */
#if !defined(P4_CONSOLE_BACKEND_ONLY)
#define CONSOLE_RAW_PUTC console_raw_putc
static void console_raw_putc(char c);
static void console_flush_self(void);
#else
#define CONSOLE_RAW_PUTC krnP4PutC
#endif

static inline uint32_t mmio_rd(uint32_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_wr(uint32_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

#if P4_CONSOLE_USB

/*
 * Bytes accumulate in the endpoint buffer and are handed over on a
 * newline or when it fills. Flushing every byte would work but would cap
 * the console at one USB transaction per character.
 */
static unsigned int usj_pending;

/* One aggregate failed-poll budget for this boot, shared by byte and flush
 * waits. Blocking diagnostic mode must not replenish it. After exhaustion
 * each call still probes once, so a reader arriving later recovers output.
 * Normal writers are serialized by console_emit's lock; atomic accounting
 * also covers its reentrant fault/unlocked fallback paths. */
#define ATTACH_SPINS    2000000
static unsigned int usj_wait_remaining = ATTACH_SPINS;

static int usj_wait_free(unsigned int cap)
{
    unsigned int limit = 1;
    unsigned int i;
    unsigned int remaining = __atomic_load_n(&usj_wait_remaining,
                                              __ATOMIC_RELAXED);

    if (!__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) && remaining)
        limit = remaining < cap ? remaining : cap;
    for (i = 0; i < limit; i++)
    {
        if (mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE)
            return 1;
        if (!__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE))
        {
            remaining = __atomic_load_n(&usj_wait_remaining, __ATOMIC_RELAXED);
            while (remaining &&
                   !__atomic_compare_exchange_n(&usj_wait_remaining, &remaining,
                                                remaining - 1, 0,
                                                __ATOMIC_RELAXED,
                                                __ATOMIC_RELAXED))
                ;
            /* Another writer may have exhausted the snapshot allowance.
             * Stop here rather than spinning to that obsolete local limit. */
            if (remaining <= 1)
                break;
        }
    }
    return 0;
}

static void usj_flush(void)
{
    if (!usj_pending)
        return;

    mmio_wr(P4_USJ_BASE, P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
    usj_pending = 0;

    if (__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE))
        return;

    /* Wait for the host to collect it, so the next byte has somewhere to
       go. If no host ever does, give up and keep going. */
    (void)usj_wait_free(SPIN_LIMIT);
}

#if !defined(P4_CONSOLE_BACKEND_ONLY)
/* The sender must stay single-probe even if another hart changes modes for
 * an emergency report while it owns the hardware lock. */
static void usj_flush_nowait(void)
{
    if (usj_pending)
    {
        mmio_wr(P4_USJ_BASE, P4_USJ_EP1_CONF, P4_USJ_WR_DONE);
        usj_pending = 0;
    }
}
#endif

/*
 * Waits for the host rather than dropping. During bring-up the important
 * output is the earliest, and the host is typically not attached yet when
 * it is produced - so dropping means the one sequence worth reading is
 * the one that cannot be read. A finite waiting allowance helps a listener
 * attaching during enumeration, but cannot guarantee lossless early output.
 *
 * The waiting allowance is aggregate, not renewed for every character.
 * A board with no reader consumes it once and then uses single probes.
 */

void CONSOLE_RAW_PUTC(char c)
{
    if (!usj_wait_free(ATTACH_SPINS))
    {
        console_dropped++;
        return;
    }

    mmio_wr(P4_USJ_BASE, P4_USJ_EP1, (uint32_t)(unsigned char)c);

    if (++usj_pending >= P4_USJ_EP1_DEPTH || c == '\n')
        usj_flush();
}

/*
 * One character from the host, or -1 if it has sent none.
 *
 * Never waits.  A caller polling this has to be able to do something else,
 * and the one caller that matters - econsole's Raw_Read() - reschedules
 * between attempts.
 *
 * A pending transmit is flushed first.  Without that, a prompt written
 * without a trailing newline sits in the endpoint buffer unsent while this
 * waits for a reply to it, which is a deadlock made entirely of politeness.
 */
int krnP4GetC(void)
{
#if !defined(P4_CONSOLE_BACKEND_ONLY)
    console_flush_self();
#endif
    if ((!__atomic_load_n(&console_sender_ready, __ATOMIC_ACQUIRE) ||
         !__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE)) && usj_pending)
        usj_flush();

    if (!(mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_OUT_EP_DATA_AVAIL))
        return -1;

    return (int)(mmio_rd(P4_USJ_BASE, P4_USJ_EP1) & 0xFF);
}

#else /* UART0 */

void CONSOLE_RAW_PUTC(char c)
{
    unsigned int spins = __atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE)
                       ? 1 : SPIN_LIMIT;

    while (spins--)
    {
        uint32_t used = (mmio_rd(P4_UART0_BASE, P4_UART_STATUS)
                            >> P4_UART_TXFIFO_CNT_S) & P4_UART_TXFIFO_CNT_M;

        if (used < P4_UART_FIFO_DEPTH)
        {
            mmio_wr(P4_UART0_BASE, P4_UART_FIFO, (uint32_t)(unsigned char)c);
            return;
        }
    }
    console_dropped++;
}

int krnP4GetC(void)
{
#if !defined(P4_CONSOLE_BACKEND_ONLY)
    console_flush_self();
#endif
    if (((mmio_rd(P4_UART0_BASE, P4_UART_STATUS) >> P4_UART_RXFIFO_CNT_S)
            & P4_UART_RXFIFO_CNT_M) == 0)
        return -1;

    return (int)(mmio_rd(P4_UART0_BASE, P4_UART_FIFO) & 0xFF);
}

#endif

#if !defined(P4_CONSOLE_BACKEND_ONLY)
/*
 * Both harts print, mostly a character at a time through RawPutChar().
 * Each hart assembles its line in a buffer of its own, and a finished
 * line goes out whole under a lock, so lines from the two harts do not
 * mix and the USB writer's state has one user at a time. Interrupts are
 * masked while a hart touches its buffer or holds the lock: an interrupt
 * that prints would otherwise find its own hart holding it.
 *
 * The lock records its holder, so a fault taken inside the output, whose
 * report comes back here, does not wait for itself. The wait is bounded
 * like every wait in this file: past it the line goes out regardless, as
 * a mixed line is the lesser loss against a hart stuck on the console.
 */
#define CONSOLE_LINE        128
#define CONSOLE_LOCK_SPINS  20000000

static char console_line[P4_TLS_HARTS][CONSOLE_LINE];
static unsigned int console_len[P4_TLS_HARTS];
static volatile uint32_t console_holder;    /* hart + 1, or 0 */
static struct p4_console_ring console_queue;
static struct p4_console_ring console_tap_queue;
/* State 0 is free, 1 is held, and 3 is held with a release waiting. All tap
 * operations make one acquisition attempt. The combined state lets release
 * revoke a session even when a bounded producer/pop currently owns the lock;
 * that owner clears the ring before handing the lock on.
 */
static uint32_t console_tap_control;
static void *console_tap_owner;
static uint32_t console_tap_session;
static uint32_t console_tap_active_session;
static uint32_t console_tap_epoch;
/* Lifetime loss total: unlike ring indices and contents, this is never reset
 * at claim/release boundaries. It includes ring-full and control-lock drops.
 */
static uint32_t console_tap_lifetime_drops;
static uint32_t console_tap_registered;
static char console_frame[224];
static unsigned int console_frame_len, console_frame_pos;
static uint32_t console_reported_drops;
static unsigned int console_last_overflow;
static inline unsigned int console_hart(void);

static int console_tap_trylock(void)
{
    uint32_t expected = 0;

    return __atomic_compare_exchange_n(&console_tap_control, &expected, 1u, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Called with console_tap_control held. Logically emptying the tap ring only
 * resets its indices and counters; unread slot bytes are unreachable and do
 * not need to be overwritten in a producer or claim path.
 */
static void console_tap_clear_locked(void)
{
    __atomic_store_n(&console_tap_queue.lock, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_queue.head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_queue.tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_queue.count, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_queue.next_sequence, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_queue.dropped, 0u, __ATOMIC_RELAXED);
}

static void console_tap_finish_release_locked(void *owner, uint32_t session)
{
    uint32_t active;

    if (!owner || !session ||
        __atomic_load_n(&console_tap_owner, __ATOMIC_RELAXED) != owner ||
        __atomic_load_n(&console_tap_session, __ATOMIC_RELAXED) != session)
        return;

    /* Release may already have revoked this exact session before it acquired
     * the control lock. Never clear a different active lease defensively.
     */
    active = __atomic_load_n(&console_tap_active_session, __ATOMIC_ACQUIRE);
    if (active == session)
    {
        uint32_t expected = session;

        if (!__atomic_compare_exchange_n(&console_tap_active_session,
                                        &expected, 0u, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return;
    }
    else if (active != 0u)
        return;

    console_tap_clear_locked();
    __atomic_store_n(&console_tap_owner, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_session, 0u, __ATOMIC_RELAXED);
}

static void console_tap_unlock(void)
{
    uint32_t expected = 1u;

    if (__atomic_compare_exchange_n(&console_tap_control, &expected, 0u, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return;

    /* release() changed 1 to 3 after revoking active_session. No new tap
     * operation can pass the lock until this matching-lease cleanup ends.
     */
    if (expected == 3u)
    {
        void *owner = __atomic_load_n(&console_tap_owner, __ATOMIC_RELAXED);
        uint32_t session = __atomic_load_n(&console_tap_session,
                                           __ATOMIC_RELAXED);

        console_tap_finish_release_locked(owner, session);
        __atomic_store_n(&console_tap_control, 0u, __ATOMIC_RELEASE);
    }
}

static void console_tap_capture(const char *data, size_t length,
                               uint8_t hart)
{
    uint32_t session = __atomic_load_n(&console_tap_active_session,
                                       __ATOMIC_ACQUIRE);

    if (!session)
        return;

    if (!console_tap_trylock())
    {
        /* This record was observed during a live claim. Account the loss in
         * a lifetime counter; it may be recorded just after release/reclaim.
         */
        (void)__atomic_fetch_add(&console_tap_lifetime_drops, 1u,
                                 __ATOMIC_RELAXED);
        return;
    }

    /* A claim/release may have changed the epoch after the initial load. A
     * record is never moved across that boundary into another consumer's
     * session.
     */
    if (__atomic_load_n(&console_tap_active_session,
                        __ATOMIC_ACQUIRE) == session &&
        !p4_console_ring_push(&console_tap_queue, data, length, hart))
        (void)__atomic_fetch_add(&console_tap_lifetime_drops, 1u,
                                 __ATOMIC_RELAXED);

    console_tap_unlock();
}

static uint32_t console_tap_claim(void *owner)
{
    uint32_t session;

    if (!owner || !console_tap_trylock())
        return 0u;
    if (__atomic_load_n(&console_tap_active_session, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&console_tap_owner, __ATOMIC_RELAXED) != NULL ||
        __atomic_load_n(&console_tap_session, __ATOMIC_RELAXED) != 0u)
    {
        /* A release can revoke active_session just before its control-lock
         * handoff. Do not admit a replacement owner while those old lease
         * fields remain; the releasing call or its lock holder must finish
         * that exact lease first.
         */
        console_tap_unlock();
        return 0u;
    }

    console_tap_clear_locked();
    session = console_tap_epoch + 1u;
    if (!session)
        session = 1u;
    console_tap_epoch = session;
    __atomic_store_n(&console_tap_owner, owner, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_session, session, __ATOMIC_RELAXED);
    __atomic_store_n(&console_tap_active_session, session, __ATOMIC_RELEASE);
    console_tap_unlock();
    return session;
}

static void console_tap_release(void *owner, uint32_t session)
{
    uint32_t expected_session;
    uint32_t expected_state;

    if (!owner || !session)
        return;

    if (console_tap_trylock())
    {
        if (__atomic_load_n(&console_tap_owner, __ATOMIC_RELAXED) == owner &&
            __atomic_load_n(&console_tap_session, __ATOMIC_RELAXED) == session)
        {
            /* Only the release that wins revocation may perform direct
             * cleanup. If active_session is already zero, another release
             * owns the interval between revocation and cleanup; leave its
             * owner/session fields intact for that caller (or state-3
             * unlock) to finish.
             */
            expected_session = session;
            if (__atomic_compare_exchange_n(&console_tap_active_session,
                                           &expected_session, 0u, 0,
                                           __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE))
                console_tap_finish_release_locked(owner, session);
        }
        console_tap_unlock();
        return;
    }

    /* A void release must still revoke promptly when a bounded producer or
     * pop has the lock. The owner of state 1 observes state 3 before unlock
     * and clears the ring. If that owner just unlocked, this caller takes the
     * lock once and performs the same cleanup itself.
     */
    if (__atomic_load_n(&console_tap_owner, __ATOMIC_ACQUIRE) != owner ||
        __atomic_load_n(&console_tap_session, __ATOMIC_ACQUIRE) != session)
        return;

    expected_session = session;
    if (!__atomic_compare_exchange_n(&console_tap_active_session,
                                    &expected_session, 0u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    expected_state = 1u;
    if (__atomic_compare_exchange_n(&console_tap_control, &expected_state,
                                    3u, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return;

    if (console_tap_trylock())
    {
        console_tap_finish_release_locked(owner, session);
        console_tap_unlock();
    }
}

static int console_tap_pop(void *owner, uint32_t session,
                           struct p4_console_record *record)
{
    int result = 0;

    if (!owner || !session || !record || !console_tap_trylock())
        return 0;

    if (__atomic_load_n(&console_tap_active_session, __ATOMIC_ACQUIRE) ==
            session &&
        __atomic_load_n(&console_tap_owner, __ATOMIC_RELAXED) == owner &&
        __atomic_load_n(&console_tap_session, __ATOMIC_RELAXED) == session)
        result = p4_console_ring_pop(&console_tap_queue, record);

    console_tap_unlock();
    return result;
}

static uint32_t console_tap_dropped(void *owner, uint32_t session)
{
    uint32_t active;

    if (!owner || !session)
        return 0u;
    active = __atomic_load_n(&console_tap_active_session, __ATOMIC_ACQUIRE);
    if (active != session ||
        __atomic_load_n(&console_tap_owner, __ATOMIC_ACQUIRE) != owner ||
        __atomic_load_n(&console_tap_session, __ATOMIC_ACQUIRE) != session)
        return 0u;

    return __atomic_load_n(&console_tap_lifetime_drops, __ATOMIC_RELAXED);
}

static struct P4ConsoleTapResource console_tap_resource =
{
    .node = { NULL, NULL, NT_RESOURCE, 0,
              (char *)P4_CONSOLE_TAP_NAME },
    .abi = P4_CONSOLE_TAP_ABI,
    .size = sizeof(struct P4ConsoleTapResource),
    .claim = console_tap_claim,
    .release = console_tap_release,
    .pop = console_tap_pop,
    .dropped = console_tap_dropped
};

void krnP4ConsoleTapRegister(struct ExecBase *SysBase)
{
    uint32_t expected = 0u;

    if (!SysBase ||
        !__atomic_compare_exchange_n(&console_tap_registered, &expected, 1u,
                                     0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;

    AddResource(&console_tap_resource);
    __atomic_store_n(&console_tap_registered, 2u, __ATOMIC_RELEASE);
}

/* Fixed hexadecimal fields require no formatting library, allocation or
 * recursive console calls. In diagnostic mode len describes the original
 * payload; its synthetic framing newline is omitted in ordinary mode. */
static void console_frame_text(const char *s)
{
    while (*s)
        console_frame[console_frame_len++] = *s++;
}

static void console_frame_hex(uint32_t n)
{
    static const char digits[] = "0123456789abcdef";
    unsigned int i;
    for (i = 0; i < 8; i++)
        console_frame[console_frame_len++] = digits[(n >> (28 - i * 4)) & 15];
}

void krnP4ConsoleSenderReady(void)
{
    __atomic_store_n(&console_sender_ready, 1, __ATOMIC_RELEASE);
}

void krnP4ConsoleSenderFailed(void)
{
    /* No replay promise after a timer failure: queued records and a partial
     * frame become undeliverable. The caller explicitly reports the failed
     * sender through the bounded direct path. Never block trying to empty
     * the ring from a failing scheduler service. */
    __atomic_store_n(&console_sender_ready, 0, __ATOMIC_RELEASE);
}

/* One writer task; each call holds the hardware lock with interrupts masked
 * for at most budget single-probe byte writes. Never waits for USB or UART.
 * Keep an unsent suffix across calls, including an overflow report. */
unsigned int krnP4ConsoleDrain(unsigned int budget)
{
    unsigned long s = p4_tls_mask();
    uint32_t free = 0;
    unsigned int sent = 0;
    struct p4_console_record record;
    uint32_t drops;

    if (!__atomic_load_n(&console_sender_ready, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) ||
        !__atomic_compare_exchange_n(&console_holder, &free,
                                     console_hart() + 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        p4_tls_unmask(s);
        return 0;
    }
    if (budget > 256)
        budget = 256;
    while (sent < budget)
    {
        if (console_frame_pos == console_frame_len)
        {
            console_frame_pos = console_frame_len = 0;
            drops = p4_console_ring_dropped(&console_queue);
            if (drops != console_reported_drops && !console_last_overflow)
            {
                console_frame_text("[p4log overflow records=");
                console_frame_hex(drops);
                console_frame_text("]\n");
                console_reported_drops = drops;
                console_last_overflow = 1;
            }
            else if (p4_console_ring_pop(&console_queue, &record))
            {
                unsigned int i;
                console_last_overflow = 0;
#if P4_CONSOLE_RECORD_PREFIX
                console_frame_text("[p4log seq=");
                console_frame_hex(record.sequence);
                console_frame_text(" hart=");
                console_frame_hex(record.hart);
                console_frame_text(" len=");
                console_frame_hex(record.length);
                console_frame_text("] ");
#endif
                for (i = 0; i < record.length; i++)
                    console_frame[console_frame_len++] = record.data[i];
#if P4_CONSOLE_RECORD_PREFIX
                if (record.data[record.length - 1] != '\n')
                    console_frame[console_frame_len++] = '\n';
#endif
            }
            else
            {
                console_last_overflow = 0;
                break;
            }
        }
#if P4_CONSOLE_USB
        if (!(mmio_rd(P4_USJ_BASE, P4_USJ_EP1_CONF) & P4_USJ_IN_EP_DATA_FREE))
            break;
        mmio_wr(P4_USJ_BASE, P4_USJ_EP1,
                (unsigned char)console_frame[console_frame_pos]);
        if (++usj_pending >= P4_USJ_EP1_DEPTH ||
            console_frame[console_frame_pos] == '\n')
            usj_flush_nowait();
#else
        if (((mmio_rd(P4_UART0_BASE, P4_UART_STATUS) >> P4_UART_TXFIFO_CNT_S)
             & P4_UART_TXFIFO_CNT_M) >= P4_UART_FIFO_DEPTH)
            break;
        mmio_wr(P4_UART0_BASE, P4_UART_FIFO,
                (unsigned char)console_frame[console_frame_pos]);
#endif
        console_frame_pos++;
        sent++;
    }
#if P4_CONSOLE_USB
    /* Ship short records/prefixes even when a budget boundary splits them. */
    usj_flush_nowait();
#endif
    __atomic_store_n(&console_holder, 0, __ATOMIC_RELEASE);
    p4_tls_unmask(s);
    return sent;
}

static inline unsigned int console_hart(void)
{
    unsigned long hart;

    asm volatile("csrr %0, mhartid" : "=r"(hart));
    return (unsigned int)hart & (P4_TLS_HARTS - 1);
}

/* Interrupts are masked by the caller */
static void console_emit(unsigned int hart)
{
    uint32_t me = hart + 1;
    uint32_t free = 0;
    unsigned int spins = 0;
    int locked = 0;
    unsigned int i;

    /* Mirror the complete record before either the existing queue or the
     * bounded direct fallback consumes the per-hart line. The tap owns a
     * separate ring and never participates in the USB/UART transmit path.
     */
    console_tap_capture(console_line[hart], console_len[hart], (uint8_t)hart);

    if (__atomic_load_n(&console_runtime, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&console_sender_ready, __ATOMIC_ACQUIRE))
    {
        (void)p4_console_ring_push(&console_queue, console_line[hart],
                                  console_len[hart], (uint8_t)hart);
        console_len[hart] = 0;
        return;
    }

    if (__atomic_load_n(&console_holder, __ATOMIC_ACQUIRE) != me)
    {
        for (;;)
        {
            free = 0;
            if (__atomic_compare_exchange_n(&console_holder, &free, me, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            {
                locked = 1;
                break;
            }
            if (++spins == CONSOLE_LOCK_SPINS)
                break;
        }
    }

    for (i = 0; i < console_len[hart]; i++)
        console_raw_putc(console_line[hart][i]);
    console_len[hart] = 0;

    if (locked)
        __atomic_store_n(&console_holder, 0, __ATOMIC_RELEASE);
}

/* What this hart has of an unfinished line, before it waits for input */
static void console_flush_self(void)
{
    unsigned long s = p4_tls_mask();
    unsigned int hart = console_hart();

    if (console_len[hart])
        console_emit(hart);
    p4_tls_unmask(s);
}

void krnP4PutC(char c)
{
    unsigned long s = p4_tls_mask();
    unsigned int hart = console_hart();

    console_line[hart][console_len[hart]++] = c;
    if (c == '\n' || console_len[hart] == CONSOLE_LINE)
        console_emit(hart);
    p4_tls_unmask(s);
}
#endif

void krnP4PutStr(const char *s)
{
    while (*s)
        krnP4PutC(*s++);
}

void krnP4PutHex32(uint32_t val)
{
    static const char hexchars[] = "0123456789abcdef";
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hexchars[(val >> (28 - i * 4)) & 0xF];
    buf[10] = '\0';

    krnP4PutStr(buf);
}

void krnP4PutDec(uint32_t val)
{
    char buf[11];
    int i = 10;

    buf[i] = '\0';
    do
    {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    } while (val && i > 0);

    krnP4PutStr(&buf[i]);
}

/*
 * Resident and node priorities are signed bytes, and printing them through
 * krnP4PutDec() turns -120 into 4294967176 - a number that says nothing and
 * cannot be compared against the priority written in a .conf file.  The
 * negation is done on the unsigned value so that INT32_MIN has no special
 * case.
 */
void krnP4PutDecS(int32_t val)
{
    if (val < 0)
    {
        krnP4PutStr("-");
        krnP4PutDec(-(uint32_t)val);
    }
    else
        krnP4PutDec((uint32_t)val);
}
