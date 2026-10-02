/* E2-A1 SRAM-only slice. No allocation, console, peripheral or Exec calls. */
#include <stdint.h>
#include "secondary_hw.h"
#include "secondary_mailbox.h"
#define MAIL_SRAM __attribute__((section(".sramtext.secondary_mailbox"), noinline))
struct p4_mailbox __p4_secondary_mailbox;
extern uint32_t __p4_secondary_report[];
#ifdef P4_E2_PRIMITIVES
#include "secondary_primitives.h"
#endif
static inline __attribute__((always_inline)) uintptr_t mail_uncached(void *p)
{
    uintptr_t alias;
    /* Keep the alias conversion as an actual add, as in secondary_entry.S.
       A folded +0x40000010 pointer falsely resembles an XIP reference in
       objdump and must not weaken the SRAM residency checker. */
    __asm__ volatile("add %0, %1, %2" : "=r"(alias)
        : "r"((uintptr_t)p), "r"(P4_SECONDARY_UNCACHED_OFFSET));
    return alias;
}
MAIL_SRAM void krnP4SecondaryMailboxWorker(void)
{
    volatile struct p4_mailbox *mail = (volatile struct p4_mailbox *)
        mail_uncached(&__p4_secondary_mailbox);
    volatile uint32_t *report = (volatile uint32_t *)
        mail_uncached(__p4_secondary_report);
    struct p4_mail_state state = {0, 0, 0};
    uint32_t epoch = report[4], start, now;
#ifdef P4_E2_PRIMITIVES
    krnP4E2WorkerInit();
#endif
    unsigned int left = 20000000;
    __asm__ volatile("csrr %0, mcycle" : "=r"(start));
    /* Worker bounds are independent of primary acknowledgement timeouts.
       1.8B cycles = five seconds at CPU360, safely below unsigned wrap. */
    do {
#ifdef P4_E2_PRIMITIVES
        /* Acquire the published ticket BEFORE selecting its payload lane.
           Selecting mode first could combine an old SRAM mode with a new
           PSRAM ticket, despite p4_mail_step_payload's internal fence. */
        {
            volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)report[8];
            p4_mail_step_selected(mail, &state, epoch,
                                  p ? p + 16 : 0, p ? p + 272 : 0);
        }
#else
        p4_mail_step(mail, &state, epoch);
#endif
#ifdef P4_E2_PRIMITIVES
        krnP4E2WorkerStep();
#endif
        __asm__ volatile("csrr %0, mcycle" : "=r"(now));
    } while (--left && (uint32_t)(now - start) < 1800000000u);
    /* Return into private WFI park; hart0 must still assert reset. */
}
