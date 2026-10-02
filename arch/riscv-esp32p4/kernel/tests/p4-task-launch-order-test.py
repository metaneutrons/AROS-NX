#!/usr/bin/env python3
"""Exercise the actual TaskLaunch hook/publication order in both variants."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
source = (repo / "rom/exec/newaddtask.c").read_text()
start = source.index("static void TaskLaunch(")
end = source.index("\n/****", start)
body = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
typedef void *APTR;
typedef intptr_t IPTR;
struct Node { int ln_Pri; };
struct ETask { void *et_RegFrame; int iet_CpuAffinity; };
struct Task {
    struct Node tc_Node;
    int tc_State;
    struct { struct ETask *tc_ETask; } tc_UnionETask;
};
struct Hook { int unused; };
struct Base { int IntFlags; int TaskReady; struct Task *current; } base;
#define SysBase (&base)
#define PrivExecBase(b) (b)
#define GET_THIS_TASK (base.current)
#define IntETask(e) (e)
#define GetETask(t) ((t)->tc_UnionETask.tc_ETask)
#define EXECF_CPUAffinity 1
#define TS_READY 1
#define TS_RUN 2
#define TS_INVALID 3
#define TS_REMOVED 4
#define DADDTASK(...) ((void)0)
#define bug(...) ((void)0)
static int hooks, publishes, reschedules, remote_pokes, disabled, checks;
static int expect_hook;
static void hook(struct Hook *h, struct Task *t, int arg) {
    (void)h; (void)t; (void)arg;
#ifdef __AROSEXEC_SMP__
    assert(publishes == 0);
    assert(disabled == 0);
#else
    assert(disabled == 1);
#endif
    assert(hooks == 0);
    hooks++;
}
#define CALLHOOKPKT(h,t,a) hook(h,t,a)
static int KrnGetCPUNumber(void) { return 0; }
static int KrnCPUInMask(int cpu, int mask) { return (mask & (1 << cpu)) != 0; }
static void Disable(void) { disabled++; }
static void Enable(void) { assert(disabled == 1); disabled--; }
static void Enqueue(int *q, struct Node *n) { (void)q; (void)n; publishes++; }
static void krnSysCallReschedTask(struct Task *t, int state) {
    if (state == TS_READY) {
        /* Simulate another hart observing the task at publication. */
        assert(hooks == expect_hook);
        publishes++;
    }
    t->tc_State = state;
}
static void Reschedule(void) { assert(hooks == expect_hook); reschedules++; }
static void KrnScheduleCPU(int mask) {
    assert(mask == 2); assert(hooks == expect_hook); remote_pokes++;
}
static void KrnDeleteContext(void *p) { (void)p; assert(0); }
static void CleanupETask(struct Task *t) { (void)t; assert(0); }
@BODY@
int main(void) {
    for (int affinity = 0; affinity < 3; affinity++)
    for (int priority = -1; priority <= 1; priority++)
    for (int with_hook = 0; with_hook <= 1; with_hook++) {
        struct ETask et = { 0, affinity == 2 ? 2 : 1 };
        struct Task parent = { { 0 }, TS_RUN, { &et } };
        struct Task child = { { priority }, TS_INVALID, { &et } };
        struct Hook h = { 0 };
        hooks = publishes = reschedules = remote_pokes = disabled = 0;
        expect_hook = with_hook;
        base.IntFlags = affinity ? EXECF_CPUAffinity : 0;
        base.current = &parent;
        TaskLaunch(&parent, &child, with_hook ? &h : NULL);
        assert(hooks == with_hook); checks++;
        assert(publishes == 1); checks++;
        assert(disabled == 0); checks++;
#ifdef __AROSEXEC_SMP__
        assert(remote_pokes == (affinity == 2)); checks++;
        assert(reschedules == (affinity != 2 && priority > 0)); checks++;
#else
        assert(remote_pokes == 0); checks++;
        assert(reschedules == (priority > 0)); checks++;
#endif
    }
    printf("p4-task-launch-order: %d checks passed\n", checks);
}
'''.replace("@BODY@", body)
with tempfile.TemporaryDirectory(prefix="p4-task-launch-") as temporary:
    path = Path(temporary)
    c_file = path / "test.c"
    c_file.write_text(fixture)
    for variant in ("single", "smp"):
        binary = path / variant
        command = ["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer"]
        if variant == "smp":
            command += ["-D__AROSEXEC_SMP__=1"]
        subprocess.run(command + [str(c_file), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
