#!/usr/bin/env python3
"""Compile the actual trap-exit body with mocked IRQ delivery.

Software-interrupt handling may return with MIE enabled. Delivering an IRQ
in the C-return/assembly-restore window must not schedule an ISR-stack frame
as a task. The pre-fix handler is a mandatory negative control.
"""
import pathlib
import subprocess
import tempfile

kernel = pathlib.Path(__file__).resolve().parents[1]
source = (kernel / "kernel_traps.c").read_text()
function = source[source.index("void krnTrapHandler("):]
mask = 'asm volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE) : "memory");'
assert function.count(mask) == 1, "trap-exit mask missing or ambiguous"

fixture = r'''
#include <assert.h>
#include <stdio.h>
#define TRAP_DONE 0
#define TRAP_RESCHEDULE 1
#define TRAP_SYSCALL 2
#define CTX_REG_A7 14
struct ExceptionContext { unsigned x[29]; int on_isr_stack; };
static int depth, irq_enabled, schedules, corrupt_contexts, action;
static int enable_in_handler;
static void *SysBase;
#define TRAP_DEPTH depth
static int krnTrapDispatch(struct ExceptionContext *ctx,
                          unsigned long cause, unsigned long value)
{
    (void)ctx; (void)cause; (void)value;
    return action;
}
static void core_ExitInterrupt(struct ExceptionContext *ctx)
{
    schedules++;
    corrupt_contexts += ctx->on_isr_stack;
    if (enable_in_handler) irq_enabled = 1; /* actual SoftIntDispatch exit */
}
static void core_SysCall(int sc, struct ExceptionContext *ctx)
{
    (void)sc;
    core_ExitInterrupt(ctx);
}
''' 

tests = r'''
static void run(int present, int nesting, int kind, int enables)
{
    struct ExceptionContext task = {{0}, 0};
    struct ExceptionContext nested = {{0}, 1};
    depth = nesting;
    irq_enabled = 0; schedules = 0; corrupt_contexts = 0;
    action = kind; enable_in_handler = enables;
    SysBase = present ? &task : 0;
    krnTrapHandler(&task, 0, 0);
    assert(depth == nesting);
    assert(schedules == (present && !nesting && kind != TRAP_DONE));
    /* Hardware can deliver this IRQ between C return and assembly restore
       only if the handler has left MIE set. mscratch is still zero. */
    if (irq_enabled)
        krnTrapHandler(&nested, 0x80000000UL, 0);
    assert(corrupt_contexts == 0);
    assert(!irq_enabled);
    /* mret may restore the TASK's MPIE after the entire frame is restored. */
    irq_enabled = 1;
    assert(irq_enabled && depth == nesting);
}
int main(void)
{
    for (int present = 0; present <= 1; present++)
        for (int nesting = 0; nesting < 3; nesting++)
            for (int kind = 0; kind < 3; kind++)
                for (int enables = 0; enables <= 1; enables++)
                    run(present, nesting, kind, enables);
    puts("trap exit: 36 cases passed");
}
'''

with tempfile.TemporaryDirectory(prefix="p4-trap-exit-") as temp:
    root = pathlib.Path(temp)
    for optimized in ("-O0", "-O2"):
        for fixed in (True, False):
            body = function.replace(mask, "irq_enabled = 0;" if fixed else "")
            unit = root / "fixture.c"
            unit.write_text(fixture + body + tests)
            binary = root / "fixture"
            subprocess.run(["cc", "-std=c11", optimized, "-Wall", "-Wextra",
                            "-Werror", str(unit), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if fixed:
                assert result.returncode == 0, result.stderr
                print(optimized, result.stdout.strip())
            else:
                assert result.returncode != 0, "pre-fix negative control passed"
                assert "corrupt_contexts" in result.stderr, result.stderr
                print(optimized, "pre-fix control rejected ISR-stack scheduling")
