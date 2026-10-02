#!/usr/bin/env python3
"""Compile the actual InternalAttemptSemaphore body against host mocks."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
SOURCE_PATH = REPO / "rom/exec/semaphores.c"
SIGNATURE = "ULONG InternalAttemptSemaphore("
COMPILE_TIMEOUT_SECONDS = 30
RUN_TIMEOUT_SECONDS = 10


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"function not found: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        raise RuntimeError("function opening brace not found")

    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise RuntimeError("function closing brace not found")


HARNESS = r'''#define _GNU_SOURCE 1
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint32_t ULONG;
typedef int BOOL;
#define TRUE UINT32_C(1)
#define FALSE UINT32_C(0)

struct Task { uint32_t opaque; };
struct TraceLocation { uint32_t opaque; };
struct ExecBase { uint32_t opaque; };
struct SignalSemaphore
{
    int32_t ss_QueueCount;
    struct Task *ss_Owner;
    uint32_t ss_NestCount;
    pthread_mutex_t mock_gate;
};

static _Thread_local struct Task *mock_current_task;
static _Thread_local unsigned int mock_forbid_depth;
static _Thread_local unsigned int mock_gate_depth;
static _Thread_local unsigned int event_log[8];
static _Thread_local size_t event_count;
static _Thread_local int event_enabled;
static _Thread_local int expected_state_enabled;
static _Thread_local struct SignalSemaphore *expected_sem;
static _Thread_local int32_t expected_before_count;
static _Thread_local struct Task *expected_before_owner;
static _Thread_local uint32_t expected_before_nest;
static _Thread_local int32_t expected_after_count;
static _Thread_local struct Task *expected_after_owner;
static _Thread_local uint32_t expected_after_nest;

static atomic_int check_semaphore_result = ATOMIC_VAR_INIT(1);
static atomic_int mock_error = ATOMIC_VAR_INIT(0);
static atomic_uint check_semaphore_calls = ATOMIC_VAR_INIT(0u);
static atomic_uint forbid_calls = ATOMIC_VAR_INIT(0u);
static atomic_uint permit_calls = ATOMIC_VAR_INIT(0u);
static atomic_uint gate_lock_calls = ATOMIC_VAR_INIT(0u);
static atomic_uint gate_unlock_calls = ATOMIC_VAR_INIT(0u);
static unsigned long checks;

enum
{
    EV_CHECK = 1u,
    EV_FORBID,
    EV_GATE_LOCK,
    EV_GATE_UNLOCK,
    EV_PERMIT
};

static void record_event(unsigned int event)
{
    if (!event_enabled)
        return;
    if (event_count >= sizeof(event_log) / sizeof(event_log[0]))
    {
        atomic_store_explicit(&mock_error, 1, memory_order_relaxed);
        return;
    }
    event_log[event_count++] = event;
}

static int state_is_before(const struct SignalSemaphore *sem)
{
    return sem == expected_sem &&
           sem->ss_QueueCount == expected_before_count &&
           sem->ss_Owner == expected_before_owner &&
           sem->ss_NestCount == expected_before_nest;
}

static int state_is_after(const struct SignalSemaphore *sem)
{
    return sem == expected_sem &&
           sem->ss_QueueCount == expected_after_count &&
           sem->ss_Owner == expected_after_owner &&
           sem->ss_NestCount == expected_after_nest;
}

static void mock_forbid(void)
{
    record_event(EV_FORBID);
    if (mock_forbid_depth != 0u ||
        (expected_state_enabled && !state_is_before(expected_sem)))
        atomic_store_explicit(&mock_error, 1, memory_order_relaxed);
    ++mock_forbid_depth;
    (void)atomic_fetch_add_explicit(&forbid_calls, 1u, memory_order_relaxed);
}

static void mock_permit(void)
{
    record_event(EV_PERMIT);
    if (mock_forbid_depth != 1u || mock_gate_depth != 0u ||
        (expected_state_enabled && !state_is_after(expected_sem)))
        atomic_store_explicit(&mock_error, 1, memory_order_relaxed);
    if (mock_forbid_depth != 0u)
        --mock_forbid_depth;
    (void)atomic_fetch_add_explicit(&permit_calls, 1u, memory_order_relaxed);
}

#ifdef TEST_WITH_PLATFORM_GATE
static void mock_semaphore_lock(struct SignalSemaphore *sem)
{
    if (pthread_mutex_lock(&sem->mock_gate) != 0)
        abort();
    record_event(EV_GATE_LOCK);
    if (mock_forbid_depth != 1u || mock_gate_depth != 0u ||
        (expected_state_enabled && !state_is_before(sem)))
        atomic_store_explicit(&mock_error, 1, memory_order_relaxed);
    ++mock_gate_depth;
    (void)atomic_fetch_add_explicit(&gate_lock_calls, 1u, memory_order_relaxed);
}

static void mock_semaphore_unlock(struct SignalSemaphore *sem)
{
    record_event(EV_GATE_UNLOCK);
    if (mock_forbid_depth != 1u || mock_gate_depth != 1u ||
        (expected_state_enabled && !state_is_after(sem)))
        atomic_store_explicit(&mock_error, 1, memory_order_relaxed);
    if (mock_gate_depth != 0u)
        --mock_gate_depth;
    (void)atomic_fetch_add_explicit(&gate_unlock_calls, 1u, memory_order_relaxed);
    if (pthread_mutex_unlock(&sem->mock_gate) != 0)
        abort();
}
#endif

static struct Task *mock_get_current_task(void)
{
    return mock_current_task;
}

#define GET_THIS_TASK mock_get_current_task()
#define Forbid() mock_forbid()
#define Permit() mock_permit()

#ifdef TEST_WITH_PLATFORM_GATE
#define EXEC_PLATFORM_SEMAPHORE_LOCK(sem) mock_semaphore_lock((sem))
#define EXEC_PLATFORM_SEMAPHORE_UNLOCK(sem) mock_semaphore_unlock((sem))
#endif

BOOL CheckSemaphore(struct SignalSemaphore *, struct TraceLocation *,
                    struct ExecBase *);
ULONG InternalAttemptSemaphore(struct SignalSemaphore *, struct Task *,
                               struct TraceLocation *, struct ExecBase *);

BOOL CheckSemaphore(struct SignalSemaphore *sem,
                    struct TraceLocation *caller,
                    struct ExecBase *sys_base)
{
    (void)sem;
    (void)caller;
    (void)sys_base;
    record_event(EV_CHECK);
    (void)atomic_fetch_add_explicit(&check_semaphore_calls, 1u,
                                    memory_order_relaxed);
    return atomic_load_explicit(&check_semaphore_result, memory_order_relaxed)
               ? TRUE
               : FALSE;
}

/* ACTUAL_PRODUCTION_FUNCTION */

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        (void)fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static struct TraceLocation trace_location;
static struct ExecBase exec_base;

static int init_semaphore(struct SignalSemaphore *sem)
{
    (void)memset(sem, 0, sizeof(*sem));
    return pthread_mutex_init(&sem->mock_gate, NULL) == 0;
}

static void destroy_semaphore(struct SignalSemaphore *sem)
{
    if (pthread_mutex_destroy(&sem->mock_gate) != 0)
        abort();
}

static int run_case(const char *name,
                    struct SignalSemaphore *sem,
                    struct Task *current,
                    struct Task *owner,
                    int check_allowed,
                    ULONG expected_result,
                    int32_t before_count,
                    struct Task *before_owner,
                    uint32_t before_nest,
                    int32_t after_count,
                    struct Task *after_owner,
                    uint32_t after_nest)
{
    unsigned int before_forbid = atomic_load_explicit(&forbid_calls,
                                                      memory_order_relaxed);
    unsigned int before_permit = atomic_load_explicit(&permit_calls,
                                                      memory_order_relaxed);
    unsigned int before_lock = atomic_load_explicit(&gate_lock_calls,
                                                    memory_order_relaxed);
    unsigned int before_unlock = atomic_load_explicit(&gate_unlock_calls,
                                                      memory_order_relaxed);
    ULONG result;
    unsigned int expected_events[5];
    size_t expected_event_count = 0u;

    (void)name;
    sem->ss_QueueCount = before_count;
    sem->ss_Owner = before_owner;
    sem->ss_NestCount = before_nest;
    mock_current_task = current;
    atomic_store_explicit(&check_semaphore_result, check_allowed,
                          memory_order_relaxed);
    atomic_store_explicit(&mock_error, 0, memory_order_relaxed);
    expected_sem = sem;
    expected_before_count = before_count;
    expected_before_owner = before_owner;
    expected_before_nest = before_nest;
    expected_after_count = after_count;
    expected_after_owner = after_owner;
    expected_after_nest = after_nest;
    expected_state_enabled = 1;
    event_count = 0u;
    event_enabled = 1;

    result = InternalAttemptSemaphore(sem, owner, &trace_location, &exec_base);

    event_enabled = 0;
    expected_state_enabled = 0;
    mock_current_task = NULL;
    CHECK(result == expected_result);
    CHECK(sem->ss_QueueCount == after_count);
    CHECK(sem->ss_Owner == after_owner);
    CHECK(sem->ss_NestCount == after_nest);
    CHECK(mock_forbid_depth == 0u);
    CHECK(mock_gate_depth == 0u);
    CHECK(atomic_load_explicit(&mock_error, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&forbid_calls, memory_order_relaxed) -
              before_forbid == (check_allowed ? 1u : 0u));
    CHECK(atomic_load_explicit(&permit_calls, memory_order_relaxed) -
              before_permit == (check_allowed ? 1u : 0u));

    expected_events[expected_event_count++] = EV_CHECK;
    if (check_allowed)
    {
        expected_events[expected_event_count++] = EV_FORBID;
#ifdef TEST_WITH_PLATFORM_GATE
        expected_events[expected_event_count++] = EV_GATE_LOCK;
        expected_events[expected_event_count++] = EV_GATE_UNLOCK;
#endif
        expected_events[expected_event_count++] = EV_PERMIT;
    }
    CHECK(event_count == expected_event_count);
    CHECK(memcmp(event_log, expected_events,
                 expected_event_count * sizeof(expected_events[0])) == 0);
#ifdef TEST_WITH_PLATFORM_GATE
    CHECK(atomic_load_explicit(&gate_lock_calls, memory_order_relaxed) -
              before_lock == (check_allowed ? 1u : 0u));
    CHECK(atomic_load_explicit(&gate_unlock_calls, memory_order_relaxed) -
              before_unlock == (check_allowed ? 1u : 0u));
#else
    CHECK(atomic_load_explicit(&gate_lock_calls, memory_order_relaxed) ==
          before_lock);
    CHECK(atomic_load_explicit(&gate_unlock_calls, memory_order_relaxed) ==
          before_unlock);
#endif
    return 1;
}

static int test_state_transitions(void)
{
    struct SignalSemaphore sem;
    struct Task task_a = { UINT32_C(1) };
    struct Task task_b = { UINT32_C(2) };

    CHECK(init_semaphore(&sem));
    CHECK(run_case("free exclusive", &sem, &task_a, &task_a, 1, TRUE,
                   -1, NULL, 0u, 0, &task_a, 1u));
    CHECK(run_case("free shared", &sem, &task_b, NULL, 1, TRUE,
                   -1, NULL, 0u, 0, NULL, 1u));
    CHECK(run_case("recursive exclusive", &sem, &task_a, &task_a, 1, TRUE,
                   0, &task_a, 4u, 1, &task_a, 5u));
    CHECK(run_case("recursive shared", &sem, &task_b, NULL, 1, TRUE,
                   0, NULL, 7u, 1, NULL, 8u));
    CHECK(run_case("failed attempt rollback", &sem, &task_a, &task_a, 1,
                   FALSE, 7, &task_b, 3u, 7, &task_b, 3u));
    CHECK(run_case("rejected semaphore unchanged", &sem, &task_a, &task_a,
                   0, FALSE, 9, &task_b, 2u, 9, &task_b, 2u));
    destroy_semaphore(&sem);
    return 1;
}

static int test_task_pointer_is_opaque(void)
{
    struct SignalSemaphore sem;
    long page_size_long = sysconf(_SC_PAGESIZE);
    size_t page_size;
    void *page;
    struct Task *poison_task;
    struct Task *other_poison_task;
    int ok;

    CHECK(page_size_long > 0);
    page_size = (size_t)page_size_long;
    page = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
    CHECK(page != MAP_FAILED);
    poison_task = (struct Task *)page;
    other_poison_task = (struct Task *)((unsigned char *)page + 1u);
    CHECK(init_semaphore(&sem));

    ok = run_case("opaque Task pointer", &sem, poison_task, poison_task, 1,
                  TRUE, -1, NULL, 0u, 0, poison_task, 1u);
    if (ok)
        ok = run_case("opaque Task comparison", &sem, poison_task,
                      poison_task, 1, FALSE, 0, other_poison_task, 2u, 0,
                      other_poison_task, 2u);
    destroy_semaphore(&sem);
    CHECK(munmap(page, page_size) == 0);
    CHECK(ok);
    return 1;
}

#ifdef TEST_WITH_PLATFORM_GATE
struct worker_args
{
    struct SignalSemaphore *sem;
    struct Task task;
    atomic_int *start;
    atomic_uint *ready;
    ULONG result;
};

static void *attempt_worker(void *arg)
{
    struct worker_args *worker = (struct worker_args *)arg;
    mock_current_task = &worker->task;
    (void)atomic_fetch_add_explicit(worker->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(worker->start, memory_order_acquire))
        (void)sched_yield();
    event_enabled = 0;
    expected_state_enabled = 0;
    worker->result = InternalAttemptSemaphore(worker->sem, &worker->task,
                                              &trace_location, &exec_base);
    mock_current_task = NULL;
    return NULL;
}
#endif

static int test_pthread_contention(void)
{
#ifdef TEST_WITH_PLATFORM_GATE
    enum { THREAD_COUNT = 12 };
    struct SignalSemaphore sem;
    struct worker_args workers[THREAD_COUNT];
    pthread_t threads[THREAD_COUNT];
    atomic_int start = ATOMIC_VAR_INIT(0);
    atomic_uint ready = ATOMIC_VAR_INIT(0u);
    unsigned int created = 0u;
    unsigned int winner_count = 0u;
    unsigned int winner_index = 0u;
    unsigned int i;
    unsigned int before_forbid;
    unsigned int before_permit;
    unsigned int before_lock;
    unsigned int before_unlock;

    CHECK(init_semaphore(&sem));
    sem.ss_QueueCount = -1;
    sem.ss_Owner = NULL;
    sem.ss_NestCount = 0u;
    atomic_store_explicit(&mock_error, 0, memory_order_relaxed);
    atomic_store_explicit(&check_semaphore_result, 1, memory_order_relaxed);
    before_forbid = atomic_load_explicit(&forbid_calls, memory_order_relaxed);
    before_permit = atomic_load_explicit(&permit_calls, memory_order_relaxed);
    before_lock = atomic_load_explicit(&gate_lock_calls, memory_order_relaxed);
    before_unlock = atomic_load_explicit(&gate_unlock_calls, memory_order_relaxed);

    for (i = 0u; i < THREAD_COUNT; ++i)
    {
        workers[i].sem = &sem;
        workers[i].task.opaque = i + 1u;
        workers[i].start = &start;
        workers[i].ready = &ready;
        workers[i].result = FALSE;
        if (pthread_create(&threads[i], NULL, attempt_worker, &workers[i]) != 0)
            break;
        ++created;
    }
    if (created != THREAD_COUNT)
    {
        atomic_store_explicit(&start, 1, memory_order_release);
        for (i = 0u; i < created; ++i)
            (void)pthread_join(threads[i], NULL);
        destroy_semaphore(&sem);
        CHECK(0 && "could not create all contention threads");
    }

    while (atomic_load_explicit(&ready, memory_order_acquire) != THREAD_COUNT)
        (void)sched_yield();
    atomic_store_explicit(&start, 1, memory_order_release);
    for (i = 0u; i < THREAD_COUNT; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);

    for (i = 0u; i < THREAD_COUNT; ++i)
    {
        if (workers[i].result == TRUE)
        {
            ++winner_count;
            winner_index = i;
        }
        else
            CHECK(workers[i].result == FALSE);
    }
    CHECK(winner_count == 1u);
    CHECK(sem.ss_QueueCount == 0);
    CHECK(sem.ss_Owner == &workers[winner_index].task);
    CHECK(sem.ss_NestCount == 1u);
    CHECK(atomic_load_explicit(&forbid_calls, memory_order_relaxed) -
              before_forbid == THREAD_COUNT);
    CHECK(atomic_load_explicit(&permit_calls, memory_order_relaxed) -
              before_permit == THREAD_COUNT);
    CHECK(atomic_load_explicit(&gate_lock_calls, memory_order_relaxed) -
              before_lock == THREAD_COUNT);
    CHECK(atomic_load_explicit(&gate_unlock_calls, memory_order_relaxed) -
              before_unlock == THREAD_COUNT);
    CHECK(atomic_load_explicit(&mock_error, memory_order_relaxed) == 0);
    destroy_semaphore(&sem);
#else
    /* Without the platform gate, Forbid is intentionally single-hart-only. */
#endif
    return 1;
}

int main(void)
{
    if (!test_state_transitions() || !test_task_pointer_is_opaque() ||
        !test_pthread_contention())
        return EXIT_FAILURE;
#ifdef TEST_WITH_PLATFORM_GATE
    (void)printf("PASS p4-gate checks=%lu\n", checks);
#else
    (void)printf("PASS normal-no-platform-gate checks=%lu\n", checks);
#endif
    return EXIT_SUCCESS;
}
'''


def main() -> int:
    source = SOURCE_PATH.read_text()
    function = extract_function(source, SIGNATURE)
    source_hash = hashlib.sha256(SOURCE_PATH.read_bytes()).hexdigest()
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise RuntimeError("clang is required for this host fixture")

    translation_unit = HARNESS.replace("/* ACTUAL_PRODUCTION_FUNCTION */", function)
    child_env = os.environ.copy()
    child_env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    child_env["UBSAN_OPTIONS"] = "halt_on_error=1"
    common_flags = [
        "-std=c11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wconversion",
        "-Wsign-conversion",
        "-Wshadow",
        "-Wstrict-prototypes",
        "-Wmissing-prototypes",
        "-pedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-pthread",
    ]

    with tempfile.TemporaryDirectory(prefix="p4-semaphore-attempt-") as temp:
        temp_dir = Path(temp)
        for variant, defines in (
            ("normal-no-platform-gate", []),
            ("p4-gate-mock", ["-DTEST_WITH_PLATFORM_GATE=1"]),
        ):
            binary = temp_dir / variant
            command = [compiler, *common_flags, *defines, "-x", "c", "-o",
                       str(binary), "-"]
            print(f"source sha256 {source_hash}")
            print(f"compile {variant}: {shlex.join(command)}")
            try:
                compiled = subprocess.run(
                    command,
                    input=translation_unit,
                    text=True,
                    capture_output=True,
                    check=False,
                    timeout=COMPILE_TIMEOUT_SECONDS,
                    env=child_env,
                )
            except subprocess.TimeoutExpired:
                print(f"ERROR: compile exceeded {COMPILE_TIMEOUT_SECONDS}s")
                return 124
            if compiled.returncode != 0:
                print(compiled.stdout, end="")
                print(compiled.stderr, end="")
                return compiled.returncode
            print(
                "run sanitizer environment: "
                "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "
                "UBSAN_OPTIONS=halt_on_error=1"
            )
            try:
                run = subprocess.run(
                    [str(binary)],
                    text=True,
                    capture_output=True,
                    check=False,
                    timeout=RUN_TIMEOUT_SECONDS,
                    env=child_env,
                )
            except subprocess.TimeoutExpired:
                print(f"ERROR: {variant} exceeded {RUN_TIMEOUT_SECONDS}s")
                return 124
            print(run.stdout, end="")
            print(run.stderr, end="")
            if run.returncode != 0:
                return run.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
