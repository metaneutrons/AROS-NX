/* Stack storage belongs to the linked kernel BSS, outside Exec's allocator. */
#include <stddef.h>
#include "p4_isr_stack.h"
#if !defined(P4_ISR_STACK_TEST)
#if !defined(__AROSEXEC_SMP__)
#error "P4 per-hart interrupt stacks require explicit Exec SMP selection"
#endif
static uint32_t p4_isr_stack_hart(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, mhartid" : "=r"(value));
    return value;
}
static uint32_t p4_isr_stack_status(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}
static uintptr_t p4_isr_stack_scratch(void)
{
    uintptr_t value;
    __asm__ volatile("csrr %0, mscratch" : "=r"(value));
    return value;
}
static void p4_isr_stack_set_scratch(uintptr_t top)
{
    __asm__ volatile("fence rw,rw\n\tcsrw mscratch, %0" : : "r"(top) : "memory");
}
#endif

struct p4_isr_stack_storage
{
    uint32_t guard[P4_ISR_STACK_GUARD_BYTES / sizeof(uint32_t)];
    unsigned char stack[P4_ISR_STACK_BYTES];
} __attribute__((aligned(64)));
static struct p4_isr_stack_storage p4_isr_stacks[P4_ISR_STACK_HARTS];
static unsigned char p4_isr_stack_installed[P4_ISR_STACK_HARTS];
#define P4_ISR_STACK_GUARD_WORD UINT32_C(0xa5a55a5a)

int krnP4ISRStackInstall(void)
{
    uint32_t hart = p4_isr_stack_hart();
    uintptr_t top;
    size_t word;
    if (hart >= P4_ISR_STACK_HARTS || (p4_isr_stack_status() & 8u) != 0 ||
        p4_isr_stack_scratch() != 0 || p4_isr_stack_installed[hart])
        return 0;
    top = (uintptr_t)&p4_isr_stacks[hart].stack[P4_ISR_STACK_BYTES];
    for (word = 0; word < P4_ISR_STACK_GUARD_BYTES / sizeof(uint32_t); ++word)
        p4_isr_stacks[hart].guard[word] = P4_ISR_STACK_GUARD_WORD;
    p4_isr_stack_installed[hart] = 1;
    p4_isr_stack_set_scratch(top);
    return p4_isr_stack_scratch() == top;
}

int krnP4ISRStackFrameValid(const void *frame, uint32_t frame_bytes)
{
    uint32_t hart = p4_isr_stack_hart();
    uintptr_t begin, end, address = (uintptr_t)frame;
    size_t word;
    if (hart >= P4_ISR_STACK_HARTS || !p4_isr_stack_installed[hart] ||
        frame_bytes == 0 || (address & 15u) != 0)
        return 0;
    begin = (uintptr_t)&p4_isr_stacks[hart].stack[0];
    end = begin + P4_ISR_STACK_BYTES;
    if (address < begin + P4_ISR_STACK_C_MARGIN_BYTES || address > end ||
        frame_bytes > end - address)
        return 0;
    for (word = 0; word < P4_ISR_STACK_GUARD_BYTES / sizeof(uint32_t); ++word)
        if (p4_isr_stacks[hart].guard[word] != P4_ISR_STACK_GUARD_WORD)
            return 0;
    return 1;
}
