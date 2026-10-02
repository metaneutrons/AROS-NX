#!/usr/bin/env python3
"""Compile the actual trap entry body with mocked transport and hart CSR.

This checks nesting/isolation, not assembly entry, interrupt delivery or SMP
 scheduler acceptance. Fatal-path assembly is mocked to escape the halt.
"""
from pathlib import Path
import subprocess
import tempfile

KERNEL = Path(__file__).resolve().parent.parent
source = (KERNEL / "kernel_traps.c").read_text()
start = source.index("void krnTrapHandler(")
body = source[start:]
body = body.replace("asm volatile", "TEST_ASM")
# The production function is the final declaration in this source.
assert body.rstrip().endswith("}")
super_source = (KERNEL / "issuper.c").read_text()
super_start = super_source.index("{", super_source.index("AROS_LH0I"))
super_body = super_source[super_start:]
assert super_body.rstrip().endswith("}")
fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "p4_isr_stack.h"
#include "p4_trap_frame.h"
static uint32_t hart;
static uint32_t read_hart(void) { return hart; }
#define P4_CPU_LOCAL_READ_MHARTID read_hart
#if defined(__APPLE__)
#define section(name) used
#endif
#include "cpu_local.c"
#if defined(__APPLE__)
#undef section
#endif
#define __AROSEXEC_SMP__ 1
#define TEST_ASM(...) test_asm()
#define TRAP_RESCHEDULE 1
#define TRAP_SYSCALL 2
#define CTX_REG_A7 14
#define AROS_LIBFUNC_INIT
#define AROS_LIBFUNC_EXIT
struct ExceptionContext { unsigned long x[32]; };
static void *SysBase = (void *)1;
static int action, exits, calls, nested;
static unsigned long checks;
#define CHECK(x) do { checks++; if (!(x)) { \
 fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int stack_valid = 1, fatal_expected;
static jmp_buf fatal_jump;
static void test_asm(void) {
 if (fatal_expected) longjmp(fatal_jump, 1);
}
int krnP4ISRStackFrameValid(const void *frame, uint32_t bytes) {
 (void)frame; CHECK(bytes == P4_TRAP_FPU_FRAME_BYTES); return stack_valid;
}
void krnTrapHandler(struct ExceptionContext *, unsigned long, unsigned long);
static int krnTrapDispatch(struct ExceptionContext *ctx,
                          unsigned long cause, unsigned long value)
{ (void)ctx; (void)cause; (void)value; return action; }
static void core_ExitInterrupt(struct ExceptionContext *ctx)
{
 exits++;
 CHECK(p4_cpu_local_current()->trap_depth == 1);
 if (!nested) {
  nested = 1;
  krnTrapHandler(ctx, 0, 0);
  CHECK(p4_cpu_local_current()->trap_depth == 1);
 }
}
static void core_SysCall(int n, struct ExceptionContext *ctx)
{ (void)ctx; CHECK(n == 7); calls++; }
'''
main = r'''
int main(void)
{
 struct ExceptionContext ctx = {{0}};
 CHECK(krnP4CpuLocalPrepare());
 CHECK(!testIsSuper());
 ctx.x[CTX_REG_A7] = 7;
 action = TRAP_RESCHEDULE;
 krnTrapHandler(&ctx, 0, 0);
 CHECK(exits == 1);
 CHECK(__p4_cpu_locals[0].trap_depth == 0);
 CHECK(__p4_cpu_locals[1].trap_depth == 0);
 /* A trap on hart1 must not inherit hart0's supervisor state. */
 CHECK(p4_cpu_local_trap_enter(&__p4_cpu_locals[0], 0));
 CHECK(testIsSuper());
 hart = 1;
 CHECK(!testIsSuper());
 action = TRAP_SYSCALL;
 krnTrapHandler(&ctx, 0, 0);
 CHECK(calls == 1);
 CHECK(__p4_cpu_locals[1].trap_depth == 0);
 CHECK(__p4_cpu_locals[0].trap_depth == 1);
 CHECK(!testIsSuper());
 __p4_cpu_locals[1].hart_id = 0;
 CHECK(testIsSuper());
 __p4_cpu_locals[1].hart_id = 1;
 hart = 2;
 CHECK(testIsSuper());
 hart = 0;
 CHECK(p4_cpu_local_trap_leave(&__p4_cpu_locals[0], 0));
 SysBase = NULL;
 krnTrapHandler(&ctx, 0, 0);
 CHECK(calls == 1);
 CHECK(__p4_cpu_locals[0].trap_depth == 0);
 CHECK(!testIsSuper());
 /* Rejected ISR range/depth must halt before touching local trap state. */
 fatal_expected = 1; stack_valid = 0;
 if (!setjmp(fatal_jump)) { krnTrapHandler(&ctx, 0, 0); CHECK(0); }
 CHECK(__p4_cpu_locals[0].trap_depth == 0);
 stack_valid = 1;
 __p4_cpu_locals[0].trap_depth = P4_ISR_STACK_MAX_DEPTH;
 if (!setjmp(fatal_jump)) { krnTrapHandler(&ctx, 0, 0); CHECK(0); }
 CHECK(__p4_cpu_locals[0].trap_depth == P4_ISR_STACK_MAX_DEPTH);
 __p4_cpu_locals[0].trap_depth = 0; fatal_expected = 0;
 printf("PASS: %lu production-body SMP trap checks\n", checks);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-smp-trap-") as tmp:
    src = Path(tmp) / "trap.c"
    binary = Path(tmp) / "trap-test"
    src.write_text(fixture + body + "\nstatic int testIsSuper(void)\n" +
                   super_body + main)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(KERNEL),
                    str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
