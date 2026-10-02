#!/usr/bin/env python3
"""Exercise actual Exec task-record publication hooks under ASan/UBSan."""
from hashlib import sha256
from pathlib import Path
import re
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
NEWADD_PATH = REPO / "rom/exec/newaddtask.c"
INIT_PATH = REPO / "rom/exec/exec_init.c"
newadd = NEWADD_PATH.read_text()
init = INIT_PATH.read_text()


def pp_block(source: str, macro: str) -> str:
    marker = f"#if defined({macro})"
    start = source.index(marker)
    lines = source[start:].splitlines(keepends=True)
    depth = 0
    selected = []
    for line in lines:
        directive = line.lstrip()
        if directive.startswith(("#if ", "#ifdef ", "#ifndef ")):
            depth += 1
        elif directive.startswith("#endif"):
            depth -= 1
        selected.append(line)
        if depth == 0:
            return "".join(selected)
    raise AssertionError(f"unterminated preprocessor block for {macro}")


def one_line(source: str, prefix: str) -> str:
    for line in source.splitlines():
        if line.lstrip().startswith(prefix):
            return line.strip()
    raise AssertionError(f"missing source line: {prefix}")


launch_start = newadd.index("static void TaskLaunch(")
launch_end = newadd.index("\n/****", launch_start)
task_launch = newadd[launch_start:launch_end]
create_block = pp_block(newadd, "EXEC_PLATFORM_TASK_RECORD_CREATE")
release_block = pp_block(newadd, "EXEC_PLATFORM_TASK_RECORD_RELEASE")
boot_block = pp_block(init, "EXEC_PLATFORM_BOOT_TASK_REGISTER")
run_state_line = one_line(init, "t->tc_State")
set_current_line = one_line(init, "SET_THIS_TASK(t);")
final_log_line = one_line(newadd, 'DADDTASK("NewAddTask: task 0x%p launched"')


# These checks tie the fixture's sequence to the production call sites; the C
# fixture below compiles and executes the extracted implementation blocks.
prepare_ok = newadd.index("if (!PrepareContext(task, initialPC, finalPC, tagList, SysBase))")
create_at = newadd.index(create_block)
launch_call = newadd.index("TaskLaunch(parent, task, plHook);")
release_at = newadd.index(release_block)
final_log_at = newadd.index(final_log_line)
assert prepare_ok < create_at < launch_call < final_log_at < release_at
release_end = release_at + len(release_block)
return_at = newadd.index("return task;", release_end)
assert not newadd[release_end:return_at].strip()
assert task_launch.index("CALLHOOKPKT(plHook, task, 0);") < task_launch.index(
    "EXEC_PLATFORM_NEW_TASK_READY(task)"
)
p4_ready = task_launch.index("if (EXEC_PLATFORM_NEW_TASK_READY(task) < 1)")
p4_end = task_launch.index("#else", p4_ready)
p4_tail = task_launch[p4_ready:p4_end]
assert "EXEC_PLATFORM_TASK_FATAL();" in p4_tail and "return;" in p4_tail
assert "task->" not in p4_tail and "parent->" not in p4_tail
assert "GET_THIS_TASK" not in p4_tail and "KrnGetCPUNumber" not in p4_tail
assert re.search(
    r"if \(EXEC_PLATFORM_NEW_TASK_READY\(task\) < 1\)\s*\{\s*"
    r"EXEC_PLATFORM_TASK_FATAL\(\);\s*return;\s*\}\s*return;\s*$",
    p4_tail,
)
assert task_launch.index("#if defined(EXEC_PLATFORM_NEW_TASK_READY)") < p4_ready
assert task_launch.index("int cpunum = KrnGetCPUNumber();") > task_launch.index("#else", p4_ready)
boot_at = init.index(boot_block)
run_at = init.index(run_state_line, boot_at)
set_at = init.index(set_current_line, run_at)
assert boot_at < run_at < set_at


