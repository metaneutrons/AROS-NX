/* Isolated, opt-in HP core 1 experiment. No Exec SMP or second producer. */
#include <stdint.h>
#include "secondary_hw.h"
#ifdef P4_SECONDARY_HOST_TEST
#include "tests/secondary-probe-mock.h"
#else
#include "hardware.h"
#include "psram.h"
#include "kernel_intern.h"
#endif

extern unsigned char __p4_secondary_entry[], __p4_secondary_entry_end[];
extern uint32_t __p4_secondary_report[], __p4_secondary_stack_end[];
extern uint32_t __p4_secondary_guard_low[], __p4_secondary_guard_high[];
extern uint32_t __p4_secondary_boot_snapshot[8];

static void barrier(void)
{
#ifndef P4_SECONDARY_HOST_TEST
    asm volatile("fence iorw, iorw" ::: "memory");
#endif
}

static uint32_t cycles(void)
{
#ifdef P4_SECONDARY_HOST_TEST
    return mock_cycles();
#else
    uint32_t value;
    asm volatile("csrr %0, mcycle" : "=r"(value));
    return value;
#endif
}

static volatile uint32_t *uncached(uint32_t *address)
{
#ifdef P4_SECONDARY_HOST_TEST
    return address;
#else
    return (volatile uint32_t *)((uintptr_t)address +
                                P4_SECONDARY_UNCACHED_OFFSET);
#endif
}

static void update(uint32_t address, uint32_t mask, uint32_t value)
{
    p4_w32(address, (p4_r32(address) & ~mask) | value);
    barrier();
}

static int quiesce(void)
{
    /* Global reset + gated CPU clock, not the unreliable WFI stall bit. */
    update(P4_SECONDARY_RESET_REG, P4_SECONDARY_RESET_BIT,
           P4_SECONDARY_RESET_BIT);
    update(P4_SECONDARY_CLOCK_REG, P4_SECONDARY_CLOCK_BIT, 0);
    update(P4_SECONDARY_STALL_REG, P4_SECONDARY_STALL_MASK,
           P4_SECONDARY_STALL);
    p4_w32(P4_SECONDARY_BOOT_REG, 0);
    barrier();
    return (p4_r32(P4_SECONDARY_RESET_REG) & P4_SECONDARY_RESET_BIT) &&
           !(p4_r32(P4_SECONDARY_CLOCK_REG) & P4_SECONDARY_CLOCK_BIT) &&
           p4_r32(P4_SECONDARY_BOOT_REG) == 0;
}

static int wait_report(volatile uint32_t *report, uint32_t nonce)
{
    uint32_t start = cycles();
    unsigned int left = 1000000;
    /* Two independent bounds; cycle wrap is intentional unsigned math.
       36M cycles is 100ms at CPU360; the iteration cap covers dead mcycle. */
    do
    {
        uint32_t state = report[1];
        barrier();
        if (state == 2)
            return -1;
        if (state == 1)
            return report[0] == 1 && report[5] == nonce &&
                   report[6] == (uint32_t)(uintptr_t)__p4_secondary_stack_end
                   ? 1 : -1;
    } while (--left && (uint32_t)(cycles() - start) < 36000000);
    return 0;
}

#ifdef P4_E2_MAILBOX
#include "secondary_mailbox_probe.h"
#endif
#ifdef P4_E2_PRIMITIVES
#include "secondary_primitives.h"
#endif

