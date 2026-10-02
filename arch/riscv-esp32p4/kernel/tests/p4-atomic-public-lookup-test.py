#!/usr/bin/env python3
"""Exercise the extracted P4 public atomic lookup/cache with mocked LVOs.

This is a host fixture for the public service contract only. In particular,
the mocked OpenResource cannot establish that the real resource-list lock is
safe when lookup is interrupted or re-entered.
"""
from pathlib import Path
import re
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[4]
header = (repo / "arch/riscv-esp32p4/include/aros/platform_atomic.h").read_text()


def extract_function(signature):
    match = re.search(signature, header)
    if not match:
        raise RuntimeError(f"could not find production function: {signature}")
    opening = header.find("{", match.end())
    depth = 0
    for offset in range(opening, len(header)):
        if header[offset] == "{":
            depth += 1
        elif header[offset] == "}":
            depth -= 1
            if depth == 0:
                return header[match.start():offset + 1]
    raise RuntimeError(f"unterminated production function: {signature}")


missing = extract_function(
    r"static inline\s+__attribute__\(\(noreturn\)\)\s+void\s+"
    r"p4_atomic_missing_service\s*\(\s*void\s*\)")
service = extract_function(
    r"static inline\s+const struct KrnAtomicOps\s*\*\s*"
    r"p4_atomic_public_service\s*\(")

# Replace only the two RISC-V instructions with a bounded host fatal escape.
# The extracted missing-service hook still owns the fatal control flow.
missing, csr_count = re.subn(
    r'__asm__\s+volatile\("csrci mstatus, 8"\s*:::\s*"memory"\);',
    "test_atomic_missing_service_stop();", missing)
missing, wfi_count = re.subn(
    r'for\s*\(\s*;\s*;\s*\)\s*__asm__\s+'
    r'volatile\("wfi"\s*:::\s*"memory"\);',
    "for (;;) test_atomic_wait_forever();", missing)
if csr_count != 1 or wfi_count != 1:
    raise RuntimeError("unexpected production missing-service hook body")

macro_start = header.index(
    "#define P4_ATOMIC_OPERATE(address, width, operation, operand) \\",
    header.index("#define P4_ATOMIC_OPERATE(address, width, operation, operand) \\") + 1)
macro_end = header.index(
    "#endif /* Exec SMP; normal P4 retains its legacy atomic selection */",
    macro_start)
public_macros = header[macro_start:macro_end]
public_macros, branch_end_count = re.subn(r"\n#endif\n", "\n", public_macros, count=1)
if branch_end_count != 1:
    raise RuntimeError("could not isolate public macro branch from core selector")

