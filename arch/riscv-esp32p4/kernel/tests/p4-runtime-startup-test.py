#!/usr/bin/env python3
"""Exercise the guarded P4 startup metadata helper with host MMIO/CSR mocks."""

import os
import pathlib
import shlex
import subprocess
import sys
import tempfile


GUARD = "#if defined(P4_E3_RUNTIME_PREPARE) && (P4_E3_RUNTIME_PREPARE == 1)"
SIGNATURE = "static int krnP4PrepareExecRuntimeMetadata(void)"


def extract_function(source):
    start = source.index(SIGNATURE)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError("startup helper has an unterminated function body")


def source_contract(source, helper):
    if source.count(GUARD) != 3:
        raise AssertionError("runtime-preparation includes/declaration, helper, and call must be guarded")

    guarded_includes = source[source.index(GUARD):]
    guarded_includes = guarded_includes[:guarded_includes.index("#endif")]
    if '"secondary_hw.h"' not in guarded_includes or \
            '"cpu_local_runtime.h"' not in guarded_includes:
        raise AssertionError("runtime-only headers escaped their opt-in guard")

    prepare = source.index("if (!krnPrepareExecBase(")
    call = source.index("if (!krnP4PrepareExecRuntimeMetadata())")
    singletask = source.index("InitCode(RTF_SINGLETASK, 0);")
    coldstart = source.index("InitCode(RTF_COLDSTART, 0);")
    if not prepare < singletask < call < coldstart:
        raise AssertionError("runtime metadata must bind after singletask residents and before coldstart")

    kernel_init = (pathlib.Path(__file__).resolve().parents[4] /
                   "rom/kernel/kernel_init.c").read_text()
    resident_start = kernel_init.index("const struct Resident Kernel_resident")
    resident_end = kernel_init.index("static const UBYTE version[]", resident_start)
    resident = kernel_init[resident_start:resident_end]
    if "RTF_SINGLETASK" not in resident or "Kernel_Init" not in resident:
        raise AssertionError("Kernel_Init is no longer guaranteed to run in the singletask pass")
    init_body = kernel_init[kernel_init.index("AROS_UFH3S(struct KernelBase *, Kernel_Init"):]
    init_order = ["set_call_libfuncs(SETNAME(INITLIB)", "setKernelBase(KernelBase)",
                  "return KernelBase;"]
    init_positions = [init_body.index(token) for token in init_order]
    if init_positions != sorted(init_positions):
        raise AssertionError("KernelBase global is not set before Kernel_Init returns")

    order = [
        '"csrrci %0, mstatus, 8"',
        "P4_CPU_LOCAL_READ_MHARTID() == 0",
        "P4_SECONDARY_RESET_REG",
        "P4_SECONDARY_CLOCK_REG",
        "krnP4CpuLocalPrepare()",
        "krnP4ExecRuntimePrepare(sys_base, kernel_base)",
        "krnP4ExecRuntimeBind(sys_base, kernel_base)",
    ]
    positions = [helper.index(token) for token in order]
    if positions != sorted(positions):
        raise AssertionError("runtime setup order changed or no longer checks hart isolation first")

    bind = helper.index("krnP4ExecRuntimeBind(sys_base, kernel_base)")
    if helper.index("P4_SECONDARY_RESET_REG", bind) < bind or \
            helper.index("P4_SECONDARY_CLOCK_REG", bind) < bind:
        raise AssertionError("hart1 isolation is not checked again after binding")

    if "P4_SECONDARY_RESET_REG) =" in helper or \
            "P4_SECONDARY_CLOCK_REG) =" in helper:
        raise AssertionError("startup helper must not write reset or clock gates")

    if "if (saved_status & MSTATUS_MIE)" not in helper or \
            '"csrsi mstatus, 8"' not in helper or \
            '"csrci mstatus, 8"' not in helper:
        raise AssertionError("helper must restore the exact prior MIE bit state")

    failure_end = source.index("#endif", call)
    failure_block = source[call:failure_end]
    diagnostic_tokens = [
        "krnP4PrepareExecRuntimeMetadata()",
        "getKernelBase()",
        "P4_CPU_LOCAL_READ_MHARTID()",
        "P4_SECONDARY_RESET_REG",
        "P4_SECONDARY_CLOCK_REG",
        "return;",
    ]
    diagnostic_positions = [failure_block.index(token) for token in diagnostic_tokens]
    if diagnostic_positions != sorted(diagnostic_positions):
        raise AssertionError("failure diagnostic must read bounded metadata after helper MIE restore")
    error_return = source.index('"[smp-e3] runtime metadata prepare FAILED; coldstart withheld; hart "')
    if not call < error_return < coldstart:
        raise AssertionError("failure must stop Exec before cold-start code")

    diagnostic_decl = source.index("int diagnostic_sent;", error_return)
    diagnostic_end = source.index("#endif", diagnostic_decl)
    success_block = source[diagnostic_decl:diagnostic_end]
    success_tokens = [
        "int diagnostic_sent;",
        "Forbid();",
        "diagnostic_sent = krnP4DiagnosticLine(",
        '"[smp-e3] metadata PASS; hart0 bound only; hart1 reset-held; "',
        '"no scheduler/Exec SMP\\n"',
        "Permit();",
        "if (!diagnostic_sent)",
        '"[smp-e3] diagnostic line FAILED; coldstart withheld\\n"',
        "return;",
    ]
    success_positions = [success_block.index(token) for token in success_tokens]
    if success_positions != sorted(success_positions):
        raise AssertionError("PASS must be one bounded whole line under Forbid and gate COLDSTART")
    if not diagnostic_decl < coldstart:
        raise AssertionError("diagnostic transport result must be checked before coldstart")
    if 'krnP4PutStr("[smp-e3] metadata PASS;' in source:
        raise AssertionError("PASS must not use the lossy runtime console")

    diagnostic_prototype = source.index("extern int krnP4DiagnosticLine(const char *line);")
    if not source.index(GUARD) < diagnostic_prototype < source.index("#endif", diagnostic_prototype):
        raise AssertionError("diagnostic API declaration escaped the runtime opt-in guard")

    # The normal build has no runtime include, helper call, or altered macro path.
    if source.count("krnP4PrepareExecRuntimeMetadata") != 2:
        raise AssertionError("runtime helper reference escaped the opt-in definition/call pair")


