#!/usr/bin/env python3
"""Run the actual P4 creation-channel pin wrappers in a bounded host model.

The wrapper/helper bodies are extracted verbatim from p4_task_pins.c. The
registry, lifetime, semaphore-ledger, and removal-bank implementations are
included from their production C sources. Only CPU runtime, gate/IRQ, and fatal
platform boundaries are modeled.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
KERNEL = ROOT / "arch/riscv-esp32p4/kernel"
PINS_C = KERNEL / "p4_task_pins.c"

FUNCTIONS = (
    "p4_creation_current_locked",
    "p4_creation_stack_frame",
    "krnP4CreationChannelBegin",
    "krnP4CreationChannelEnd",
    "krnP4CreationChannelResolve",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_function(source: str, name: str) -> str:
    """Extract one whole C function, ignoring braces in comments/literals."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\(", source)
    if not match:
        raise ValueError(f"function not found: {name}")
    function_start = source.rfind("\n", 0, match.start()) + 1
    open_paren = source.find("(", match.start())
    paren_depth = 0
    cursor = open_paren
    while cursor < len(source):
        char = source[cursor]
        if char == "(":
            paren_depth += 1
        elif char == ")":
            paren_depth -= 1
            if paren_depth == 0:
                break
        cursor += 1
    if cursor == len(source):
        raise ValueError(f"unterminated parameter list: {name}")
    body = source.find("{", cursor)
    if body < 0:
        raise ValueError(f"function body not found: {name}")

    depth = 0
    state = "code"
    escaped = False
    cursor = body
    while cursor < len(source):
        char = source[cursor]
        following = source[cursor + 1] if cursor + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                cursor += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        else:
            if char == "/" and following == "/":
                state = "line_comment"
                cursor += 1
            elif char == "/" and following == "*":
                state = "block_comment"
                cursor += 1
            elif char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[function_start : cursor + 1]
        cursor += 1
    raise ValueError(f"unterminated function body: {name}")


