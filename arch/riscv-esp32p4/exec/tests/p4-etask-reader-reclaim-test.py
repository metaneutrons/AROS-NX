#!/usr/bin/env python3
"""Run actual P4 identity/lifetime hooks through Exec ETask reclamation."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[4]
EXEC_UTIL = REPO / "rom/exec/exec_util.c"
IDENTITY_C = REPO / "arch/riscv-esp32p4/exec/p4_etask_identity.c"
LIFETIME_C = REPO / "arch/riscv-esp32p4/exec/p4_etask_lifetime.c"
BINDING_C = REPO / "arch/riscv-esp32p4/exec/p4_etask_identity_binding.c"
IDENTITY_H = REPO / "arch/riscv-esp32p4/exec/p4_etask_identity.h"
LIFETIME_H = REPO / "arch/riscv-esp32p4/exec/p4_etask_lifetime.h"
BINDING_H = REPO / "arch/riscv-esp32p4/exec/p4_etask_identity_binding.h"


def extract_braced(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line"
            index += 1
        elif char == "/" and following == "*":
            state = "block"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
        index += 1
    raise RuntimeError(f"unterminated function after {signature!r}")


exec_util_source = EXEC_UTIL.read_text(encoding="utf-8")
expunge_body = extract_braced(
    exec_util_source, "Exec_ExpungeETask(struct ETask *et,"
)
storage_body_raw = extract_braced(
    exec_util_source, "Exec_FreeETaskStorage(struct ETask *et,"
)
# D() compiles away in this minimal harness, so mark the actual helper's
# otherwise-unused base argument in the extracted fixture signature only.
storage_body = storage_body_raw.replace(
    "struct ExecBase *SysBase)",
    "struct ExecBase *SysBase __attribute__((unused)))",
    1,
)

input_paths = [
    EXEC_UTIL,
    IDENTITY_C,
    LIFETIME_C,
    BINDING_C,
    IDENTITY_H,
    LIFETIME_H,
    BINDING_H,
]
input_hashes = {
    path.relative_to(REPO).as_posix(): hashlib.sha256(
        path.read_bytes()
    ).hexdigest()
    for path in input_paths
}
test_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()

PRELUDE = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef void *APTR;
#define MEMF_PUBLIC 1u
#define MEMF_CLEAR 2u
#define P4_SRAMDATA
#define P4_PUBLIC_SPIN_BAD_STATE 7u
#define P4_ETASK_IDENTITY_BINDING_TEST 1
#define D(...) do { } while (0);
#define EXEC_PLATFORM_ETASK_RELEASE(et) \
    krnP4ETaskIdentityRelease((et), SysBase)
#define IntETask(et) ((struct IntETask *)(et))

struct ETask
{
    uint32_t et_UniqueID;
    APTR et_Result2;
};
struct IntETask
{
    struct ETask iet_ETask;
    uint32_t private_words[6];
};
struct ExecBase { uint32_t ex_TaskID; };

#include "__IDENTITY_HEADER__"
#include "__LIFETIME_HEADER__"
#include "__BINDING_HEADER__"

static void *AllocMem(size_t size, ULONG flags);
static void FreeMem(APTR memory, size_t size);
static void FreeVec(APTR memory);
static uint32_t krnP4ETaskGateLock(struct ExecBase *base);
static void krnP4ETaskGateUnlock(uint32_t token);
static void krnP4PublicSpinFatal(unsigned int reason);
void Exec_ExpungeETask(struct ETask *et, struct ExecBase *SysBase);
void Exec_FreeETaskStorage(struct ETask *et, struct ExecBase *SysBase);

enum event_kind
{
    EVENT_ALLOC_RECORD = 1,
    EVENT_GATE_LOCK,
    EVENT_GATE_UNLOCK,
    EVENT_FREE_SIDECAR,
    EVENT_FREE_RESULT,
    EVENT_FREE_ETASK
};

static unsigned int checks;
static unsigned int event_count;
static enum event_kind events[256];
static unsigned int alloc_calls;
static unsigned int sidecar_free_calls;
static unsigned int result_free_calls;
static unsigned int etask_free_calls;
static unsigned int gate_lock_calls;
static unsigned int gate_unlock_calls;
static unsigned int fatal_calls;
static unsigned int gate_depth;
static int failed;
static struct ETask *expected_storage_etask;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(2); \
    } \
} while (0)

static void record_event(enum event_kind event)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = event;
}
"""

