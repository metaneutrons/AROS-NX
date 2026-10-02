#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t hart, status;
static uintptr_t scratch[2];
static int refuse_write;
static uint32_t p4_isr_stack_hart(void) { return hart; }
static uint32_t p4_isr_stack_status(void) { return status; }
static uintptr_t p4_isr_stack_scratch(void) { return hart < 2 ? scratch[hart] : 0; }
static void p4_isr_stack_set_scratch(uintptr_t top) {
    assert(hart < 2 && !(status & 8));
    if (!refuse_write) scratch[hart] = top;
}
#define P4_ISR_STACK_TEST 1
#include "../p4_isr_stack.c"
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)
int main(void)
{
    hart = 2;
    CHECK(!krnP4ISRStackInstall());
    CHECK(!krnP4ISRStackFrameValid((void *)16, 288));
    hart = 0; status = 8;
    CHECK(!krnP4ISRStackInstall());
    CHECK(!p4_isr_stack_installed[0]);
    status = 0; scratch[0] = 16;
    CHECK(!krnP4ISRStackInstall());
    CHECK(!p4_isr_stack_installed[0]);
    scratch[0] = 0;
    CHECK(krnP4ISRStackInstall());
    CHECK(!krnP4ISRStackInstall());
    uintptr_t end = scratch[0];
    CHECK((end & 63) == 0);
    CHECK(krnP4ISRStackFrameValid((void *)(end - 288), 288));
    CHECK(krnP4ISRStackFrameValid((void *)(end - 576), 288));
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 287), 288));
    CHECK(!krnP4ISRStackFrameValid((void *)end, 288));
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 288), 0));
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 288), UINT32_MAX));
    uintptr_t limit = (uintptr_t)p4_isr_stacks[0].stack + P4_ISR_STACK_C_MARGIN_BYTES;
    CHECK(krnP4ISRStackFrameValid((void *)limit, 288));
    CHECK(!krnP4ISRStackFrameValid((void *)(limit - 16), 288));
    CHECK(!krnP4ISRStackFrameValid((void *)0, 288));
    CHECK(!krnP4ISRStackFrameValid((void *)(UINTPTR_MAX & ~(uintptr_t)15), 288));
    p4_isr_stacks[0].guard[15] ^= 1;
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 288), 288));
    p4_isr_stacks[0].guard[15] ^= 1;
    hart = 1;
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 288), 288));
    CHECK(krnP4ISRStackInstall());
    CHECK(scratch[1] != end);
    CHECK(!krnP4ISRStackFrameValid((void *)(end - 288), 288));
    CHECK(krnP4ISRStackFrameValid((void *)(scratch[1] - 288), 288));
    /* A failed CSR install is terminal for that boot, never silently retry. */
    memset(p4_isr_stack_installed, 0, sizeof p4_isr_stack_installed);
    scratch[1] = 0; refuse_write = 1;
    CHECK(!krnP4ISRStackInstall());
    CHECK(p4_isr_stack_installed[1]);
    CHECK(!krnP4ISRStackInstall());
    printf("PASS: %u actual ISR-stack storage checks\n", checks);
}