static int attempt(int launch, uint32_t nonce, int retain)
{
    volatile uint32_t *report = uncached(__p4_secondary_report);
    volatile uint32_t *low = uncached(__p4_secondary_guard_low);
    volatile uint32_t *high = uncached(__p4_secondary_guard_high);
    unsigned int i;
    int result;
    if (!quiesce())
        return -2;
#ifdef P4_E2_MAILBOX
    mailbox_prepare();
#endif
    /* Evict the BSS aliases BEFORE uncached writes: a later dirty zero
       writeback must not replace the secondary's report/canaries. */
    krnP4CacheSyncData(__p4_secondary_report, 64);
    krnP4CacheSyncData(__p4_secondary_guard_low, 64);
    krnP4CacheSyncData(__p4_secondary_guard_high, 64);
    for (i = 0; i < 16; ++i)
    {
        report[i] = 0;
        low[i] = 0x53a17e01;
        high[i] = 0x53a17e02;
    }
    report[4] = nonce;
#ifdef P4_E2_PRIMITIVES
    /* The suppressed release checks reset isolation only. Do not consume
       one-shot primitive preparation without running its completion gate. */
    if (launch && !krnP4E2Prepare())
        return -1; /* already reset-held; no PSRAM access without capacity */
    report[8] = P4_FB_BASE - 4096;
#endif
    barrier();
    if (launch)
    {
        krnP4SyncCode(__p4_secondary_entry,
                     (uintptr_t)__p4_secondary_entry_end -
                     (uintptr_t)__p4_secondary_entry);
        update(P4_SECONDARY_STALL_REG, P4_SECONDARY_STALL_MASK,
               P4_SECONDARY_UNSTALL);
        update(P4_SECONDARY_CLOCK_REG, P4_SECONDARY_CLOCK_BIT,
               P4_SECONDARY_CLOCK_BIT);
        update(P4_SECONDARY_RESET_REG, P4_SECONDARY_RESET_BIT, 0);
        /* IDF order; actual rev1.3 ROM setter disassembled to precisely
           this store (0x4fc058e2). No ROM polling loop imported. */
        p4_w32(P4_SECONDARY_BOOT_REG,
               (uint32_t)(uintptr_t)__p4_secondary_entry);
        barrier();
    }
    result = wait_report(report, nonce);
#ifdef P4_E2_MAILBOX
    if (launch && result == 1 && !mailbox_run(nonce))
        result = -1;
#endif
#ifdef P4_E2_PRIMITIVES
    if (launch && result == 1 && !krnP4E2Run(nonce))
        result = -1;
#endif
    for (i = 0; i < 16; ++i)
        if (low[i] != 0x53a17e01 || high[i] != 0x53a17e02)
            result = -1;
    /* Retention is only for a verified positive report. Both harts park;
       no Exec, cache operation or second producer may follow that exit. */
    if (retain && launch && result == 1)
    {
        barrier();
        if (!(p4_r32(P4_SECONDARY_RESET_REG) & P4_SECONDARY_RESET_BIT) &&
            (p4_r32(P4_SECONDARY_CLOCK_REG) & P4_SECONDARY_CLOCK_BIT) &&
            p4_r32(P4_SECONDARY_BOOT_REG) ==
                (uint32_t)(uintptr_t)__p4_secondary_entry &&
            (p4_r32(P4_SECONDARY_STALL_REG) & P4_SECONDARY_STALL_MASK) ==
                P4_SECONDARY_UNSTALL && report[1] == 1)
            retain = 1;
        else
        {
            retain = 0;
            result = -1;
        }
    }
    else
        retain = 0;
    /* Normal and every failure exit stops the target before logging/Exec. */
    if (!retain && !quiesce())
        return -2;
#ifdef P4_E2_PRIMITIVES
    /* Hard reset is the ISR completion barrier; only now restore routing. */
    if (launch) {
        krnP4PutStr("[smp-e2] teardown; secondary reset-held/clock-off\n");
        krnP4E2Teardown();
    }
#endif
    krnP4PutStr(launch ? "[smp] release state=" : "[smp] suppressed state=");
    krnP4PutHex32(report[1]);
    krnP4PutStr(" hart="); krnP4PutHex32(report[0]);
    krnP4PutStr(" echo="); krnP4PutHex32(report[5]);
    krnP4PutStr(" cause="); krnP4PutHex32(report[2]);
    krnP4PutStr(" pc="); krnP4PutHex32(report[3]);
    krnP4PutStr(" result="); krnP4PutHex32((uint32_t)result);
    krnP4PutStr(retain ? " reset=clear clock=on\n" : " reset=held clock=off\n");
    return result;
}

int krnP4SecondaryProbe(void)
{
    int negative, positive;
    int retain = 0;
    krnP4PutStr("[smp] early inherited reset=");
    krnP4PutHex32(__p4_secondary_boot_snapshot[0]);
    krnP4PutStr(" clock="); krnP4PutHex32(__p4_secondary_boot_snapshot[1]);
    krnP4PutStr("\n[smp] early isolation reset=");
    krnP4PutHex32(__p4_secondary_boot_snapshot[2]);
    krnP4PutStr(" clock="); krnP4PutHex32(__p4_secondary_boot_snapshot[3]);
    krnP4PutStr(" boot="); krnP4PutHex32(__p4_secondary_boot_snapshot[4]);
    krnP4PutStr("\n");
    if (!(__p4_secondary_boot_snapshot[2] & P4_SECONDARY_RESET_BIT) ||
        (__p4_secondary_boot_snapshot[3] & P4_SECONDARY_CLOCK_BIT) ||
        __p4_secondary_boot_snapshot[4])
        return 0;
    krnP4PutStr("[smp] isolated core1 probe; Exec remains single-hart\n");
    negative = attempt(0, 0xe1000001, 0);
    if (negative == -2)
        return 0;
#ifdef P4_SECONDARY_RETAIN
    retain = negative == 0;
#endif
    positive = attempt(1, 0xe1000002, retain);
    if (positive == -2)
        return 0;
#ifdef P4_E2_MAILBOX
    /* A fresh epoch after a hard hart1 reset must restart sequence at one.
       No runtime/Exec continuation is qualified by this diagnostic. */
    if (negative == 0 && positive == 1)
        positive = attempt(1, 0xe2000003, 0);
    if (positive == -2)
        return 0;
#ifdef P4_E2_PRIMITIVES
    krnP4PutStr(negative == 0 && positive == 1
        ? "[smp-e2] PRIMITIVES PASS; epochs=2 secondary stopped\n"
        : "[smp-e2] PRIMITIVES FAIL; secondary stopped\n");
#else
    krnP4PutStr(negative == 0 && positive == 1
        ? "[smp-e2] SRAM PASS; epochs=2 exchanges=130 refusals=12 missing-ack=2 secondary stopped\n"
        : "[smp-e2] SRAM FAIL; secondary stopped\n");
#endif
    return 3; /* caller parks safely before Exec, including diagnostic failure */
#endif
    if (retain && positive == 1)
    {
        krnP4PutStr("[smp] retained PASS; hart1=1 guards=ok reset=clear clock=on\n");
        return 2; /* The caller MUST park before Exec. */
    }
    krnP4PutStr(negative == 0 && positive == 1
               ? "[smp] probe PASS; secondary stopped before Exec\n"
               : "[smp] probe FAIL; safely continuing single-hart\n");
    return 1; /* isolation, not the diagnostic acceptance result */
}
