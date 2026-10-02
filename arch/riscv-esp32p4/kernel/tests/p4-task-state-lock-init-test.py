#!/usr/bin/env python3
"""Extract the real task-state lock initialization in both creation paths."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
cases = (
    ("rom/exec/newaddtask.c", "task", "if (!InitETask(task, parent))"),
    ("rom/exec/exec_init.c", "t", "SET_THIS_TASK(t);"),
)
snippets = []
for filename, variable, publication in cases:
    source = (repo / filename).read_text()
    statement = f"EXEC_SPINLOCK_INIT(&{variable}->tc_SpinLock);"
    position = source.index(statement)
    start = source.rfind("#if ", 0, position)
    end = source.index("#endif", position) + len("#endif")
    assert source[start:source.index("\n", start)] == (
        "#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)"
    )
    assert position < source.index(publication)
    snippets.append(source[start:end])

fixture = """
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct Task { uint32_t tc_SpinLock; };
static int calls;
#define EXEC_SPINLOCK_INIT(p) do { *(p) = 0; ++calls; } while (0)
"""
for number, snippet in enumerate(snippets):
    variable = cases[number][1]
    fixture += f"static void init{number}(struct Task *{variable}) {{\n{snippet}\n}}\n"
fixture += """
int main(void) {
    struct Task first = { UINT32_MAX }, boot = { UINT32_MAX };
    init0(&first); init1(&boot);
#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
    assert(calls == 2 && first.tc_SpinLock == 0 && boot.tc_SpinLock == 0);
#else
    assert(calls == 0 && first.tc_SpinLock == UINT32_MAX && boot.tc_SpinLock == UINT32_MAX);
#endif
    puts("PASS: extracted birth and bootstrap state-lock initialization");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="p4-task-state-lock-") as directory:
    source = Path(directory) / "fixture.c"
    source.write_text(fixture)
    for defines in ((), ("__AROSEXEC_SMP__",), ("__AROSPLATFORM_SMP__",),
                    ("__AROSEXEC_SMP__", "__AROSPLATFORM_SMP__")):
        binary = Path(directory) / ("test-" + str(len(defines)) + "-" + "-".join(defines))
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-variable",
                        "-fsanitize=address,undefined", *(f"-D{name}=1" for name in defines),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