TESTS = r"""
static void *AllocMem(size_t size, ULONG flags)
{
    CHECK(gate_depth == 0u);
    CHECK(size == sizeof(struct p4_etask_owner_record));
    CHECK(flags == (MEMF_PUBLIC | MEMF_CLEAR));
    ++alloc_calls;
    record_event(EVENT_ALLOC_RECORD);
    return calloc(1u, size);
}

static void FreeMem(APTR memory, size_t size)
{
    CHECK(memory != NULL);
    CHECK(gate_depth == 0u);
    if (memory == (APTR)expected_storage_etask)
    {
        CHECK(size == sizeof(struct IntETask));
        CHECK(expected_storage_etask != NULL);
        CHECK(p4_etask_identity_lookup(&identities,
                                      expected_storage_etask) == NULL);
        ++etask_free_calls;
        record_event(EVENT_FREE_ETASK);
    }
    else
    {
        struct p4_etask_owner_record *owner =
            (struct p4_etask_owner_record *)memory;
        CHECK(size == sizeof(*owner));
        CHECK(p4_etask_identity_lookup(&identities,
                                       owner->identity.identity) == NULL);
        ++sidecar_free_calls;
        record_event(EVENT_FREE_SIDECAR);
    }
    free(memory);
}

static void FreeVec(APTR memory)
{
    CHECK(gate_depth == 0u);
    CHECK(expected_storage_etask != NULL);
    CHECK(memory == expected_storage_etask->et_Result2);
    CHECK(p4_etask_identity_lookup(&identities,
                                   expected_storage_etask) == NULL);
    ++result_free_calls;
    record_event(EVENT_FREE_RESULT);
    free(memory);
}

static uint32_t krnP4ETaskGateLock(struct ExecBase *base)
{
    CHECK(base != NULL);
    CHECK(gate_depth == 0u); /* The production metadata gate is nonrecursive. */
    ++gate_depth;
    ++gate_lock_calls;
    record_event(EVENT_GATE_LOCK);
    return UINT32_C(0x51);
}

static void krnP4ETaskGateUnlock(uint32_t token)
{
    CHECK(token == UINT32_C(0x51));
    CHECK(gate_depth == 1u);
    --gate_depth;
    ++gate_unlock_calls;
    record_event(EVENT_GATE_UNLOCK);
}

static void krnP4PublicSpinFatal(unsigned int reason)
{
    CHECK(reason == P4_PUBLIC_SPIN_BAD_STATE);
    ++fatal_calls;
    failed = 1;
}

static void reset_observation(void)
{
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(gate_depth == 0u);
    event_count = 0u;
    alloc_calls = 0u;
    sidecar_free_calls = 0u;
    result_free_calls = 0u;
    etask_free_calls = 0u;
    gate_lock_calls = 0u;
    gate_unlock_calls = 0u;
    fatal_calls = 0u;
    expected_storage_etask = NULL;
}

static struct ETask *new_reserved_etask(struct ExecBase *base,
                                        uint32_t current_pid)
{
    struct IntETask *allocation =
        (struct IntETask *)calloc(1u, sizeof(*allocation));
    struct ETask *et;
    CHECK(allocation != NULL);
    if (allocation == NULL)
        return NULL;
    et = &allocation->iet_ETask;
    et->et_Result2 = malloc(37u);
    CHECK(et->et_Result2 != NULL);
    expected_storage_etask = et;
    base->ex_TaskID = current_pid;
    CHECK(krnP4ETaskIdentityReserve(et, base));
    CHECK(et->et_UniqueID == current_pid + 1u);
    CHECK(p4_etask_identity_lookup(&identities, et) != NULL);
    return et;
}

static void begin_and_retain_result(struct ETask *et, struct ExecBase *base)
{
    krnP4ETaskMetadataBeginCleanup(et, base);
    krnP4ETaskMetadataRetainResult(et, base);
    CHECK(p4_etask_identity_lookup(&identities, et) != NULL);
}

static void assert_gate_calls_balanced(void)
{
    CHECK(gate_depth == 0u);
    CHECK(gate_lock_calls == gate_unlock_calls);
    CHECK(fatal_calls == 0u);
}

static void test_no_reader_expunges_immediately(void)
{
    struct ExecBase base = {300u};
    struct ETask *et;
    reset_observation();
    et = new_reserved_etask(&base, 300u);
    if (et == NULL)
        return;
    begin_and_retain_result(et, &base);

    Exec_ExpungeETask(et, &base);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(alloc_calls == 1u && sidecar_free_calls == 1u);
    CHECK(result_free_calls == 1u && etask_free_calls == 1u);
    CHECK(event_count >= 6u);
    CHECK(events[event_count - 3u] == EVENT_FREE_SIDECAR);
    CHECK(events[event_count - 2u] == EVENT_FREE_RESULT);
    CHECK(events[event_count - 1u] == EVENT_FREE_ETASK);
    assert_gate_calls_balanced();
}

static void test_two_readers_defer_final_storage_and_pid_release(void)
{
    struct ExecBase base = {400u};
    struct ETask *et;
    struct p4_etask_identity_record *record;
    struct p4_etask_owner_record *owner;
    struct ETask first_probe = {0};
    struct ETask second_probe = {0};
    APTR retained_result;
    uint32_t pid;
    unsigned int sidecars_before_unpin;
    unsigned int results_before_unpin;
    unsigned int tasks_before_unpin;

    reset_observation();
    et = new_reserved_etask(&base, 400u);
    if (et == NULL)
        return;
    pid = et->et_UniqueID;
    retained_result = et->et_Result2;
    CHECK(krnP4ETaskMetadataReadPin(et, &base));
    CHECK(krnP4ETaskMetadataReadPin(et, &base));
    begin_and_retain_result(et, &base);

    Exec_ExpungeETask(et, &base);
    record = p4_etask_identity_lookup(&identities, et);
    CHECK(record != NULL && record->pid == pid);
    CHECK(et->et_UniqueID == pid && et->et_Result2 == retained_result);
    owner = (struct p4_etask_owner_record *)record;
    CHECK(owner->lifetime.task_ref == 0u);
    CHECK(owner->lifetime.result_ref == 0u);
    CHECK(owner->lifetime.readers == 2u);
    CHECK(owner->lifetime.expunge == 1u);
    CHECK(owner->lifetime.reclaim_claimed == 0u);
    CHECK(sidecar_free_calls == 0u && result_free_calls == 0u &&
          etask_free_calls == 0u);
    CHECK(krnP4ETaskMetadataReadPin(et, &base) == 0);
    CHECK(owner->lifetime.readers == 2u);
    CHECK(sidecar_free_calls == 0u && result_free_calls == 0u &&
          etask_free_calls == 0u);

    /* Force PID selection to try the retained PID first. The live retained
     * record must keep that candidate occupied while its readers remain. */
    base.ex_TaskID = pid - 1u;
    CHECK(krnP4ETaskIdentityReserve(&first_probe, &base));
    CHECK(first_probe.et_UniqueID != pid);
    CHECK(first_probe.et_UniqueID == pid + 1u);
    CHECK(krnP4ETaskIdentityRelease(&first_probe, &base) == 1);

    sidecars_before_unpin = sidecar_free_calls;
    results_before_unpin = result_free_calls;
    tasks_before_unpin = etask_free_calls;
    expected_storage_etask = et;
    krnP4ETaskMetadataReadUnpin(et, &base);
    CHECK(sidecar_free_calls == sidecars_before_unpin);
    CHECK(result_free_calls == results_before_unpin);
    CHECK(etask_free_calls == tasks_before_unpin);
    record = p4_etask_identity_lookup(&identities, et);
    CHECK(record != NULL && record->pid == pid);
    owner = (struct p4_etask_owner_record *)record;
    CHECK(owner->lifetime.readers == 1u);
    CHECK(owner->lifetime.reclaim_claimed == 0u);

    /* The second, unique last-unpin claim detaches PID/sidecar under the gate,
     * then frees outside it through the actual Exec helper. */
    expected_storage_etask = et;
    krnP4ETaskMetadataReadUnpin(et, &base);
    CHECK(identities.head == NULL && identities.count == 0u);
    CHECK(sidecar_free_calls == sidecars_before_unpin + 1u);
    CHECK(result_free_calls == results_before_unpin + 1u);
    CHECK(etask_free_calls == tasks_before_unpin + 1u);
    CHECK(events[event_count - 4u] == EVENT_GATE_UNLOCK);
    CHECK(events[event_count - 3u] == EVENT_FREE_SIDECAR);
    CHECK(events[event_count - 2u] == EVENT_FREE_RESULT);
    CHECK(events[event_count - 1u] == EVENT_FREE_ETASK);
    assert_gate_calls_balanced();
    expected_storage_etask = NULL;

    /* Once the final lease released the old registry record, its PID may be
     * selected again when the same starting counter is presented. */
    base.ex_TaskID = pid - 1u;
    CHECK(krnP4ETaskIdentityReserve(&second_probe, &base));
    CHECK(second_probe.et_UniqueID == pid);
    CHECK(krnP4ETaskIdentityRelease(&second_probe, &base) == 1);
    CHECK(identities.head == NULL && identities.count == 0u);
    assert_gate_calls_balanced();
}

int main(void)
{
    test_no_reader_expunges_immediately();
    test_two_readers_defer_final_storage_and_pid_release();
    if (failed)
        return 1;
    printf("p4-etask-reader-reclaim-test: PASS (%u checks)\n", checks);
    return 0;
}
"""


