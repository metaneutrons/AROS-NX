#!/usr/bin/env python3
"""Compile/execute actual P4 cleanup-delivery C snippets with host stubs.

This checks the live dispatcher call/remask statements and the live SMP outer
trap guard/body. It is not a trap emulator or hardware qualification.
"""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[4]
KERNEL = ROOT / "arch/riscv-esp32p4/kernel"


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AssertionError(message)


def smp_arm(source: str) -> str:
    """Keep only the __AROSEXEC_SMP__ arms in the extracted trap block."""
    out: list[str] = []
    # Entries are parent-active, condition, and whether #else was seen.
    stack: list[tuple[bool, bool, bool]] = []
    active = True
    for line in source.splitlines(keepends=True):
        directive = line.strip()
        if directive == "#if defined(__AROSEXEC_SMP__)":
            stack.append((active, True, False))
        elif directive == "#else":
            require(bool(stack), "unmatched #else in trap source")
            parent, condition, _ = stack[-1]
            stack[-1] = (parent, condition, True)
            active = parent and not condition
        elif directive == "#endif":
            require(bool(stack), "unmatched #endif in trap source")
            parent, _, _ = stack.pop()
            active = parent
        elif active:
            out.append(line)
    require(not stack, "unterminated SMP branch in trap source")
    return "".join(out)


def close_brace(source: str, start: int) -> int:
    depth = 0
    block_comment = False
    line_comment = False
    string = False
    escaped = False
    pos = start
    while pos < len(source):
        char = source[pos]
        nxt = source[pos + 1] if pos + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and nxt == "/":
                block_comment = False
                pos += 1
        elif string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                string = False
        elif char == "/" and nxt == "/":
            line_comment = True
            pos += 1
        elif char == "/" and nxt == "*":
            block_comment = True
            pos += 1
        elif char == '"':
            string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return pos
        pos += 1
    raise AssertionError("unclosed trap delivery block")


def extract_trap(source: str) -> str:
    after_dispatch = source.index(
        "action = krnTrapDispatch(ctx, mcause, mtval);"
    )
    start = source.index("if (SysBase && (", after_dispatch)
    opening = source.index("{", start)
    raw_block = source[start : close_brace(source, opening) + 1]
    block = smp_arm(raw_block)
    require("local->trap_depth == 1" in block,
            "actual SMP outer-depth condition absent")
    require(re.search(
        r"if\s*\(\s*IDNESTCOUNT_GET\s*<\s*0\s*\)\s*\{\s*"
        r"Exec_P4DeliverCleanupWake\(SysBase\);\s*"
        r"csr_clear\(mstatus,\s*MSTATUS_MIE\);\s*\}", block,
    ) is not None, "actual outer trap call/remask order absent")
    require("hart_id == 0" not in block and "hart_id != 0" not in block,
            "actual outer trap delivery is hart0-filtered")
    return block


def extract_dispatch(source: str) -> tuple[str, str]:
    pair = re.compile(
        r"Exec_P4DeliverCleanupWake\(SysBase\);\s*"
        r"csr_clear\(mstatus,\s*MSTATUS_MIE\);"
    )
    sites = list(pair.finditer(source))
    require(len(sites) == 2,
            f"expected dispatch and post-WFI delivery sites, found {len(sites)}")
    dispatch = re.search(
        r"IDNESTCOUNT_SET\(-1\);\s*"
        r"Exec_P4DeliverCleanupWake\(SysBase\);\s*"
        r"csr_clear\(mstatus,\s*MSTATUS_MIE\);", source,
    )
    require(dispatch is not None, "actual dispatcher call/remask order absent")
    idle = next(site for site in sites
                if not dispatch.start() <= site.start() < dispatch.end())
    prefix = source[max(0, idle.start() - 220):idle.start()]
    require('asm volatile("wfi")' in prefix and
            "csr_clear(mstatus, MSTATUS_MIE);" in prefix,
            "actual idle site is not reached after WFI/MIE clear")
    dispatch_prefix = source[source.index("dispatch_again:"):dispatch.end()]
    require("hart_id == 0" not in dispatch_prefix and
            "hart_id != 0" not in dispatch_prefix,
            "actual taskless dispatcher delivery is hart0-filtered")
    return dispatch.group(0), idle.group(0)


