#!/usr/bin/env python3
"""Compile and exercise the guarded quiesce-park block from kernel_startup.c."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
STARTUP = ROOT / "kernel" / "kernel_startup.c"
DIAGNOSTIC = "Reference park requires quiesce and the normal timer-fed watchdog"


def source_block(source: bytes) -> tuple[bytes, int]:
    """Return the complete guarded block, retaining its source #ifdef."""
    lines = source.splitlines(keepends=True)
    starts = [i for i, line in enumerate(lines)
              if line.strip() == b"#ifdef P4_REFERENCE_QUIESCE_PARK"]
    if len(starts) != 1:
        raise ValueError("expected one P4_REFERENCE_QUIESCE_PARK source guard")

    start = starts[0]
    depth = 0
    for end in range(start, len(lines)):
        directive = lines[end].lstrip()
        if directive.startswith((b"#if ", b"#if\t", b"#ifdef ", b"#ifndef ")):
            depth += 1
        elif directive.startswith((b"#endif",)):
            depth -= 1
            if depth == 0:
                block = b"".join(lines[start:end + 1])
                remainder = b"".join(lines[end + 1:])
                if not remainder.lstrip().startswith(b"{\n        struct P4PSRAMInfo"):
                    raise ValueError("guarded block is no longer immediately before P4PSRAMInfo")
                return block, start + 1
            if depth < 0:
                break
    raise ValueError("unterminated P4_REFERENCE_QUIESCE_PARK source guard")


HARNESS_PREFIX = r'''#include <limits.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P4_DMAC_CHEN 0x100UL
#define P4_CLKRST_HP_RST_EN0 0x200UL
#define P4_CLKRST_PERI_CLK_CTRL03 0x300UL
#define P4_DSI_BRG_BASE 0x400UL
#define P4_DSI_BRG_CLK_EN 0x000UL
#define P4_DSI_BRG_CLK_EN_BIT (1UL << 0)
#define P4_DSI_BRG_EN 0x004UL

#define P4_DMAC_CH1_EN (UINT32_C(1) << 0)
#define P4_RST_EN_GDMA (UINT32_C(1) << 1)
#define P4_RST_EN_DSI_BRG (UINT32_C(1) << 2)
#define P4_DSI_DPICLK_EN (UINT32_C(1) << 3)
#define P4_DSI_BRG_DSI_EN (UINT32_C(1) << 4)

enum fault_kind
{
    FAULT_NONE = 0,
    FAULT_CHANNEL,
    FAULT_GDMA_RESET,
    FAULT_DSI_RESET,
    FAULT_DPI_CLOCK,
    FAULT_BRIDGE
};

static jmp_buf g_escape;
static char g_transcript[16384];
static size_t g_transcript_length;
static unsigned int g_wait_calls;
static unsigned int g_successful_waits;
static unsigned int g_mmio_reads;
static unsigned int g_mmio_writes;
static unsigned int g_stop_after_waits;
static unsigned int g_fault_sample;
static unsigned int g_false_wait_call;
static unsigned int g_short_wait_call;
static unsigned int g_bad_wait_call;
static unsigned int g_wrong_wait_requests;
static unsigned long g_ticks;
static enum fault_kind g_fault;
static int g_fault_seen;
static int g_fault_clear_seen;
static int g_gate_write_seen;
static int g_bad_mmio_write;
static int g_wait_false_seen;
static int g_short_tick_seen;
static int g_later_good_wait_seen;
static int g_block_returned;
static unsigned int g_failed_checks;

static void append_text(const char *text, size_t length)
{
    if (length > sizeof(g_transcript) - g_transcript_length - 1U)
    {
        fputs("FAIL: transcript buffer overflow\n", stderr);
        abort();
    }
    memcpy(g_transcript + g_transcript_length, text, length);
    g_transcript_length += length;
    g_transcript[g_transcript_length] = '\0';
}

static void krnP4PutStr(const char *text)
{
    append_text(text, strlen(text));
}

static void krnP4PutC(char value)
{
    append_text(&value, 1U);
}

static void krnP4PutHex32(uint32_t value)
{
    char text[9];
    int length = snprintf(text, sizeof(text), "%08" PRIX32, value);
    if (length < 0 || (size_t)length >= sizeof(text))
    {
        fputs("FAIL: hex formatting failed\n", stderr);
        abort();
    }
    append_text(text, (size_t)length);
}

static void krnP4PutDec(unsigned long value)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%lu", value);
    if (length < 0 || (size_t)length >= sizeof(text))
    {
        fputs("FAIL: decimal formatting failed\n", stderr);
        abort();
    }
    append_text(text, (size_t)length);
}

static uint32_t fault_mask_for(unsigned long address)
{
    switch (g_fault)
    {
        case FAULT_CHANNEL:
            return address == P4_DMAC_CHEN ? P4_DMAC_CH1_EN : UINT32_C(0);
        case FAULT_GDMA_RESET:
            return address == P4_CLKRST_HP_RST_EN0 ? P4_RST_EN_GDMA : UINT32_C(0);
        case FAULT_DSI_RESET:
            return address == P4_CLKRST_HP_RST_EN0 ? P4_RST_EN_DSI_BRG : UINT32_C(0);
        case FAULT_DPI_CLOCK:
            return address == P4_CLKRST_PERI_CLK_CTRL03 ? P4_DSI_DPICLK_EN : UINT32_C(0);
        case FAULT_BRIDGE:
            return address == P4_DSI_BRG_BASE + P4_DSI_BRG_EN
                       ? P4_DSI_BRG_DSI_EN : UINT32_C(0);
        case FAULT_NONE:
        default:
            return UINT32_C(0);
    }
}

static uint32_t p4_r32(unsigned long address)
{
    uint32_t mask = fault_mask_for(address);
    ++g_mmio_reads;
    if (mask == UINT32_C(0))
        return UINT32_C(0);
    if (g_wait_calls == g_fault_sample)
    {
        g_fault_seen = 1;
        return mask;
    }
    if (g_fault_seen && g_wait_calls > g_fault_sample)
        g_fault_clear_seen = 1;
    return UINT32_C(0);
}

static void p4_w32(unsigned long address, unsigned long value)
{
    ++g_mmio_writes;
    if (g_mmio_writes != 1U || g_mmio_reads != 0U ||
        address != P4_DSI_BRG_BASE + P4_DSI_BRG_CLK_EN ||
        value != P4_DSI_BRG_CLK_EN_BIT)
    {
        g_bad_mmio_write = 1;
        return;
    }
    g_gate_write_seen = 1;
}

static unsigned long krnTimerTicks(void)
{
    return g_ticks;
}

static int krnTimerWait(unsigned long ticks)
{
    unsigned long before = g_ticks;
    unsigned long advance = ticks;
    int served = 1;

    ++g_wait_calls;
    if (ticks != 100UL)
        ++g_wrong_wait_requests;
    if (g_wait_calls == g_short_wait_call)
    {
        advance = ticks - 1UL;
        g_short_tick_seen = 1;
    }
    g_ticks += advance;
    if (g_wait_calls == g_false_wait_call)
    {
        served = 0;
        g_wait_false_seen = 1;
    }
    if (served && g_ticks - before >= 100UL)
        ++g_successful_waits;
    if (g_bad_wait_call != 0U && g_wait_calls > g_bad_wait_call &&
        served && g_ticks - before >= 100UL)
        g_later_good_wait_seen = 1;

    if (g_wait_calls >= g_stop_after_waits)
        longjmp(g_escape, 1);
    return served;
}

static void run_extracted_block(void)
{
'''