def main() -> int:
    compiler = shutil.which(os.environ.get("CC", "clang"))
    if compiler is None:
        raise SystemExit("clang was not found; set CC to a Clang executable")

    prelude = (
        PRELUDE.replace("__IDENTITY_HEADER__", str(IDENTITY_H))
        .replace("__LIFETIME_HEADER__", str(LIFETIME_H))
        .replace("__BINDING_HEADER__", str(BINDING_H))
    )
    source_includes = "\n".join(
        f'#include "{path}"' for path in (IDENTITY_C, LIFETIME_C, BINDING_C)
    )
    with tempfile.TemporaryDirectory(prefix="p4-etask-reader-reclaim-") as temp:
        temp_path = Path(temp)
        generated = temp_path / "fixture.c"
        binary = temp_path / "fixture"
        generated.write_text(
            prelude
            + "\n"
            + source_includes
            + "\nvoid\n"
            + expunge_body
            + "\nvoid\n"
            + storage_body
            + "\n"
            + TESTS
        )
        command = [
            compiler,
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wconversion",
            "-Wsign-conversion",
            "-Wpedantic",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            str(generated),
            "-o",
            str(binary),
        ]
        print("Source hashes:")
        for name, value in input_hashes.items():
            print(f"  {name}: {value}")
        print("Exec_ExpungeETask body SHA-256:",
              hashlib.sha256(expunge_body.encode()).hexdigest())
        print("Exec_FreeETaskStorage body SHA-256:",
              hashlib.sha256(storage_body_raw.encode()).hexdigest())
        print("Fixture SHA-256:", test_hash)
        print("Compile:", shlex.join(command))
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
