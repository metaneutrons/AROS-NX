#!/usr/bin/env python3
"""Compile the production ETask construction prefix with allocator mocks."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
source = (repo / "rom/exec/exec_util.c").read_text()
start = source.index("BOOL\nExec_InitETask(")
end = source.index("    et->et_Parent = parent;", start)
body = source[start:end] + "    (void)parent; return TRUE;\n}\n"
fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define __AROSEXEC_SMP__ 1
#define D(...) ((void)0);
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define TF_ETASK 0x40u
#define EXECF_CPUAffinity 1u
#define TRUE 1
#define FALSE 0
typedef int BOOL;
struct ETask { unsigned dummy; };
struct IntETask { struct ETask pub; void *iet_CpuAffinity; unsigned iet_TaskLock; };
struct Task { struct { struct ETask *tc_ETask; } tc_UnionETask; unsigned tc_Flags; };
struct ExecBase { unsigned IntFlags; };
#define PrivExecBase(base) (base)
#define IntETask(et) ((struct IntETask *)(et))
#define EXEC_SPINLOCK_INIT(lock) (*(lock) = 0)
static struct IntETask storage;
static uint32_t mask;
static unsigned alloc_fail, mask_fail, frees, sets, checks;
static size_t free_size;
static void *AllocMem(size_t bytes, unsigned flags)
{ (void)flags; return !alloc_fail && bytes == sizeof(storage) ? &storage : NULL; }
static void FreeMem(void *ptr, size_t bytes)
{ if (ptr != &storage) abort(); frees++; free_size = bytes; }
static int KrnGetCPUNumber(void) { return 1; }
static void *KrnAllocCPUMask(void) { return mask_fail ? NULL : &mask; }
static void KrnGetCPUMask(int cpu, void *ptr)
{ if (cpu != 1 || ptr != &mask) abort(); sets++; mask = 1u << cpu; }
#define CHECK(x) do { checks++; if (!(x)) { \
 fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
'''
main = r'''
int main(void)
{
 struct ExecBase base = { EXECF_CPUAffinity };
 struct Task task = { { NULL }, 1u };
 mask_fail = 1;
 CHECK(!Exec_InitETask(&task, NULL, &base));
 CHECK(task.tc_UnionETask.tc_ETask == NULL);
 CHECK(task.tc_Flags == 1u);
 CHECK(frees == 1 && free_size == sizeof(storage));
 CHECK(sets == 0);
 mask_fail = 0;
 CHECK(Exec_InitETask(&task, NULL, &base));
 CHECK(task.tc_UnionETask.tc_ETask == &storage.pub);
 CHECK(task.tc_Flags == (1u | TF_ETASK));
 CHECK(mask == 2u && sets == 1);
 CHECK(frees == 1);
 task.tc_UnionETask.tc_ETask = NULL; task.tc_Flags = 1u;
 alloc_fail = 1;
 CHECK(!Exec_InitETask(&task, NULL, &base));
 CHECK(task.tc_UnionETask.tc_ETask == NULL && task.tc_Flags == 1u);
 CHECK(frees == 1 && sets == 1);
 printf("PASS: %u ETask affinity-construction checks\n", checks);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-etask-affinity-") as tmp:
    src = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    src.write_text(fixture + body + main)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(src),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