HARNESS_SUFFIX = r'''}

static unsigned int count_substring(const char *text, const char *needle)
{
    unsigned int count = 0U;
    size_t needle_length = strlen(needle);
    const char *cursor = text;

    while ((cursor = strstr(cursor, needle)) != NULL)
    {
        ++count;
        cursor += needle_length;
    }
    return count;
}

static int park_log_passes_gate(void)
{
    return strstr(g_transcript, " READY\n") != NULL &&
           count_substring(g_transcript, " READY\n") == 1U &&
           strstr(g_transcript, " FAIL\n") == NULL;
}

static void check(int condition, const char *label)
{
    if (condition)
        printf("PASS %s\n", label);
    else
    {
        printf("FAIL %s\nTRANSCRIPT-BEGIN\n%sTRANSCRIPT-END\n", label,
               g_transcript);
        ++g_failed_checks;
    }
}

static void run_fixture(enum fault_kind fault, unsigned int fault_sample,
                        unsigned int false_wait_call,
                        unsigned int short_wait_call,
                        unsigned long initial_ticks,
                        unsigned int stop_after_waits)
{
    g_transcript_length = 0U;
    g_transcript[0] = '\0';
    g_wait_calls = 0U;
    g_successful_waits = 0U;
    g_mmio_reads = 0U;
    g_mmio_writes = 0U;
    g_stop_after_waits = stop_after_waits;
    g_fault_sample = fault_sample;
    g_false_wait_call = false_wait_call;
    g_short_wait_call = short_wait_call;
    g_bad_wait_call = false_wait_call != 0U ? false_wait_call : short_wait_call;
    g_ticks = initial_ticks;
    g_fault = fault;
    g_fault_seen = 0;
    g_fault_clear_seen = 0;
    g_gate_write_seen = 0;
    g_bad_mmio_write = 0;
    g_wait_false_seen = 0;
    g_short_tick_seen = 0;
    g_later_good_wait_seen = 0;
    g_wrong_wait_requests = 0U;
    g_block_returned = 0;

    if (setjmp(g_escape) == 0)
    {
        run_extracted_block();
        g_block_returned = 1;
    }

    check(g_mmio_writes == 1U && g_gate_write_seen,
          "fixture performs exactly one gate-clock write");
    check(!g_bad_mmio_write && g_mmio_reads > 0U,
          "gate-clock write precedes MMIO reads; no other writes occur");
}

static void check_latched_fault(enum fault_kind fault, const char *name)
{
    run_fixture(fault, 5U, 0U, 0U, 0UL, 11U);
    check(!g_block_returned, "longjmp bounds the infinite park loop");
    check(g_fault_seen, name);
    check(g_fault_clear_seen, "transient register condition later clears");
    check(strstr(g_transcript, "intervals=10 FAIL\n") != NULL,
          "latched register failure remains at sample 10");
    check(count_substring(g_transcript, " READY\n") == 0U,
          "latched register failure suppresses READY");
    check(g_wrong_wait_requests == 0U, "park requests exactly 100 ticks");
}

static void check_latched_timer_fault(unsigned int false_wait_call,
                                      unsigned int short_wait_call,
                                      const char *name)
{
    run_fixture(FAULT_NONE, 0U, false_wait_call, short_wait_call, 0UL, 11U);
    check(!g_block_returned, "longjmp bounds the infinite park loop");
    check(false_wait_call != 0U ? g_wait_false_seen : g_short_tick_seen, name);
    check(g_later_good_wait_seen,
          "later successful 100-tick interval does not clear timer failure");
    check(strstr(g_transcript, "intervals=10 FAIL\n") != NULL,
          "latched timer failure remains at sample 10");
    check(count_substring(g_transcript, " READY\n") == 0U,
          "latched timer failure suppresses READY");
    check(g_wrong_wait_requests == 0U, "park requests exactly 100 ticks");
}

int main(void)
{
    printf("MODEL native unsigned long bits=%lu\n",
           (unsigned long)(sizeof(unsigned long) * CHAR_BIT));

    run_fixture(FAULT_NONE, 0U, 0U, 0U, 0UL, 10U);
    check(!g_block_returned, "early fixture exits only through bounded longjmp");
    check(g_wait_calls == 10U && g_successful_waits == 10U,
          "early fixture completes ten successful 100-tick intervals");
    check(strstr(g_transcript, " READY\n") == NULL,
          "no READY before ten successful 100-tick intervals");
    check(strstr(g_transcript, "intervals=9 observing\n") != NULL,
          "sample 9 remains observing before interval 10 completes");
    check(g_wrong_wait_requests == 0U, "park requests exactly 100 ticks");

    run_fixture(FAULT_NONE, 0U, 0U, 0U, 0UL, 11U);
    check(strstr(g_transcript, "intervals=10 READY\n") != NULL &&
          count_substring(g_transcript, " READY\n") == 1U,
          "healthy park reaches READY at sample 10");
    check(park_log_passes_gate(),
          "healthy READY/no-FAIL transcript passes the log gate");
    check(g_successful_waits >= 10U,
          "healthy READY follows at least ten successful intervals");
    check(g_wrong_wait_requests == 0U, "park requests exactly 100 ticks");

    check_latched_fault(FAULT_CHANNEL, "CH1 enabled is detected");
    check_latched_fault(FAULT_GDMA_RESET, "GDMA reset asserted is detected");
    check_latched_fault(FAULT_DSI_RESET, "DSI bridge reset asserted is detected");
    check_latched_fault(FAULT_DPI_CLOCK, "DPI clock enabled is detected");
    check_latched_fault(FAULT_BRIDGE, "DSI bridge enabled is detected");

    run_fixture(FAULT_CHANNEL, 11U, 0U, 0U, 0UL, 13U);
    check(strstr(g_transcript, "intervals=10 READY\n") != NULL,
          "late fixture first reaches READY");
    check(strstr(g_transcript, "intervals=10 FAIL\n") != NULL &&
          strstr(g_transcript, " READY\n") < strstr(g_transcript, " FAIL\n"),
          "post-READY interval 11 fault prints a later FAIL");
    check(g_fault_seen && g_fault_clear_seen,
          "post-READY CH1 fault is transient and later clears");
    check(!park_log_passes_gate(),
          "log gate rejects FAIL even when it follows a prior READY");

    check_latched_timer_fault(5U, 0U, "timer wait false is detected");
    check(g_successful_waits == 10U,
          "timer wait false leaves only ten successful intervals");
    check_latched_timer_fault(0U, 5U, "tick advance below 100 is detected");
    check(g_successful_waits == 10U,
          "short tick advance leaves only ten successful intervals");

    run_fixture(FAULT_NONE, 0U, 0U, 0U, ULONG_MAX - 50UL, 11U);
    check(g_successful_waits == 11U,
          "unsigned tick subtraction accepts an interval across ULONG_MAX");
    check(strstr(g_transcript, "intervals=10 READY\n") != NULL,
          "unsigned tick wrap still reaches READY at sample 10");
    check(g_wrong_wait_requests == 0U, "park requests exactly 100 ticks");

    printf("RESULT %s (%u failed assertion(s))\n",
           g_failed_checks == 0U ? "PASS" : "FAIL", g_failed_checks);
    return g_failed_checks == 0U ? 0 : 1;
}
'''


