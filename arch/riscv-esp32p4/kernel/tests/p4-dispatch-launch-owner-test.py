#!/usr/bin/env python3
"""Exercise extracted dispatch callback sites; not a scheduler/FPU test."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
generic = (repo / "rom/kernel/kernel_scheduler.c").read_text()
cpu = (repo / "arch/riscv-esp32p4/kernel/kernel_cpu.c").read_text()
platform = (repo / "arch/riscv-esp32p4/exec/exec_platform.h").read_text()
assert platform.index("#define EXEC_DISPATCH_LAUNCH_IN_CPU 1") < platform.index("#else\n\n/* Preserve")
start = generic.index("#if !defined(EXEC_DISPATCH_LAUNCH_IN_CPU)")
generic_site = generic[start:generic.index("#endif", start) + len("#endif")]
start = cpu.index("    if (task->tc_Flags & TF_LAUNCH)")
cpu_site = cpu[start:cpu.index("\n    }", start) + len("\n    }")]
assert cpu.index("copyContext(regs, ctx);") < start
assert cpu.index("        Exception();") < start
fixture = r'''
#include <assert.h>
#include <stdio.h>
struct ExecBase { int unused; } base;
struct Task { unsigned tc_Flags; void (*tc_Launch)(struct ExecBase *); };
#define TF_LAUNCH 1u
#define SysBase (&base)
#define AROS_UFCA(type, value, reg) (value)
#define AROS_UFC1NR(type, function, arg) (function)(arg)
#define AROS_UFC1(type, function, arg) (function)(arg)
static int calls, restored;
static void callback(struct ExecBase *b) {
    assert(b == &base);
#if defined(EXEC_DISPATCH_LAUNCH_IN_CPU)
    assert(restored);
#endif
    ++calls;
}
'''
fixture += "static void core(struct Task *task) { (void)task;\n" + generic_site + "\n}\n"
fixture += "static void cpu(struct Task *task) { restored = 1;\n" + cpu_site + "\n}\n"
fixture += r'''
int main(void) {
    struct Task task = { TF_LAUNCH, callback };
    for (int i = 0; i < 3; ++i) {
        calls = restored = 0;
        core(&task); cpu(&task);
#if defined(EXEC_DISPATCH_LAUNCH_IN_CPU)
        assert(calls == 1);
#else
        /* Preserve and expose the existing normal-P4 duplicate separately. */
        assert(calls == 2);
#endif
    }
    task.tc_Flags = 0; calls = 0;
    core(&task); cpu(&task); assert(calls == 0);
    puts("PASS: extracted dispatch launch-owner sites");
}
'''
with tempfile.TemporaryDirectory(prefix="p4-launch-owner-") as directory:
    source = Path(directory) / "fixture.c"
    source.write_text(fixture)
    for selected in (False, True):
        binary = Path(directory) / str(selected)
        flags = ["-DEXEC_DISPATCH_LAUNCH_IN_CPU=1"] if selected else []
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", *flags,
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