def host_function(helper):
    replacements = [
        (
            r'__asm__ volatile\("csrrci %0, mstatus, 8"\s*'
            r': "=r"\(saved_status\) : : "memory"\);',
            "saved_status = test_disable_mie();",
        ),
        (r'__asm__ volatile\("csrsi mstatus, 8" ::: "memory"\);',
         "test_restore_mie(1);"),
        (r'__asm__ volatile\("csrci mstatus, 8" ::: "memory"\);',
         "test_restore_mie(0);"),
    ]
    import re

    for pattern, replacement in replacements:
        helper, count = re.subn(pattern, replacement, helper)
        if count != 1:
            raise AssertionError("could not substitute exactly one production MIE operation")
    return helper


HARNESS_PREFIX = r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct ExecBase { unsigned int marker; };
struct KernelBase { unsigned int marker; };
static struct ExecBase sys_object;
static struct KernelBase kernel_object;
static struct ExecBase *SysBase = &sys_object;
static struct KernelBase *mock_kernel_base = &kernel_object;
static struct KernelBase *getKernelBase(void) { return mock_kernel_base; }

static volatile uint32_t reset_register;
static volatile uint32_t clock_register;
#define P4_SECONDARY_RESET_REG ((uintptr_t)&reset_register)
#define P4_SECONDARY_RESET_BIT 0x100u
#define P4_SECONDARY_CLOCK_REG ((uintptr_t)&clock_register)
#define P4_SECONDARY_CLOCK_BIT 0x10u
#define MSTATUS_MIE 0x8u

static uint32_t mock_hartid;
#define P4_CPU_LOCAL_READ_MHARTID() mock_hartid

static unsigned long mie_state;
static unsigned long events[8];
static unsigned int event_count;
static int local_result = 1;
static int runtime_result = 1;
static int bind_result = 1;
static int change_mie_in_bind = -1;
static int raise_clock_in_bind;