fixture = r''' 
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *APTR;
typedef intptr_t IPTR;
typedef unsigned int ULONG;
typedef struct Task Task;
struct Node { int ln_Pri; char *ln_Name; int ln_Type; };
struct ETask { void *et_RegFrame; int iet_CpuAffinity; };
struct Task {
    struct Node tc_Node;
    int tc_State;
    unsigned int tc_Flags;
    int tc_Switch, tc_Launch;
    union { struct ETask *tc_ETask; } tc_UnionETask;
};
struct Hook { int unused; };
struct MemList { struct Node ml_Node; };
struct ExecBase { int IntFlags; int TaskReady; };
static struct ExecBase base;
#define SysBase (&base)
#define PrivExecBase(basearg) (basearg)
#define GET_THIS_TASK (current_task)
#define EXECF_CPUAffinity 1
#define TS_READY 1
#define TS_RUN 2
#define TS_INVALID 3
#define TS_REMOVED 4
#define TS_ADDED 5
#define NT_TASK 1
#define TF_SWITCH 0x10
#define TF_LAUNCH 0x20
#define IntETask(et) ((struct ETask *)(et))
#define GetETask(task) ((task)->tc_UnionETask.tc_ETask)

static Task *current_task;
static unsigned int checks, hook_calls, queue_publications;
static unsigned int reschedules, remote_pokes, disable_depth;
static unsigned int expected_hooks;
static int cpu_number;
static unsigned int creator_pins, record_live, final_launch_logs;
static unsigned int delete_context_calls, cleanup_calls, remove_calls, free_entry_calls;
static unsigned int delete_sequence, cleanup_sequence, sequence_number;
static void *expected_frame;
#ifdef TEST_P4_MACROS
static int fatal_code;
static jmp_buf fatal_env;
static unsigned int task_retired;
static unsigned int ready_calls, create_calls, release_calls;
static unsigned int ready_result, fail_create, retire_in_ready;
static unsigned int boot_calls, running_owner, boot_record_live;
static unsigned int set_current_calls;
static unsigned int boot_result = 1;
#endif
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        abort(); \
    } \
} while (0)

static void log_line(const char *fmt, ...)
{
    if (strcmp(fmt, "NewAddTask: task 0x%p launched") == 0) {
        CHECK(creator_pins == 1);
        CHECK(record_live == 1);
        CHECK(hook_calls == expected_hooks);
        final_launch_logs++;
    }
}
#define DADDTASK(...) log_line(__VA_ARGS__)
#define bug(...) ((void)0)

static void invoke_hook(struct Hook *hook, Task *task, int arg)
{
    (void)hook; (void)arg; (void)task;
#ifdef __AROSEXEC_SMP__
    CHECK(queue_publications == 0);
#else
    CHECK(disable_depth == 1);
#endif
#ifdef TEST_P4_MACROS
    CHECK(creator_pins == 1);
    CHECK(record_live == 1);
#endif
    CHECK(hook_calls == 0);
    hook_calls++;
}
#define CALLHOOKPKT(hook, task, arg) invoke_hook((hook), (task), (arg))

static int KrnGetCPUNumber(void) { return cpu_number; }
static int KrnCPUInMask(int cpu, int mask) { return (mask & (1 << cpu)) != 0; }
static void Disable(void) { disable_depth++; }
static void Enable(void) { CHECK(disable_depth == 1); disable_depth--; }
static void Enqueue(void *queue, struct Node *node)
{
    (void)queue; (void)node; queue_publications++;
}
static void krnSysCallReschedTask(Task *task, int state)
{
    if (state == TS_READY) {
        CHECK(hook_calls == expected_hooks);
        queue_publications++;
    }
    task->tc_State = state;
}
static void Reschedule(void) { reschedules++; }
static void KrnScheduleCPU(int mask) { (void)mask; remote_pokes++; }
static void KrnDeleteContext(void *frame)
{
    CHECK(frame == expected_frame);
    delete_context_calls++;
    delete_sequence = ++sequence_number;
}
static void CleanupETask(Task *task)
{
    CHECK(GetETask(task)->et_RegFrame == NULL);
    cleanup_calls++;
    cleanup_sequence = ++sequence_number;
}
static void Remove(struct Node *node) { (void)node; remove_calls++; }
static void FreeEntry(struct MemList *entry) { (void)entry; free_entry_calls++; }

#ifdef TEST_P4_MACROS
static int p4_ready(Task *task)
{
    CHECK(task != NULL);
    CHECK(creator_pins == 1);
    CHECK(record_live == 1);
    CHECK(hook_calls == expected_hooks);
    ready_calls++;
    if (retire_in_ready) {
        task_retired = 1;
        /* The remote exit cannot reclaim Task while NewAddTask owns its pin. */
        CHECK(creator_pins != 0);
        CHECK(record_live != 0);
    }
    return (int)ready_result;
}
static void p4_create(Task *task)
{
    CHECK(task != NULL);
    create_calls++;
    if (!fail_create) {
        CHECK(record_live == 0);
        record_live = 1;
        creator_pins = 1;
    }
}
static int p4_create_result(Task *task)
{
    p4_create(task);
    return !fail_create;
}
static void p4_release(Task *task)
{
    /* Registry lookup is identity-only; this operation does not dereference Task. */
    (void)task;
    CHECK(record_live == 1);
    CHECK(creator_pins == 1);
    release_calls++;
    creator_pins--;
    if (task_retired) {
        record_live = 0;
        free(task);
    }
}
static void p4_fatal(void) __attribute__((noreturn));
static void p4_fatal(void)
{
    fatal_code++;
    longjmp(fatal_env, 1);
    abort();
}
static int p4_boot_register(Task *task)
{
    boot_calls++;
    CHECK(task->tc_State == TS_INVALID);
    if (!boot_result)
        return 0;
    CHECK(current_task == NULL);
    CHECK(running_owner == 0);
    boot_record_live = 1;
    running_owner = 1;
    creator_pins = 0; /* helper drops its creation pin after RUN/current publish */
    current_task = task;
    task->tc_State = TS_RUN; /* RunningLocked publishes the state. */
    return 1;
}
static void set_this_task(Task *task)
{
    CHECK(current_task == task);
    CHECK(running_owner == 1);
    CHECK(task->tc_State == TS_RUN);
    current_task = task; /* P4 SET_THIS_TASK is idempotent after registration. */
    set_current_calls++;
}
#define EXEC_PLATFORM_NEW_TASK_READY(task) p4_ready((task))
#define EXEC_PLATFORM_TASK_RECORD_CREATE(task) p4_create_result((task))
#define EXEC_PLATFORM_TASK_RECORD_RELEASE(task) p4_release((task))
#define EXEC_PLATFORM_BOOT_TASK_REGISTER(task) p4_boot_register((task))
#define EXEC_PLATFORM_TASK_FATAL() p4_fatal()
#define SET_THIS_TASK(task) set_this_task((task))
#endif

@TASK_LAUNCH@

static void reset_launch_state(void)
{
    hook_calls = queue_publications = reschedules = remote_pokes = 0;
    disable_depth = 0;
    current_task = NULL;
    base.IntFlags = 0;
    cpu_number = 0;
}

static void test_legacy_launch(void)
{
    for (int priority = -1; priority <= 1; ++priority)
    for (int with_hook = 0; with_hook <= 1; ++with_hook) {
        Task parent = { .tc_Node = { .ln_Pri = 0 }, .tc_State = TS_RUN };
        Task child = { .tc_Node = { .ln_Pri = priority }, .tc_State = TS_INVALID };
        struct Hook hook = { 0 };
        reset_launch_state();
        expected_hooks = with_hook;
        current_task = &parent;
        TaskLaunch(&parent, &child, with_hook ? &hook : NULL);
        CHECK(hook_calls == (unsigned int)with_hook);
        CHECK(queue_publications == 1);
        CHECK(disable_depth == 0);
#ifdef __AROSEXEC_SMP__
        CHECK(reschedules == (unsigned int)(priority > 0));
#else
        CHECK(reschedules == (unsigned int)(priority > 0));
#endif
        CHECK(remote_pokes == 0);
    }
}

#ifdef TEST_P4_MACROS
static Task *run_add_success(Task *parent, Task *task, struct Hook *hook)
{
    struct MemList *mlExtra = NULL;
    char *taskName = task->tc_Node.ln_Name;
    @CREATE_BLOCK@
    task->tc_Flags |= TF_SWITCH;
    TaskLaunch(parent, task, hook);
    @FINAL_LOG@
    @RELEASE_BLOCK@
    return task;
}

static Task *run_create_failure(Task *task, struct MemList *mlExtra, char *taskName)
{
    @CREATE_BLOCK@
    (void)mlExtra; (void)taskName;
    return task;
}

static int run_boot_registration(Task *task)
{
    Task *t = task;
    @BOOT_BLOCK@
    @RUN_STATE@
    @SET_CURRENT@
    return 1;
execfatal:
    return 0;
}

static void reset_p4_state(void)
{
    creator_pins = record_live = task_retired = 0;
    final_launch_logs = ready_calls = create_calls = release_calls = 0;
    ready_result = 1; fail_create = retire_in_ready = 0;
    delete_context_calls = cleanup_calls = remove_calls = free_entry_calls = 0;
    delete_sequence = cleanup_sequence = sequence_number = 0;
    boot_calls = running_owner = boot_record_live = set_current_calls = 0;
    fatal_code = 0;
    reset_launch_state();
}

static void test_p4_create_launch_retire(void)
{
    Task *task = calloc(1, sizeof(*task));
    Task *parent = calloc(1, sizeof(*parent));
    struct Hook hook = { 0 };
    uintptr_t identity = (uintptr_t)task;
    CHECK(task && parent);
    reset_p4_state();
    task->tc_State = TS_INVALID;
    parent->tc_State = TS_RUN;
    current_task = parent;
    expected_hooks = 1;
    retire_in_ready = 1;
    Task *returned = run_add_success(parent, task, &hook);
    /* Compare the raw identity only: release may have reclaimed Task storage. */
    CHECK((uintptr_t)returned == identity);
    CHECK(create_calls == 1 && ready_calls == 1 && release_calls == 1);
    CHECK(final_launch_logs == 1 && hook_calls == 1);
    CHECK(task_retired == 1 && record_live == 0 && creator_pins == 0);
    free(parent);
}

static void test_p4_ready_failure_fatal(void)
{
    Task *task = calloc(1, sizeof(*task));
    Task parent = { .tc_State = TS_RUN };
    CHECK(task != NULL);
    reset_p4_state();
    task->tc_State = TS_INVALID;
    current_task = &parent;
    expected_hooks = 0;
    ready_result = 0;
    if (setjmp(fatal_env) == 0) {
        (void)run_add_success(&parent, task, NULL);
        CHECK(0 && "fatal hook returned");
    }
    CHECK(fatal_code == 1);
    CHECK(release_calls == 0 && creator_pins == 1);
    CHECK(final_launch_logs == 0);
    /* Fatal is non-returning in production; discard only the host fixture. */
    creator_pins = 0;
    record_live = 0;
    free(task);
}

static void test_p4_create_failure_cleanup(void)
{
    Task task = { .tc_Node = { .ln_Name = "original" } };
    struct ETask et = { .et_RegFrame = &expected_frame_marker };
    struct MemList extra = { 0 };
    char *original = task.tc_Node.ln_Name;
    task.tc_UnionETask.tc_ETask = &et;
    expected_frame = et.et_RegFrame;
    reset_p4_state();
    fail_create = 1;
    CHECK(run_create_failure(&task, &extra, original) == NULL);
    CHECK(create_calls == 1 && ready_calls == 0 && release_calls == 0);
    CHECK(delete_context_calls == 1 && cleanup_calls == 1);
    CHECK(delete_sequence < cleanup_sequence);
    CHECK(et.et_RegFrame == NULL);
    CHECK(remove_calls == 1 && free_entry_calls == 1);
    CHECK(task.tc_Node.ln_Name == original);
    CHECK(record_live == 0 && creator_pins == 0);
}

static void test_p4_boot_register_before_state_and_current(void)
{
    Task task = { .tc_State = TS_INVALID };
    reset_p4_state();
    boot_result = 1;
    CHECK(run_boot_registration(&task) == 1);
    CHECK(boot_calls == 1 && boot_record_live == 1 && running_owner == 1);
    CHECK(current_task == &task && task.tc_State == TS_RUN);
    CHECK(set_current_calls == 1);

    reset_p4_state();
    task.tc_State = TS_INVALID;
    boot_result = 0;
    CHECK(run_boot_registration(&task) == 0);
    CHECK(boot_calls == 1 && boot_record_live == 0 && running_owner == 0);
    CHECK(current_task == NULL && set_current_calls == 0);
    CHECK(task.tc_State == TS_INVALID);
    boot_result = 1;
}
#endif

int main(void)
{
#ifdef TEST_P4_MACROS
    test_p4_create_launch_retire();
    test_p4_ready_failure_fatal();
    test_p4_create_failure_cleanup();
    test_p4_boot_register_before_state_and_current();
#else
    test_legacy_launch();
#endif
    printf("p4-task-record-publication: %u checks passed\n", checks);
    return 0;
}
'''