fixture = r'''#include <aros/atomic_ops.h>
#include <inttypes.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *APTR;
typedef const char *CONST_STRPTR;
#define TAG_USER 0x80000000u
#define KATTR_CPULoad (TAG_USER + 0x03f00004u)
#define KATTR_CPULoad_END (KATTR_CPULoad + 32u)
#define KATTR_AtomicOps (KATTR_CPULoad_END + 11u)
struct ExecBase { unsigned marker; };
static struct ExecBase exec_base = { 0x41524f53u };
static int mode;
static unsigned checks;
static unsigned fatal_calls;
static unsigned open_calls;
static unsigned attr_calls;
static unsigned operate_calls;
static unsigned compare_calls;
static unsigned cache_load_target;
static unsigned cache_load_waiters;
static int cache_load_released;
static int reenter_open_once;
static unsigned open_depth;
static unsigned max_open_depth;
static jmp_buf fatal_escape;

enum {
    MODE_VALID,
    MODE_NULL_KERNEL,
    MODE_UNSUPPORTED,
    MODE_ZERO,
    MODE_MINUS_ONE,
    MODE_WRONG_VERSION,
    MODE_SHORT_SIZE,
    MODE_NULL_OPERATE,
    MODE_NULL_CAS,
    MODE_CONCURRENT,
    MODE_REENTRANT,
    MODE_MACRO
};

static pthread_mutex_t load_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t load_condition = PTHREAD_COND_INITIALIZER;
static const struct KrnAtomicOps *test_cache_load(
    const struct KrnAtomicOps *const *address, int order)
{
    const struct KrnAtomicOps *value;
    __atomic_load(address, &value, order);
    if (value || cache_load_target == 0)
        return value;

    pthread_mutex_lock(&load_mutex);
    cache_load_waiters++;
    if (cache_load_waiters == cache_load_target)
    {
        cache_load_released = 1;
        pthread_cond_broadcast(&load_condition);
    }
    while (!cache_load_released)
        pthread_cond_wait(&load_condition, &load_mutex);
    pthread_mutex_unlock(&load_mutex);
    return value;
}

/* Deterministically hold every concurrent caller after its cold cache load. */
#define __atomic_load_n(pointer, order) test_cache_load((pointer), (order))

static struct KrnAtomicOps active_table;
static unsigned kernel_resource;
static volatile void *last_address;
static uint32_t last_width;
static uint32_t last_operation;
static uint32_t last_operand;
static uint32_t last_expected;
static uint32_t last_desired;
static unsigned address_evaluations;
static unsigned operand_evaluations;

static void check_impl(int condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "line %d: check failed: %s\n", line, expression);
        abort();
    }
    __atomic_add_fetch(&checks, 1u, __ATOMIC_RELAXED);
}
#define CHECK(expression) check_impl(!!(expression), #expression, __LINE__)

static _Noreturn void test_atomic_missing_service_stop(void)
{
    fatal_calls++;
    longjmp(fatal_escape, 1);
}

static void test_atomic_wait_forever(void)
{
    abort();
}

static const struct KrnAtomicOps *p4_atomic_public_service(
    struct ExecBase *exec_base);

static uint32_t mock_operate(volatile void *address, uint32_t width,
                             uint32_t operation, uint32_t operand)
{
    __atomic_add_fetch(&operate_calls, 1u, __ATOMIC_RELAXED);
    last_address = address;
    last_width = width;
    last_operation = operation;
    last_operand = operand;
    return 0x13579bdfu;
}

static uint32_t mock_compare_exchange(volatile void *address, uint32_t width,
                                     uint32_t expected, uint32_t desired)
{
    __atomic_add_fetch(&compare_calls, 1u, __ATOMIC_RELAXED);
    last_address = address;
    last_width = width;
    last_expected = expected;
    last_desired = desired;
    return 0x2468ace0u;
}

static APTR mock_OpenResource(unsigned lvo, struct ExecBase *base,
                              CONST_STRPTR name)
{
    CHECK(lvo == 83u);
    CHECK(base == &exec_base);
    CHECK(name && strcmp(name, "kernel.resource") == 0);
    __atomic_add_fetch(&open_calls, 1u, __ATOMIC_RELAXED);
    if (mode == MODE_REENTRANT)
    {
        open_depth++;
        if (open_depth > max_open_depth)
            max_open_depth = open_depth;
    }

    if (mode == MODE_NULL_KERNEL)
        return NULL;

    if (reenter_open_once)
    {
        const struct KrnAtomicOps *nested;
        reenter_open_once = 0;
        CHECK(attr_calls == 0u);
        nested = p4_atomic_public_service(base);
        CHECK(nested == &active_table);
        CHECK(attr_calls == 1u);
    }
    if (mode == MODE_REENTRANT)
        open_depth--;
    return &kernel_resource;
}

static intptr_t mock_KrnGetSystemAttr(unsigned lvo, APTR kernel,
                                      uint32_t attribute)
{
    CHECK(lvo == 29u);
    CHECK(kernel == &kernel_resource);
    CHECK(attribute == KATTR_AtomicOps);
    __atomic_add_fetch(&attr_calls, 1u, __ATOMIC_RELAXED);
    if (mode == MODE_UNSUPPORTED || mode == MODE_MINUS_ONE)
        return -1;
    if (mode == MODE_ZERO)
        return 0;

    if (mode == MODE_WRONG_VERSION)
        active_table.version++;
    if (mode == MODE_SHORT_SIZE)
        active_table.size--;
    if (mode == MODE_NULL_OPERATE)
        active_table.operate = NULL;
    if (mode == MODE_NULL_CAS)
        active_table.compare_exchange = NULL;
    return (intptr_t)&active_table;
}

#define AROS_LCA(type, value, reg) ((type)(value))
#define AROS_LC1(ret, name, arg, base_type, base, lvo, library) \
    mock_##name((unsigned)(lvo), (base), (arg))
#define __aros_getbase_SysBase() (&exec_base)
''' + missing + "\n" + service + "\n#undef __atomic_load_n\n" + public_macros + r'''
struct Cell { uint32_t value; };
static struct Cell cells[2];
static struct Cell *next_cell(void)
{
    address_evaluations++;
    return &cells[0];
}
static uint32_t next_operand(void)
{
    operand_evaluations++;
    return 0x00c0ffeeu;
}

static void setup_table(void)
{
    active_table.version = KRN_ATOMIC_OPS_VERSION;
    active_table.size = sizeof(active_table);
    active_table.operate = mock_operate;
    active_table.compare_exchange = mock_compare_exchange;
}

static void expect_fatal(void)
{
    unsigned expected_attr_calls = mode == MODE_NULL_KERNEL ? 0u : 1u;
    if (setjmp(fatal_escape) == 0)
    {
        (void)p4_atomic_public_service(&exec_base);
        CHECK(0 && "missing service returned instead of failing closed");
    }
    CHECK(fatal_calls == 1u);
    CHECK(open_calls == 1u);
    CHECK(attr_calls == expected_attr_calls);
}

static void test_cold_hot_valid(void)
{
    const struct KrnAtomicOps *first =
        p4_atomic_public_service(&exec_base);
    CHECK(first == &active_table);
    CHECK(open_calls == 1u && attr_calls == 1u);
    CHECK(first->compare_exchange(&cells[0].value, 4u, 7u, 9u)
          == 0x2468ace0u);
    CHECK(compare_calls == 1u);
    CHECK(last_address == &cells[0].value && last_width == 4u);
    CHECK(last_expected == 7u && last_desired == 9u);
    CHECK(p4_atomic_public_service(&exec_base) == first);
    CHECK(open_calls == 1u && attr_calls == 1u);
}

enum { THREAD_COUNT = 8 };
static const struct KrnAtomicOps *thread_results[THREAD_COUNT];
static void *lookup_thread(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    thread_results[index] = p4_atomic_public_service(&exec_base);
    return NULL;
}

static void test_concurrent_first_callers(void)
{
    pthread_t threads[THREAD_COUNT];
    unsigned index;
    cache_load_target = THREAD_COUNT;
    for (index = 0; index < THREAD_COUNT; index++)
        CHECK(pthread_create(&threads[index], NULL, lookup_thread,
                             (void *)(uintptr_t)index) == 0);
    for (index = 0; index < THREAD_COUNT; index++)
        CHECK(pthread_join(threads[index], NULL) == 0);
    for (index = 0; index < THREAD_COUNT; index++)
        CHECK(thread_results[index] == &active_table);
    CHECK(open_calls == THREAD_COUNT);
    CHECK(attr_calls == THREAD_COUNT);
    CHECK(p4_atomic_public_service(&exec_base) == &active_table);
    CHECK(open_calls == THREAD_COUNT && attr_calls == THREAD_COUNT);
}

static void test_reentrant_first_lookup(void)
{
    const struct KrnAtomicOps *ops;
    reenter_open_once = 1;
    ops = p4_atomic_public_service(&exec_base);
    CHECK(ops == &active_table);
    CHECK(reenter_open_once == 0);
    CHECK(max_open_depth == 2u);
    CHECK(open_calls == 2u && attr_calls == 2u);
    CHECK(p4_atomic_public_service(&exec_base) == &active_table);
    CHECK(open_calls == 2u && attr_calls == 2u);
}

static void test_macro_evaluation_once(void)
{
    __AROS_ATOMIC_AND_L(next_cell()->value, next_operand());
    CHECK(address_evaluations == 1u);
    CHECK(operand_evaluations == 1u);
    CHECK(operate_calls == 1u);
    CHECK(last_address == &cells[0].value);
    CHECK(last_width == 4u);
    CHECK(last_operation == KRN_ATOMIC_OP_AND);
    CHECK(last_operand == 0x00c0ffeeu);
    CHECK(open_calls == 1u && attr_calls == 1u);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (strcmp(argv[1], "cold-hot-valid") == 0) mode = MODE_VALID;
    else if (strcmp(argv[1], "null-kernel") == 0) mode = MODE_NULL_KERNEL;
    else if (strcmp(argv[1], "unsupported") == 0) mode = MODE_UNSUPPORTED;
    else if (strcmp(argv[1], "zero-result") == 0) mode = MODE_ZERO;
    else if (strcmp(argv[1], "minus-one-result") == 0) mode = MODE_MINUS_ONE;
    else if (strcmp(argv[1], "wrong-version") == 0) mode = MODE_WRONG_VERSION;
    else if (strcmp(argv[1], "short-size") == 0) mode = MODE_SHORT_SIZE;
    else if (strcmp(argv[1], "null-operate") == 0) mode = MODE_NULL_OPERATE;
    else if (strcmp(argv[1], "null-cas") == 0) mode = MODE_NULL_CAS;
    else if (strcmp(argv[1], "concurrent-first-callers") == 0) mode = MODE_CONCURRENT;
    else if (strcmp(argv[1], "reentrant-first-lookup") == 0) mode = MODE_REENTRANT;
    else if (strcmp(argv[1], "macro-evaluation-once") == 0) mode = MODE_MACRO;
    else return 2;

    setup_table();
    if (mode == MODE_NULL_OPERATE)
        active_table.operate = NULL;
    if (mode == MODE_NULL_CAS)
        active_table.compare_exchange = NULL;

    if (mode >= MODE_NULL_KERNEL && mode <= MODE_NULL_CAS)
        expect_fatal();
    else if (mode == MODE_CONCURRENT)
        test_concurrent_first_callers();
    else if (mode == MODE_REENTRANT)
        test_reentrant_first_lookup();
    else if (mode == MODE_MACRO)
        test_macro_evaluation_once();
    else
        test_cold_hot_valid();

    printf("%s: %u checks passed\n", argv[1], checks);
    return 0;
}
'''

cases = (
    "cold-hot-valid",
    "null-kernel",
    "unsupported",
    "zero-result",
    "minus-one-result",
    "wrong-version",
    "short-size",
    "null-operate",
    "null-cas",
    "concurrent-first-callers",
    "reentrant-first-lookup",
    "macro-evaluation-once",
)

with tempfile.TemporaryDirectory(prefix="p4-atomic-public-lookup-") as tmp:
    tmpdir = Path(tmp)
    c_file = tmpdir / "lookup-test.c"
    binary = tmpdir / "lookup-test"
    c_file.write_text(fixture)
    subprocess.run(
        ["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
         "-I", str(repo / "compiler/include"), str(c_file), "-o", str(binary)],
        check=True,
    )
    total = 0
    for case in cases:
        result = subprocess.run([str(binary), case], check=True, capture_output=True,
                                text=True, timeout=5)
        output = result.stdout.strip()
        print(output)
        match = re.search(r": (\d+) checks passed$", output)
        if not match:
            raise RuntimeError(f"missing check count for {case}")
        total += int(match.group(1))

print(f"PASS: {len(cases)} isolated scenarios, {total} ASan/UBSan checks")
print("LIMIT: mocked LVO/re-entry checks do not prove real OpenResource IRQ-lock safety")