def harness_source(extracted: str) -> str:
    paths = {
        name: (KERNEL / name).as_posix()
        for name in (
            "p4_task_lifetime.c",
            "p4_semaphore_ledger.c",
            "p4_task_registry.c",
        )
    }
    paths["p4_remove_completion.c"] = (
        KERNEL.parent / "exec/p4_remove_completion.c"
    ).as_posix()
    paths["p4_remove_bank.c"] = (KERNEL.parent / "exec/p4_remove_bank.c").as_posix()

    return r'''#define _DEFAULT_SOURCE 1
#define __AROSEXEC_SMP__ 1
#define __AROSPLATFORM_SMP__ 1
#define P4_PUBLIC_SPINLOCK_TEST 1

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define P4_CPU_LOCAL_READ_MHARTID p4_creation_mock_hartid
uint32_t p4_creation_mock_hartid(void)
{
    return 0u;
}

struct Task
{
    void *tc_SPLower;
    void *tc_SPUpper;
};
struct ExecBase { uint32_t marker; };

#include "cpu_local_runtime.h"
#include "p4_public_spinlock.h"
#include "p4_task_registry.h"

static struct p4_task_registry p4_task_records;
static unsigned int gate_depth;
static unsigned int irq_depth;
static unsigned int lock_count;
static unsigned int unlock_count;
static unsigned int fatal_count;
static enum p4_public_spinlock_result fatal_reason;
static jmp_buf fatal_env;
static int fatal_armed;
static struct ExecBase sysbase = { UINT32_C(0x50c4) };
static struct p4_cpu_local cpu_local;
static struct p4_exec_runtime runtime;
static struct p4_exec_runtime *runtime_result;

void Disable(void)
{
    ++irq_depth;
}

void Enable(void)
{
    if (irq_depth == 0u || gate_depth != 0u)
    {
        fprintf(stderr, "mock Enable invariant failed\n");
        abort();
    }
    --irq_depth;
}

static void p4_task_pins_lock(struct ExecBase *SysBase)
{
    (void)SysBase;
    if (gate_depth != 0u || irq_depth != 0u)
    {
        fprintf(stderr, "mock gate acquisition invariant failed\n");
        abort();
    }
    Disable();
    gate_depth = 1u;
    ++lock_count;
}

static void p4_task_pins_unlock(struct ExecBase *SysBase)
{
    (void)SysBase;
    if (gate_depth != 1u || irq_depth != 1u)
    {
        fprintf(stderr, "mock gate release invariant failed\n");
        abort();
    }
    gate_depth = 0u;
    ++unlock_count;
    Enable();
}

struct p4_exec_runtime *p4_exec_runtime_current(struct ExecBase *base)
{
    if (base != &sysbase)
        return NULL;
    return runtime_result;
}

void krnP4PublicSpinFatal(enum p4_public_spinlock_result reason)
{
    ++fatal_count;
    fatal_reason = reason;
    if (fatal_armed)
        longjmp(fatal_env, 1);
    abort();
}

#include "''' + paths["p4_task_lifetime.c"] + r'''"
#include "''' + paths["p4_semaphore_ledger.c"] + r'''"
#include "''' + paths["p4_remove_completion.c"] + r'''"
#include "''' + paths["p4_remove_bank.c"] + r'''"
#include "''' + paths["p4_task_registry.c"] + r'''"

''' + extracted + r'''

static unsigned int checks;
static unsigned int failures;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

struct task_fixture
{
    struct Task task;
    _Alignas(16) unsigned char stack[512];
    struct p4_task_registry_record record;
};

static struct task_fixture owner_storage;
static struct task_fixture target_storage;

static struct p4_creation_channel *frame_at(size_t offset)
{
    return (struct p4_creation_channel *)(void *)&owner_storage.stack[offset];
}

static void fixture_reset(uint32_t hart)
{
    memset(&owner_storage, 0, sizeof(owner_storage));
    memset(&target_storage, 0, sizeof(target_storage));
    memset(&p4_task_records, 0, sizeof(p4_task_records));
    p4_task_registry_init(&p4_task_records);
    owner_storage.task.tc_SPLower = owner_storage.stack;
    owner_storage.task.tc_SPUpper = owner_storage.stack + sizeof(owner_storage.stack);
    target_storage.task.tc_SPLower = target_storage.stack;
    target_storage.task.tc_SPUpper = target_storage.stack + sizeof(target_storage.stack);
    memset(&cpu_local, 0, sizeof(cpu_local));
    memset(&runtime, 0, sizeof(runtime));
    cpu_local.hart_id = hart;
    cpu_local.current_task = &owner_storage.task;
    cpu_local.sys_base = &sysbase;
    runtime.hart_id = hart;
    runtime.initialized = 1u;
    runtime.cpu_local = &cpu_local;
    runtime.stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
    runtime_result = &runtime;
    gate_depth = 0u;
    irq_depth = 0u;
    lock_count = 0u;
    unlock_count = 0u;
    fatal_count = 0u;
    fatal_armed = 0;
    p4_task_pins_lock(&sysbase);
    if (p4_task_registry_register_locked(&p4_task_records,
            &owner_storage.record, &owner_storage.task) != 1 ||
        p4_task_registry_enter_running_locked(&p4_task_records,
            &owner_storage.task, hart) != 1)
    {
        p4_task_pins_unlock(&sysbase);
        fprintf(stderr, "fixture registry setup failed\n");
        abort();
    }
    p4_task_pins_unlock(&sysbase);
}

static void check_wrapper_balance(void)
{
    CHECK(gate_depth == 0u);
    CHECK(irq_depth == 0u);
    CHECK(lock_count == unlock_count);
}

static void test_live_and_one_use(void)
{
    struct p4_creation_channel *frame;
    struct p4_creation_channel *other_frame;
    struct p4_creation_channel frame_before;
    struct p4_task_registry_record record_before;
    uint32_t *resolved;
    uint32_t generation;
    fixture_reset(0u);
    frame = frame_at(32u);
    other_frame = frame_at(128u);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 1);
    generation = frame->generation;
    CHECK(generation != 0u && frame->value == 1u);
    frame_before = *frame;
    record_before = owner_storage.record;
    /* These failures precede the valid resolve so they test the exact match. */
    CHECK(krnP4CreationChannelResolve(&owner_storage.task, generation,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation + 1u,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)&other_frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)&frame->value + 1u, &sysbase) == NULL);
    CHECK(memcmp(frame, &frame_before, sizeof(frame_before)) == 0);
    CHECK(memcmp(&owner_storage.record, &record_before, sizeof(record_before)) == 0);
    resolved = krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)&frame->value, &sysbase);
    CHECK(resolved == &frame->value);
    if (resolved != NULL)
        *resolved = 2u;
    CHECK(frame->value == 2u);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&owner_storage.task, generation,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation + 1u,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)&frame->generation, &sysbase) == NULL);
    CHECK(krnP4CreationChannelEnd(generation + 1u, frame, &sysbase) == 0);
    CHECK(frame->generation == generation);
    CHECK(krnP4CreationChannelEnd(generation, frame, &sysbase) == 1);
    CHECK(frame->generation == 0u);
    CHECK(owner_storage.record.creation_frame == 0u);
    check_wrapper_balance();
}

static void test_runtime_admission_refusals(void)
{
    struct p4_creation_channel *frame;
    struct p4_cpu_local *saved_local;
    struct Task *saved_task;
    struct p4_exec_runtime *saved_runtime;
    fixture_reset(0u);
    frame = frame_at(32u);

    saved_task = cpu_local.current_task;
    cpu_local.current_task = &target_storage.task;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    CHECK(frame->generation == 0u);
    cpu_local.current_task = saved_task;

    runtime.hart_id = 1u;
    cpu_local.hart_id = 1u;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    CHECK(frame->generation == 0u);
    runtime.hart_id = 0u;
    cpu_local.hart_id = 0u;

    cpu_local.trap_depth = 1u;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    CHECK(krnP4CreationChannelEnd(1u, frame, &sysbase) == 0);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, 1u,
        (uintptr_t)&frame->value, &sysbase) == NULL);
    cpu_local.trap_depth = 0u;

    saved_runtime = runtime_result;
    runtime_result = NULL;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    runtime_result = saved_runtime;
    saved_local = runtime.cpu_local;
    runtime.cpu_local = NULL;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    runtime.cpu_local = saved_local;

    saved_runtime = runtime_result;
    runtime_result = (struct p4_exec_runtime *)(void *)&target_storage;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame,
        (struct ExecBase *)(void *)&target_storage) == 0);
    runtime_result = saved_runtime;
    check_wrapper_balance();
}

static void test_migrated_owner_and_nested_restore(void)
{
    struct p4_creation_channel *outer;
    struct p4_creation_channel *inner;
    uint32_t outer_generation;
    uint32_t inner_generation;
    fixture_reset(0u);
    outer = frame_at(32u);
    inner = frame_at(128u);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, outer, &sysbase) == 1);
    outer_generation = outer->generation;
    p4_task_pins_lock(&sysbase);
    CHECK(p4_task_registry_leave_running_locked(&p4_task_records,
        &owner_storage.task, 0u) == 1);
    CHECK(p4_task_registry_enter_running_locked(&p4_task_records,
        &owner_storage.task, 1u) == 1);
    p4_task_pins_unlock(&sysbase);
    runtime.hart_id = 1u;
    cpu_local.hart_id = 1u;
    CHECK(krnP4CreationChannelBegin(&target_storage.task, inner, &sysbase) == 1);
    inner_generation = inner->generation;
    CHECK(inner->previous_frame == (uintptr_t)outer);
    CHECK(inner->previous_generation == outer_generation);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, inner_generation,
        (uintptr_t)&inner->value, &sysbase) == &inner->value);
    CHECK(krnP4CreationChannelEnd(inner_generation, inner, &sysbase) == 1);
    CHECK(owner_storage.record.creation_frame == (uintptr_t)outer);
    CHECK(owner_storage.record.creation_generation == outer_generation);
    /* The outer token was still unused before nesting, so it is restored. */
    CHECK(krnP4CreationChannelResolve(&target_storage.task, outer_generation,
        (uintptr_t)&outer->value, &sysbase) == &outer->value);
    CHECK(krnP4CreationChannelEnd(outer_generation, outer, &sysbase) == 1);

    outer = frame_at(224u);
    inner = frame_at(320u);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, outer, &sysbase) == 1);
    outer_generation = outer->generation;
    CHECK(krnP4CreationChannelResolve(&target_storage.task, outer_generation,
        (uintptr_t)&outer->value, &sysbase) == &outer->value);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, inner, &sysbase) == 1);
    inner_generation = inner->generation;
    CHECK(krnP4CreationChannelEnd(inner_generation, inner, &sysbase) == 1);
    CHECK(owner_storage.record.creation_frame == (uintptr_t)outer);
    /* A consumed outer authority stays consumed after the nested scope. */
    CHECK(krnP4CreationChannelResolve(&target_storage.task, outer_generation,
        (uintptr_t)&outer->value, &sysbase) == NULL);
    CHECK(krnP4CreationChannelEnd(outer_generation, outer, &sysbase) == 1);
    check_wrapper_balance();
}

static void test_stack_extent_and_fatal_paths(void)
{
    struct p4_creation_channel *good;
    struct p4_creation_channel *bad;
    struct p4_creation_channel *exact_end;
    struct p4_task_registry_record record_before;
    uint32_t generation;
    long page_size = sysconf(_SC_PAGESIZE);
    void *guard;
    if (page_size <= 0)
        abort();
    guard = mmap(NULL, (size_t)page_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guard == MAP_FAILED)
        abort();
    fixture_reset(0u);
    good = frame_at(32u);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, good, &sysbase) == 1);
    generation = good->generation;
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        (uintptr_t)guard + 64u, &sysbase) == NULL);
    CHECK(krnP4CreationChannelEnd(generation, good, &sysbase) == 1);
    exact_end = (struct p4_creation_channel *)(void *)
        (owner_storage.stack + sizeof(owner_storage.stack) -
         sizeof(struct p4_creation_channel));
    memset(exact_end, 0, sizeof(*exact_end));
    CHECK(krnP4CreationChannelBegin(&target_storage.task, exact_end,
        &sysbase) == 1);
    CHECK(krnP4CreationChannelEnd(exact_end->generation, exact_end,
        &sysbase) == 1);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, good, &sysbase) == 1);
    generation = good->generation;

    /* This aligned frame begins inside bounds but does not fit in full. */
    bad = (struct p4_creation_channel *)(void *)
        (owner_storage.stack + sizeof(owner_storage.stack) -
         _Alignof(struct p4_creation_channel));
    record_before = owner_storage.record;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
        (void)krnP4CreationChannelBegin(&target_storage.task, bad, &sysbase);
    else
        CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    fatal_armed = 0;
    CHECK(fatal_count == 1u);
    CHECK(memcmp(&owner_storage.record, &record_before, sizeof(record_before)) == 0);
    /* The fatal hook models a terminal halt; reset mock lock state for audit. */
    CHECK(gate_depth == 1u && irq_depth == 1u);
    CHECK(lock_count == unlock_count + 1u);
    gate_depth = 0u;
    irq_depth = 0u;
    unlock_count = lock_count;
    CHECK(krnP4CreationChannelEnd(generation, good, &sysbase) == 1);
    CHECK(munmap(guard, (size_t)page_size) == 0);

    CHECK(krnP4CreationChannelBegin(&target_storage.task, good, &sysbase) == 1);
    generation = good->generation;
    record_before = owner_storage.record;
    fatal_armed = 1;
    if (setjmp(fatal_env) == 0)
        (void)krnP4CreationChannelEnd(generation, bad, &sysbase);
    else
        CHECK(fatal_reason == P4_PUBLIC_SPIN_BAD_STATE);
    fatal_armed = 0;
    CHECK(fatal_count == 2u);
    CHECK(memcmp(&owner_storage.record, &record_before, sizeof(record_before)) == 0);
    CHECK(gate_depth == 1u && irq_depth == 1u);
    CHECK(lock_count == unlock_count + 1u);
    gate_depth = 0u;
    irq_depth = 0u;
    unlock_count = lock_count;
    CHECK(krnP4CreationChannelEnd(generation, good, &sysbase) == 1);
    check_wrapper_balance();
}

static void test_prot_none_tag_and_retired_owner(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char *task_page;
    unsigned char *frame_page;
    struct Task *guarded_task;
    struct p4_creation_channel *frame;
    uint32_t generation;
    uintptr_t frame_value_address;
    if (page_size <= 0)
        abort();
    task_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    frame_page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (task_page == MAP_FAILED || frame_page == MAP_FAILED)
        abort();
    memset(&owner_storage, 0, sizeof(owner_storage));
    memset(&target_storage, 0, sizeof(target_storage));
    memset(&p4_task_records, 0, sizeof(p4_task_records));
    p4_task_registry_init(&p4_task_records);
    memset(&cpu_local, 0, sizeof(cpu_local));
    memset(&runtime, 0, sizeof(runtime));
    gate_depth = 0u;
    irq_depth = 0u;
    lock_count = 0u;
    unlock_count = 0u;
    fatal_count = 0u;
    fatal_armed = 0;
    guarded_task = (struct Task *)(void *)task_page;
    guarded_task->tc_SPLower = frame_page;
    guarded_task->tc_SPUpper = frame_page + (size_t)page_size;
    cpu_local.hart_id = 0u;
    cpu_local.current_task = guarded_task;
    cpu_local.sys_base = &sysbase;
    runtime.hart_id = 0u;
    runtime.initialized = 1u;
    runtime.cpu_local = &cpu_local;
    runtime.stage = P4_EXEC_RUNTIME_STAGE_FINALIZED;
    runtime_result = &runtime;
    p4_task_pins_lock(&sysbase);
    CHECK(p4_task_registry_register_locked(&p4_task_records,
        &owner_storage.record, guarded_task) == 1);
    CHECK(p4_task_registry_enter_running_locked(&p4_task_records,
        guarded_task, 0u) == 1);
    p4_task_pins_unlock(&sysbase);
    frame = (struct p4_creation_channel *)(void *)(frame_page + 64u);
    memset(frame, 0, sizeof(*frame));
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 1);
    generation = frame->generation;
    frame_value_address = (uintptr_t)frame +
        offsetof(struct p4_creation_channel, value);
    /* Once the owner retires, the wrapper must reject before frame access. */
    p4_task_pins_lock(&sysbase);
    CHECK(p4_task_registry_retire_for_service_locked(&p4_task_records,
        guarded_task) == 1);
    p4_task_pins_unlock(&sysbase);
    CHECK(mprotect(task_page, (size_t)page_size, PROT_NONE) == 0);
    CHECK(mprotect(frame_page, (size_t)page_size, PROT_NONE) == 0);
    CHECK(krnP4CreationChannelBegin(&target_storage.task, frame, &sysbase) == 0);
    CHECK(krnP4CreationChannelResolve(&target_storage.task, generation,
        frame_value_address, &sysbase) == NULL);
    CHECK(krnP4CreationChannelEnd(generation, frame, &sysbase) == 0);
    CHECK(munmap(task_page, (size_t)page_size) == 0);
    CHECK(munmap(frame_page, (size_t)page_size) == 0);
    check_wrapper_balance();
}

int main(void)
{
    test_live_and_one_use();
    test_runtime_admission_refusals();
    test_migrated_owner_and_nested_restore();
    test_stack_extent_and_fatal_paths();
    test_prot_none_tag_and_retired_owner();
    if (failures != 0u)
    {
        fprintf(stderr, "%u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("P4 actual creation-channel wrappers: %u checks passed\n", checks);
    return 0;
}
'''.replace(
        '#include "cpu_local_runtime.h"',
        f'#include "{(KERNEL / "cpu_local_runtime.h").as_posix()}"',
    ).replace(
        '#include "p4_public_spinlock.h"',
        f'#include "{(KERNEL / "p4_public_spinlock.h").as_posix()}"',
    ).replace(
        '#include "p4_task_registry.h"',
        f'#include "{(KERNEL / "p4_task_registry.h").as_posix()}"',
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "p4-creation-channel-binding",
    )
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang"))
    args = parser.parse_args()
    args.artifact_dir.mkdir(parents=True, exist_ok=True)

    source = PINS_C.read_text(encoding="utf-8")
    extracted = "\n\n".join(extract_function(source, name) for name in FUNCTIONS)
    generated = harness_source(extracted)
    generated_path = args.artifact_dir / "p4-creation-channel-actual-wrapper-harness.c"
    generated_path.write_text(generated, encoding="utf-8")
    print(f"p4_task_pins.c sha256 {sha256(PINS_C)}")
    print(f"extracted functions {', '.join(FUNCTIONS)}")
    print(f"generated harness sha256 {sha256(generated_path)}")

    failures = 0
    for optimization in ("O1", "O2"):
        binary = args.artifact_dir / f"p4-creation-channel-actual-wrapper-{optimization}"
        command = [
            args.clang,
            "-std=c11",
            f"-{optimization}",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wconversion",
            "-Wpedantic",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer",
            "-I",
            str(KERNEL),
            str(generated_path),
            "-o",
            str(binary),
        ]
        print("compile:", shlex.join(command))
        compile_result = subprocess.run(command, text=True, capture_output=True)
        if compile_result.stdout:
            print(compile_result.stdout, end="")
        if compile_result.stderr:
            print(compile_result.stderr, end="", file=sys.stderr)
        if compile_result.returncode != 0:
            print(f"{optimization}: compile failed ({compile_result.returncode})")
            failures += 1
            continue

        run_command = [str(binary)]
        print("run:", shlex.join(run_command))
        run_result = subprocess.run(
            run_command,
            text=True,
            capture_output=True,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"},
            timeout=20,
        )
        if run_result.stdout:
            print(run_result.stdout, end="")
        if run_result.stderr:
            print(run_result.stderr, end="", file=sys.stderr)
        if run_result.returncode != 0:
            print(f"{optimization}: run failed ({run_result.returncode})")
            failures += 1
            continue
        print(f"{optimization} binary sha256 {sha256(binary)}")
        print(f"{optimization} binary bytes {binary.stat().st_size}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