HARNESS = r'''#include <stdio.h>
#include <stdlib.h>
struct ExecBase { int unused; };
struct ExceptionContext { int x[16]; };
struct p4_cpu_local { unsigned trap_depth; };
enum { mstatus = 0, MSTATUS_MIE = 8, TRAP_RESCHEDULE = 1,
       TRAP_SYSCALL = 2, TRAP_DONE = 0, CTX_REG_A7 = 14 };
static struct ExecBase base_storage;
static struct ExecBase *SysBase = &base_storage;
static int mock_mie, mock_idnest, mock_hart;
static unsigned events[8], event_count, deliveries, remasks, checks;
#define IDNESTCOUNT_GET (mock_idnest)
#define IDNESTCOUNT_SET(value) do { mock_idnest = (value); } while (0)
static void check(int ok, const char *what)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL %s hart=%d idnest=%d\n", what,
                mock_hart, mock_idnest);
        exit(1);
    }
}
void Exec_P4DeliverCleanupWake(struct ExecBase *base)
{
    check(base == SysBase, "delivery SysBase");
    ++deliveries; events[event_count++] = 1;
    mock_mie = 1; /* model callback return with interrupts enabled */
}
void csr_clear(unsigned long csr, unsigned long mask)
{
    check(csr == mstatus && mask == MSTATUS_MIE, "raw MIE operands");
    ++remasks; events[event_count++] = 2; mock_mie = 0;
}
int krnP4RetiredTrap(struct ExceptionContext *ctx, struct ExecBase *base)
{ check(ctx != NULL && base == SysBase, "retired trap arguments"); return 1; }
void core_ExitInterrupt(struct ExceptionContext *ctx) { (void)ctx; }
void core_SysCall(int code, struct ExceptionContext *ctx)
{ (void)code; (void)ctx; }
static void reset(void) { event_count = deliveries = remasks = 0; }
static void expect_one_delivery(void)
{
    check(deliveries == 1 && remasks == 1, "one delivery and one remask");
    check(event_count == 2 && events[0] == 1 && events[1] == 2,
          "delivery immediately precedes MIE remask");
    check(mock_mie == 0, "delivery boundary exits with MIE clear");
}
static void run_dispatch(void)
{
/*DISPATCH*/
}
static void run_idle(void)
{
/*IDLE*/
}
static void run_trap(struct p4_cpu_local *local, struct ExceptionContext *ctx)
{
    int action = TRAP_DONE;
/*TRAP*/
}
int main(void)
{
    struct ExceptionContext ctx = {{0}};
    struct p4_cpu_local local;
    int hart, idnest;
    unsigned depth;
    for (hart = 0; hart <= 1; ++hart) {
        mock_hart = hart; reset(); mock_mie = 0; mock_idnest = 4;
        run_dispatch(); expect_one_delivery();
        check(mock_idnest == -1, "dispatch sets IDNest before delivery");
        reset(); mock_mie = 0; mock_idnest = -1;
        run_idle(); expect_one_delivery();
    }
    for (hart = 0; hart <= 1; ++hart) {
        mock_hart = hart;
        for (depth = 1; depth <= 2; ++depth) {
            for (idnest = -1; idnest <= 0; ++idnest) {
                local.trap_depth = depth; mock_idnest = idnest;
                mock_mie = 1; reset(); SysBase = &base_storage;
                run_trap(&local, &ctx);
                if (depth == 1 && idnest < 0)
                    expect_one_delivery();
                else {
                    check(deliveries == 0 && remasks == 0 && event_count == 0,
                          "nested/ID-masked trap suppressed");
                    check(mock_mie == 1, "suppressed trap preserves MIE");
                }
            }
        }
    }
    local.trap_depth = 1; mock_idnest = -1; mock_mie = 1; reset();
    SysBase = NULL; run_trap(&local, &ctx);
    check(deliveries == 0 && remasks == 0, "NULL SysBase suppresses delivery");
    printf("PASS p4 cleanup boundary C fixture checks=%u\n", checks);
    return 0;
}
'''


def execute_actual_snippets(dispatch: str, idle: str, trap: str) -> None:
    fixture = (HARNESS.replace("/*DISPATCH*/", dispatch)
               .replace("/*IDLE*/", idle).replace("/*TRAP*/", trap))
    compiler = os.environ.get("CC", "clang")
    with tempfile.TemporaryDirectory(prefix="p4-cleanup-boundaries-") as directory:
        cfile = Path(directory) / "fixture.c"
        binary = Path(directory) / "fixture"
        cfile.write_text(fixture, encoding="utf-8")
        subprocess.run([
            compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wconversion", "-Wshadow", "-Wpedantic", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", str(cfile), "-o", str(binary),
        ], check=True)
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        subprocess.run([str(binary)], check=True, env=env)


def main() -> int:
    cpu = (KERNEL / "kernel_cpu.c").read_text(encoding="utf-8")
    traps = (KERNEL / "kernel_traps.c").read_text(encoding="utf-8")
    dispatch, idle = extract_dispatch(cpu)
    trap = extract_trap(traps)
    execute_actual_snippets(dispatch, idle, trap)
    print("PASS p4_cleanup_boundaries extracted actual kernel_cpu/kernel_traps C")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL p4_cleanup_boundaries: {exc}", file=sys.stderr)
        raise SystemExit(1)