static void event(unsigned long value)
{
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

static unsigned long test_disable_mie(void)
{
    unsigned long previous = mie_state;
    event(1);
    mie_state = 0;
    return previous;
}

static void test_restore_mie(int enabled)
{
    event(enabled ? 5 : 6);
    mie_state = enabled ? MSTATUS_MIE : 0;
}

static int krnP4CpuLocalPrepare(void)
{
    assert(mie_state == 0);
    event(2);
    return local_result;
}

static int krnP4ExecRuntimePrepare(struct ExecBase *sys_base,
                                   struct KernelBase *kernel_base)
{
    assert(mie_state == 0);
    assert(sys_base == SysBase && kernel_base == &kernel_object);
    event(3);
    return runtime_result;
}

static int krnP4ExecRuntimeBind(struct ExecBase *sys_base,
                                struct KernelBase *kernel_base)
{
    assert(mie_state == 0);
    assert(sys_base == SysBase && kernel_base == &kernel_object);
    event(4);
    if (change_mie_in_bind >= 0)
        mie_state = (unsigned long)change_mie_in_bind;
    if (raise_clock_in_bind)
        clock_register |= P4_SECONDARY_CLOCK_BIT;
    return bind_result;
}
'''


HARNESS_SUFFIX = r'''
static void reset_case(uint32_t reset, uint32_t clock, unsigned long mie)
{
    reset_register = reset;
    clock_register = clock;
    mie_state = mie;
    SysBase = &sys_object;
    mock_kernel_base = &kernel_object;
    event_count = 0;
    mock_hartid = 0;
    local_result = runtime_result = bind_result = 1;
    change_mie_in_bind = -1;
    raise_clock_in_bind = 0;
}

static void expect_events(const unsigned long *expected, unsigned int count)
{
    unsigned int index;
    assert(event_count == count);
    for (index = 0; index < count; ++index)
        assert(events[index] == expected[index]);
}

int main(void)
{
    static const unsigned long success_on[] = {1, 2, 3, 4, 5};
    static const unsigned long success_off[] = {1, 2, 3, 4, 6};
    static const unsigned long early_fail_on[] = {1, 5};
    static const unsigned long early_fail_off[] = {1, 6};
    static const unsigned long local_fail[] = {1, 2, 6};
    static const unsigned long runtime_fail[] = {1, 2, 3, 6};
    static const unsigned long bind_fail[] = {1, 2, 3, 4, 6};

    reset_case(P4_SECONDARY_RESET_BIT, 0, MSTATUS_MIE);
    change_mie_in_bind = 0;
    assert(krnP4PrepareExecRuntimeMetadata());
    assert(mie_state == MSTATUS_MIE);
    assert(reset_register == P4_SECONDARY_RESET_BIT && clock_register == 0);
    expect_events(success_on, 5);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    change_mie_in_bind = MSTATUS_MIE;
    assert(krnP4PrepareExecRuntimeMetadata());
    assert(mie_state == 0);
    expect_events(success_off, 5);

    reset_case(0, 0, MSTATUS_MIE);
    assert(!krnP4PrepareExecRuntimeMetadata());
    assert(reset_register == 0 && clock_register == 0);
    expect_events(early_fail_on, 2);

    reset_case(P4_SECONDARY_RESET_BIT, P4_SECONDARY_CLOCK_BIT, 0);
    assert(!krnP4PrepareExecRuntimeMetadata());
    assert(reset_register == P4_SECONDARY_RESET_BIT &&
           clock_register == P4_SECONDARY_CLOCK_BIT);
    expect_events(early_fail_off, 2);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    SysBase = NULL;
    assert(!krnP4PrepareExecRuntimeMetadata());
    expect_events(early_fail_off, 2);

    reset_case(P4_SECONDARY_RESET_BIT, 0, MSTATUS_MIE);
    mock_kernel_base = NULL;
    assert(!krnP4PrepareExecRuntimeMetadata());
    assert(mie_state == MSTATUS_MIE);
    expect_events(early_fail_on, 2);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    mock_hartid = 1;
    assert(!krnP4PrepareExecRuntimeMetadata());
    expect_events(early_fail_off, 2);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    local_result = 0;
    assert(!krnP4PrepareExecRuntimeMetadata());
    expect_events(local_fail, 3);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    runtime_result = 0;
    assert(!krnP4PrepareExecRuntimeMetadata());
    expect_events(runtime_fail, 4);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    bind_result = 0;
    assert(!krnP4PrepareExecRuntimeMetadata());
    expect_events(bind_fail, 5);

    reset_case(P4_SECONDARY_RESET_BIT, 0, 0);
    raise_clock_in_bind = 1;
    assert(!krnP4PrepareExecRuntimeMetadata());
    assert(clock_register == P4_SECONDARY_CLOCK_BIT);
    expect_events(bind_fail, 5);

    puts("PASS P4 guarded runtime startup source fixture");
    return 0;
}
'''


def main():
    kernel = pathlib.Path(__file__).resolve().parents[1]
    source = (kernel / "kernel_startup.c").read_text()
    helper = extract_function(source)
    source_contract(source, helper)
    harness = HARNESS_PREFIX + host_function(helper) + HARNESS_SUFFIX
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise AssertionError("CC selected no host compiler")

    with tempfile.TemporaryDirectory(prefix="p4-runtime-startup-") as directory:
        c_file = pathlib.Path(directory) / "runtime-startup-test.c"
        executable = pathlib.Path(directory) / "runtime-startup-test"
        c_file.write_text(harness)
        subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                   str(c_file), "-o", str(executable)],
                       check=True)
        subprocess.run([str(executable)], check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print("FAIL:", error, file=sys.stderr)
        raise SystemExit(1)
