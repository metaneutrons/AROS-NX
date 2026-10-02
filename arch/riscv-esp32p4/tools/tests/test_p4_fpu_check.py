#!/usr/bin/env python3
"""Saved-log parser tests for p4-fpu-check.py; no hardware interaction."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1]
CHECKER = TOOLS / "p4-fpu-check.py"
SPEC = importlib.util.spec_from_file_location("p4_fpu_check", CHECKER)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


VALID_RECORD = (
    "P4_E3_FPU_PROBE=PASS scope=hart0-context-switch-only "
    "task0_iterations=1200 task0_ticks=3 task0_dispatch_delta=7 "
    "task0_peer_interleaved=1 task0_detail=0 "
    "task1_iterations=1180 task1_ticks=4 task1_dispatch_delta=8 "
    "task1_peer_interleaved=1 task1_detail=0 "
    "smp_concurrency=not-tested"
)


class P4FpuLogTest(unittest.TestCase):
    def assert_invalid(self, text):
        with self.assertRaises(CHECK.ValidationError):
            CHECK.validate_log(text)

    def test_exact_record_and_boot_noise_are_valid(self):
        parsed = CHECK.validate_log("boot banner\r\n" + VALID_RECORD + "\r\n")
        self.assertEqual(parsed, {
            "task0_iterations": 1200,
            "task0_ticks": 3,
            "task0_dispatch_delta": 7,
            "task0_peer_interleaved": 1,
            "task0_detail": 0,
            "task1_iterations": 1180,
            "task1_ticks": 4,
            "task1_dispatch_delta": 8,
            "task1_peer_interleaved": 1,
            "task1_detail": 0,
        })

    def test_cli_validates_each_saved_file_without_visual_claims(self):
        with tempfile.TemporaryDirectory(prefix="p4-fpu-log-") as directory:
            log = Path(directory) / "boot.log"
            log.write_text(VALID_RECORD + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(CHECKER), str(log)],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SMP concurrency not tested", result.stdout)
        self.assertIn("no visual acceptance implied", result.stdout)

    def test_missing_record_rejected(self):
        self.assert_invalid("boot reached shell; no FPU probe record\n")

    def test_fail_marker_rejected_even_with_later_pass(self):
        self.assert_invalid(
            "P4_E3_FPU_PROBE=FAIL reason=worker\n" + VALID_RECORD
        )
        for marker in ("FAIL\n", "FAILED: probe worker stopped\n", "[FAIL]\n"):
            with self.subTest(marker=marker):
                self.assert_invalid(marker + VALID_RECORD)

    def test_incidental_filesystem_lookup_failure_is_not_a_probe_failure(self):
        log = "[Lock] failed, err=205\n" + VALID_RECORD + "\n"
        CHECK.validate_log(log)

    def test_duplicate_pass_records_rejected(self):
        self.assert_invalid(VALID_RECORD + "\n" + VALID_RECORD)

    def test_incomplete_and_duplicate_field_records_rejected(self):
        self.assert_invalid(VALID_RECORD.rsplit(" ", 1)[0])
        self.assert_invalid(VALID_RECORD + " task0_ticks=9")

    def test_trap_fatal_marker_rejected(self):
        for marker in ("[trap] fatal - halting hart", "Alert 80000000",
                       "Guru Meditation", "panic: stopped"):
            with self.subTest(marker=marker):
                self.assert_invalid(marker + "\n" + VALID_RECORD)

    def test_serial_or_noninterleaved_runs_rejected(self):
        for task_index in (0, 1):
            with self.subTest(task_index=task_index):
                self.assert_invalid(VALID_RECORD.replace(
                    "task{}_peer_interleaved=1".format(task_index),
                    "task{}_peer_interleaved=0".format(task_index),
                ))
        self.assert_invalid("serial_run=PASS\n" + VALID_RECORD)

    def test_zero_iterations_ticks_or_dispatches_rejected(self):
        values = {
            "task0_iterations": 1200,
            "task0_ticks": 3,
            "task0_dispatch_delta": 7,
            "task1_iterations": 1180,
            "task1_ticks": 4,
            "task1_dispatch_delta": 8,
        }
        for field, original in values.items():
            with self.subTest(field=field):
                self.assert_invalid(VALID_RECORD.replace(
                    field + "={}".format(original), field + "=0"
                ))

    def test_nonzero_detail_or_wrong_scope_rejected(self):
        for task_index in (0, 1):
            with self.subTest(task_index=task_index):
                self.assert_invalid(VALID_RECORD.replace(
                    "task{}_detail=0".format(task_index),
                    "task{}_detail=34".format(task_index),
                ))
        self.assert_invalid(VALID_RECORD.replace("scope=hart0-context-switch-only",
                                                "scope=smp"))

    def test_uint32_overflow_rejected(self):
        self.assert_invalid(VALID_RECORD.replace(
            "task0_iterations=1200", "task0_iterations=4294967296"
        ))

    def test_smp_must_remain_explicitly_untested(self):
        self.assert_invalid(VALID_RECORD.replace("smp_concurrency=not-tested",
                                                "smp_concurrency=tested"))


if __name__ == "__main__":
    unittest.main()
