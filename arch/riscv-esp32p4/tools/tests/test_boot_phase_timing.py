"""Host regression tests for the P4 boot timing diagnostic in kernel_timer.c."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


SOURCE = pathlib.Path(__file__).resolve().parents[2] / "kernel" / "kernel_timer.c"


FIXTURE_PREFIX = r'''#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define P4_BOOT_TIMING 1
#define P4_SYSTIMER_HZ 16000000UL
#define P4_ST_CONF 0x00U
#define P4_ST_UNIT0_OP 0x04U
#define P4_ST_UNIT0_VALUE_HI 0x08U
#define P4_ST_UNIT0_VALUE_LO 0x0cU
#define P4_ST_CLK_EN 0x00000001U
#define P4_ST_UNIT0_WORK_EN 0x00000002U
#define P4_ST_UNIT0_UPDATE 0x00000001U
#define P4_ST_UNIT0_VALID 0x00000001U
#define COUNTER_MASK ((UINT64_C(1) << 52) - 1)

enum P4BootPhase {
    P4_BOOT_EARLY_OUTPUT,
    P4_BOOT_PSRAM,
    P4_BOOT_PANEL,
    P4_BOOT_PACKAGE,
    P4_BOOT_EXEC,
    P4_BOOT_UPDATE1,
    P4_BOOT_UPDATE128,
    P4_BOOT_UPDATE256,
    P4_BOOT_PHASES
};

static volatile uint32_t snapshot_lock;
static uint32_t conf_register;
static uint32_t op_status;
static uint64_t counter_value;
static uint64_t latched_value;
static uint64_t counter_advance;
static unsigned int valid_enabled;
static unsigned int update_requests;
static unsigned int op_reads;
static unsigned int conf_writes;
static unsigned int unexpected_reads;
static unsigned int unexpected_writes;
static unsigned int mask_calls;
static unsigned int unmask_calls;
static unsigned long mask_state;
static char console_output[4096];
static size_t console_length;

static void append_console(const char *value)
{
    size_t length = strlen(value);
    assert(console_length + length < sizeof(console_output));
    memcpy(console_output + console_length, value, length + 1U);
    console_length += length;
}

void krnP4PutStr(const char *value)
{
    append_console(value);
}

void krnP4PutC(char value)
{
    char buffer[2] = { value, '\0' };
    append_console(buffer);
}

void krnP4PutDec(uint32_t value)
{
    char buffer[32];
    int written = snprintf(buffer, sizeof(buffer), "%" PRIu32, value);
    assert(written > 0);
    append_console(buffer);
}

void krnP4PutHex32(uint32_t value)
{
    char buffer[32];
    int written = snprintf(buffer, sizeof(buffer), "0x%08" PRIx32, value);
    assert(written > 0);
    append_console(buffer);
}

unsigned long p4_tls_mask(void)
{
    ++mask_calls;
    return mask_state;
}

void p4_tls_unmask(unsigned long state)
{
    ++unmask_calls;
    assert(state == mask_state);
}

static uint32_t st_rd(uint32_t offset)
{
    switch (offset) {
    case P4_ST_CONF:
        return conf_register;
    case P4_ST_UNIT0_OP:
        ++op_reads;
        return op_status;
    case P4_ST_UNIT0_VALUE_HI:
        return (uint32_t)(latched_value >> 32);
    case P4_ST_UNIT0_VALUE_LO:
        return (uint32_t)latched_value;
    default:
        ++unexpected_reads;
        return 0;
    }
}

static void st_wr(uint32_t offset, uint32_t value)
{
    if (offset == P4_ST_CONF) {
        ++conf_writes;
        conf_register = value;
    } else if (offset == P4_ST_UNIT0_OP && value == P4_ST_UNIT0_UPDATE) {
        ++update_requests;
        if (valid_enabled) {
            latched_value = counter_value & COUNTER_MASK;
            counter_value = (counter_value + counter_advance) & COUNTER_MASK;
            op_status = P4_ST_UNIT0_VALID;
        } else {
            op_status = 0;
        }
    } else {
        ++unexpected_writes;
    }
}

static void reset_fixture(void)
{
    unsigned int i;
    snapshot_lock = 0;
    conf_register = 0x40U;
    op_status = 0;
    counter_value = 0;
    latched_value = 0;
    counter_advance = 0;
    valid_enabled = 1;
    update_requests = 0;
    op_reads = 0;
    conf_writes = 0;
    unexpected_reads = 0;
    unexpected_writes = 0;
    mask_calls = 0;
    unmask_calls = 0;
    mask_state = 0x1357UL;
    console_output[0] = '\0';
    console_length = 0;
    boot_origin = 0;
    boot_valid = 0;
    boot_failures = 0;
    boot_updates = 0;
    for (i = 0; i < P4_BOOT_PHASES; ++i)
        boot_phase_ms[i] = UINT32_MAX;
}

static void clear_console(void)
{
    console_output[0] = '\0';
    console_length = 0;
}

static void report_to_buffer(void)
{
    unsigned int i;
    clear_console();
    for (i = 0; i < P4_BOOT_PHASES; ++i)
        krnP4BootTimingReport();
}

static void test_start_initializes_and_uses_progressing_counter(void)
{
    unsigned int i;
    reset_fixture();
    counter_value = 1234;
    counter_advance = 1;
    krnP4BootTimingStart();
    assert(conf_register == (0x40U | P4_ST_CLK_EN | P4_ST_UNIT0_WORK_EN));
    assert(conf_writes == 1);
    assert(boot_origin == 1234);
    assert(boot_valid == 1);
    assert(boot_failures == 0);
    assert(update_requests == 2);
    assert(op_reads == 2);
    assert(snapshot_lock == 0);
    assert(mask_calls == 2 && unmask_calls == 2);
    for (i = 0; i < P4_BOOT_PHASES; ++i)
        assert(boot_phase_ms[i] == UINT32_MAX);
}

static void test_report_rotates_all_slots_and_stays_within_packet(void)
{
    char expected[128];
    unsigned int i;
    reset_fixture();
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    for (i = 0; i < P4_BOOT_PHASES; ++i) {
        counter_value = boot_origin + (uint64_t)(i + 1U) *
                        (P4_SYSTIMER_HZ / 1000U);
        krnP4BootTimingMark((enum P4BootPhase)i);
    }

    for (i = 0; i < P4_BOOT_PHASES; ++i) {
        clear_console();
        krnP4BootTimingReport();
        (void)snprintf(expected, sizeof(expected),
                       "[bt] now=8 fail=0x00000000 p=%u ms=%u\n",
                       i, i + 1U);
        assert(strcmp(console_output, expected) == 0);
        assert(strlen(console_output) <= 64U);
    }

    clear_console();
    krnP4BootTimingReport();
    assert(strstr(console_output, " p=0 ms=1\n") != NULL);
    assert(strlen(console_output) <= 64U);

    for (i = 1; i < P4_BOOT_PHASES - 1U; ++i) {
        clear_console();
        krnP4BootTimingReport();
        (void)snprintf(expected, sizeof(expected), " p=%u ms=%u\n", i, i + 1U);
        assert(strstr(console_output, expected) != NULL);
    }

    boot_failures = UINT32_MAX;
    counter_value = boot_origin + (uint64_t)(UINT32_MAX - 1U) *
                    (P4_SYSTIMER_HZ / 1000U);
    boot_phase_ms[P4_BOOT_PHASES - 1U] = UINT32_MAX - 1U;
    clear_console();
    krnP4BootTimingReport();
    assert(strcmp(console_output,
                  "[bt] now=4294967294 fail=0xffffffff p=7 ms=4294967294\n") == 0);
    assert(strlen(console_output) == 54U);
    assert(strlen(console_output) <= 64U);
}

static void test_stuck_valid_timeout_is_reported(void)
{
    reset_fixture();
    valid_enabled = 0;
    krnP4BootTimingStart();
    assert(boot_valid == 0);
    assert(boot_failures == 1);
    assert(update_requests == 1);
    assert(op_reads == 1000);
    assert(snapshot_lock == 0);
    report_to_buffer();
    assert(strstr(console_output, "now=INVALID") != NULL);
    assert(strstr(console_output, "fail=0x00000001") != NULL);
}

static void test_busy_lock_returns_without_waiting_or_clearing_owner(void)
{
    reset_fixture();
    snapshot_lock = 1;
    krnP4BootTimingStart();
    assert(snapshot_lock == 1);
    assert(update_requests == 0);
    assert(op_reads == 0);
    assert(mask_calls == 1 && unmask_calls == 1);
    assert(boot_failures == 1);
    assert(unexpected_reads == 0 && unexpected_writes == 0);
}

static void test_first_phase_mark_is_immutable(void)
{
    reset_fixture();
    counter_value = 100;
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    counter_value = boot_origin + 9U * (P4_SYSTIMER_HZ / 1000U);
    krnP4BootTimingMark(P4_BOOT_PSRAM);
    counter_value = boot_origin + 13U * (P4_SYSTIMER_HZ / 1000U);
    krnP4BootTimingMark(P4_BOOT_PSRAM);
    assert(boot_phase_ms[P4_BOOT_PSRAM] == 9);
    assert(boot_failures == 0);
    report_to_buffer();
    assert(strstr(console_output, "p=1 ms=9") != NULL);
}

static void test_counter_wrap_uses_52_bit_delta(void)
{
    reset_fixture();
    counter_value = (UINT64_C(1) << 52) - (P4_SYSTIMER_HZ / 1000U);
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    counter_value = P4_SYSTIMER_HZ / 1000U;
    krnP4BootTimingMark(P4_BOOT_PANEL);
    assert(boot_phase_ms[P4_BOOT_PANEL] == 2);
    assert(boot_failures == 0);
}

static void test_millisecond_value_at_uint32_limit_is_rejected(void)
{
    uint64_t rejected_delta = (uint64_t)UINT32_MAX *
                              (P4_SYSTIMER_HZ / 1000U);
    reset_fixture();
    counter_value = 17;
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    counter_value = (boot_origin + rejected_delta) & COUNTER_MASK;
    krnP4BootTimingMark(P4_BOOT_EXEC);
    assert(boot_phase_ms[P4_BOOT_EXEC] == UINT32_MAX);
    assert(boot_failures == (1U << (P4_BOOT_EXEC + 1)));
    report_to_buffer();
    assert(strstr(console_output, "now=INVALID") != NULL);
    assert(strstr(console_output, "fail=0x00000020") != NULL);
    assert(strstr(console_output, "p=4 ms=MISSING") != NULL);
}

static void test_update_milestones_are_counted_once_and_capped(void)
{
    unsigned int i;
    reset_fixture();
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    counter_value = boot_origin + P4_SYSTIMER_HZ / 1000U;
    krnP4BootTimingUpdate();
    assert(boot_updates == 1);
    assert(boot_phase_ms[P4_BOOT_UPDATE1] == 1);
    for (i = 0; i < 126; ++i)
        krnP4BootTimingUpdate();
    assert(boot_updates == 127);
    assert(boot_phase_ms[P4_BOOT_UPDATE128] == UINT32_MAX);
    counter_value = boot_origin + 2U * (P4_SYSTIMER_HZ / 1000U);
    krnP4BootTimingUpdate();
    assert(boot_updates == 128);
    assert(boot_phase_ms[P4_BOOT_UPDATE128] == 2);
    for (i = 0; i < 127; ++i)
        krnP4BootTimingUpdate();
    assert(boot_updates == 255);
    counter_value = boot_origin + 3U * (P4_SYSTIMER_HZ / 1000U);
    krnP4BootTimingUpdate();
    assert(boot_updates == 256);
    assert(boot_phase_ms[P4_BOOT_UPDATE256] == 3);
    for (i = 0; i < 20; ++i)
        krnP4BootTimingUpdate();
    assert(boot_updates == 256);
    assert(boot_phase_ms[P4_BOOT_UPDATE1] == 1);
    assert(boot_phase_ms[P4_BOOT_UPDATE128] == 2);
    assert(boot_phase_ms[P4_BOOT_UPDATE256] == 3);
}

static void test_report_distinguishes_unavailable_from_zero(void)
{
    reset_fixture();
    valid_enabled = 0;
    krnP4BootTimingStart();
    report_to_buffer();
    assert(strstr(console_output, "now=INVALID") != NULL);
    assert(strstr(console_output, "p=0 ms=MISSING") != NULL);

    reset_fixture();
    counter_advance = 1;
    krnP4BootTimingStart();
    counter_advance = 0;
    counter_value = boot_origin;
    krnP4BootTimingMark(P4_BOOT_EARLY_OUTPUT);
    report_to_buffer();
    assert(strstr(console_output, "now=0") != NULL);
    assert(strstr(console_output, "p=0 ms=0") != NULL);
    assert(strstr(console_output, "INVALID") == NULL);
}
'''


FIXTURE_SUFFIX = r'''
int main(void)
{
    test_start_initializes_and_uses_progressing_counter();
    test_report_rotates_all_slots_and_stays_within_packet();
    test_stuck_valid_timeout_is_reported();
    test_busy_lock_returns_without_waiting_or_clearing_owner();
    test_first_phase_mark_is_immutable();
    test_counter_wrap_uses_52_bit_delta();
    test_millisecond_value_at_uint32_limit_is_rejected();
    test_update_milestones_are_counted_once_and_capped();
    test_report_distinguishes_unavailable_from_zero();
    assert(unexpected_reads == 0 && unexpected_writes == 0);
    puts("P4 boot timing diagnostic fixture: PASS");
    return 0;
}
'''


def extract_diagnostic_block(source_text):
    lock = source_text.index("static volatile uint32_t snapshot_lock;")
    start = source_text.index("#ifdef P4_BOOT_TIMING", lock)
    end = source_text.index("#endif", start) + len("#endif")
    return source_text[start:end]


class BootPhaseTimingHostTest(unittest.TestCase):
    def test_actual_diagnostic_block_with_strict_host_c_fixture(self):
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            self.skipTest("a host C compiler is required for this regression test")

        source_text = SOURCE.read_text(encoding="utf-8")
        diagnostic = extract_diagnostic_block(source_text)
        helpers_at = FIXTURE_PREFIX.index("static void reset_fixture(void)")
        fixture = (
            FIXTURE_PREFIX[:helpers_at]
            + diagnostic
            + "\n"
            + FIXTURE_PREFIX[helpers_at:]
            + FIXTURE_SUFFIX
        )

        with tempfile.TemporaryDirectory(prefix="p4-boot-timing-") as directory:
            fixture_path = pathlib.Path(directory) / "boot_timing_fixture.c"
            executable_path = pathlib.Path(directory) / "boot_timing_fixture"
            fixture_path.write_text(fixture, encoding="utf-8")
            compiled = subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    str(fixture_path),
                    "-o",
                    str(executable_path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                f"strict C fixture compilation failed:\n{compiled.stdout}{compiled.stderr}",
            )
            executed = subprocess.run(
                [str(executable_path)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                executed.returncode,
                0,
                f"C fixture assertions failed:\n{executed.stdout}{executed.stderr}",
            )
            self.assertIn("P4 boot timing diagnostic fixture: PASS", executed.stdout)


if __name__ == "__main__":
    unittest.main()