def run(label: str, command: list[str],
        environment: dict[str, str] | None = None) -> tuple[int | None, str]:
    print(f"RUN {label}: {shlex.join(command)}", flush=True)
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False,
                                env=environment, timeout=60)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        if output:
            print(output, end="" if output.endswith("\n") else "\n")
        print(f"EXIT {label}: timeout after 60 seconds", flush=True)
        return None, output
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    print(f"EXIT {label}: {result.returncode}", flush=True)
    return result.returncode, result.stdout


def main() -> int:
    try:
        source = STARTUP.read_bytes()
        block, first_line = source_block(source)
    except (OSError, ValueError) as error:
        print(f"FAIL source extraction: {error}", flush=True)
        return 1

    if not block.startswith(b"#ifdef P4_REFERENCE_QUIESCE_PARK\n"):
        print("FAIL extracted block lost its P4_REFERENCE_QUIESCE_PARK guard")
        return 1

    print(f"SOURCE_SHA256 {STARTUP.relative_to(REPO)} "
          f"{hashlib.sha256(source).hexdigest()}")
    print(f"GUARDED_BLOCK_SHA256 {hashlib.sha256(block).hexdigest()}")
    print(f"RUNNER_SHA256 {Path(__file__).relative_to(REPO)} "
          f"{hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}")
    print("SOURCE_GUARD retained in the compiled extracted snippet")
    print(f"SOURCE_LINES {first_line}-{first_line + len(block.splitlines()) - 1}")

    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        print("FAIL: CC is empty")
        return 1

    harness = (HARNESS_PREFIX + f'\n#line {first_line} "kernel_startup.c"\n' +
               block.decode("utf-8") + '\n#line 1 "host-harness-epilogue.c"\n' +
               HARNESS_SUFFIX)
    sanitizer_environment = os.environ.copy()
    sanitizer_environment["ASAN_OPTIONS"] = "halt_on_error=1"
    sanitizer_environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    strict_flags = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wsign-conversion"]
    failures = 0

    with tempfile.TemporaryDirectory(prefix="quiesce-park-tests-") as temp:
        temp_path = Path(temp)
        harness_path = temp_path / "quiesce-park-harness.c"
        harness_path.write_text(harness, encoding="utf-8")

        for optimization in ("0", "2"):
            binary = temp_path / f"test-quiesce-park-o{optimization}"
            command = compiler + strict_flags + [
                f"-O{optimization}", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-DP4_REFERENCE_QUIESCE_PARK",
                str(harness_path), "-o", str(binary),
            ]
            status, _ = run(f"native-O{optimization}-compile", command)
            failures += status != 0
            if status == 0:
                status, _ = run(f"native-O{optimization}-test", [str(binary)],
                                sanitizer_environment)
                failures += status != 0

        for conflict in ("P4_NO_QUIESCE", "P4_KEEP_WATCHDOG", "P4_NO_WATCHDOG"):
            object_path = temp_path / f"reject-{conflict}.o"
            command = compiler + strict_flags + [
                "-O0", "-DP4_REFERENCE_QUIESCE_PARK", f"-D{conflict}=1",
                "-c", str(harness_path), "-o", str(object_path),
            ]
            status, output = run(f"reject-{conflict}", command)
            rejected = status not in (None, 0) and DIAGNOSTIC in output
            print(f"RESULT {'PASS' if rejected else 'FAIL'} reject {conflict}: "
                  "guard diagnostic observed" if rejected else
                  f"RESULT FAIL reject {conflict}: expected compile rejection and guard diagnostic",
                  flush=True)
            failures += not rejected

    print(f"RESULT {'PASS' if failures == 0 else 'FAIL'} "
          f"({failures} failed runner step(s))", flush=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