fixture = fixture.replace("@TASK_LAUNCH@", task_launch)
fixture = fixture.replace("@CREATE_BLOCK@", create_block)
fixture = fixture.replace("@RELEASE_BLOCK@", release_block)
fixture = fixture.replace("@BOOT_BLOCK@", boot_block)
fixture = fixture.replace("@RUN_STATE@", run_state_line)
fixture = fixture.replace("@SET_CURRENT@", set_current_line)
fixture = fixture.replace("@FINAL_LOG@", final_log_line)
fixture = fixture.replace("expected_frame_marker", "expected_frame")


variants = {
    "single": [],
    "smp": ["-D__AROSEXEC_SMP__=1"],
    "smp-platform": ["-D__AROSEXEC_SMP__=1", "-D__AROSPLATFORM_SMP__=1"],
    "p4-smp-hooks": [
        "-D__AROSEXEC_SMP__=1",
        "-D__AROSPLATFORM_SMP__=1",
        "-DTEST_P4_MACROS=1",
    ],
}
commands = []
outputs = []
with tempfile.TemporaryDirectory(prefix="p4-task-record-publication-") as temporary:
    work = Path(temporary)
    c_file = work / "test.c"
    c_file.write_text(fixture)
    for name, defines in variants.items():
        binary = work / name
        command = [
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", *defines, str(c_file), "-o", str(binary),
        ]
        commands.append(
            " ".join(command[:-3])
            + f" <generated-fixture> -o <temporary-output>/{name}"
        )
        subprocess.run(command, check=True)
        completed = subprocess.run([str(binary)], check=True, text=True,
                                   stdout=subprocess.PIPE)
        outputs.append(f"[{name}] {completed.stdout.strip()}")

evidence = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/"
    "smp-task-record-publication-2026-10-01"
)
evidence.mkdir(parents=True, exist_ok=True)
(evidence / "commands.txt").write_text(
    "Run: python3 arch/riscv-esp32p4/kernel/tests/p4-task-record-publication-test.py\n"
    "Compiler: clang (host ASan + UBSan)\n\n"
    + "\n".join(commands)
    + "\n"
)
(evidence / "output.log").write_text("\n".join(outputs) + "\n")
hash_lines = []
for path in (NEWADD_PATH, INIT_PATH, Path(__file__).resolve()):
    hash_lines.append(f"{sha256(path.read_bytes()).hexdigest()}  {path}")
(evidence / "source.sha256").write_text("\n".join(hash_lines) + "\n")
print("\n".join(outputs))
print(f"Evidence: {evidence}")
