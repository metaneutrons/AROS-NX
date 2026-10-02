"""Pure E3 log-parser tests; no hardware or serial access is required."""

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "smp-e3-check.py"
SPEC = importlib.util.spec_from_file_location("smp_e3_check", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)
E2 = CHECKER.E2_CHECKER


SUPPRESSED = (
    b"[smp] suppressed state=0x00000000 hart=0x00000000 "
    b"echo=0x00000000 cause=0x00000000 pc=0x00000000 "
    b"result=0x00000000 reset=held clock=off"
)
MISA_REPORT = b"[smp-e2] misa0=0x40901125 misa1=0x40901125"
RETRIES_REPORT = b"[smp-e2] concurrent retries0=0x00000004 retries1=0x00000002"
CPU_LOCAL_PASS = CHECKER.CPU_LOCAL_PASS_LINE


def release(echo):
    return (
        b"[smp] release state=0x00000001 hart=0x00000001 echo=" + echo
        + b" cause=0x00000000 pc=0x00000000 result=0x00000001 "
        b"reset=held clock=off"
    )


def good_log(newline=b"\r\n"):
    lines = [SUPPRESSED]
    for echo in (b"0xe1000002", b"0xe2000003"):
        lines.extend((
            E2.STAGE_LINES["psram"],
            MISA_REPORT,
            RETRIES_REPORT,
            E2.STAGE_LINES["atomics"],
            CPU_LOCAL_PASS,
            E2.STAGE_LINES["ipi_fence"],
            E2.STAGE_LINES["cache"],
            release(echo),
        ))
    lines.extend((
        b"[smp-e2] PRIMITIVES PASS; epochs=2 secondary stopped",
        b"[smp-e2] READY; primary parked before Exec",
    ))
    return newline.join(lines) + newline


class E3LogCheckTests(unittest.TestCase):
    def soft_atomic_log(self, newline=b"\r\n"):
        spin = CHECKER.SPINLOCK_PASS_LINE
        return self.spin_log(newline).replace(
            spin + newline,
            spin + newline + CHECKER.SOFT_ATOMIC_PASS_LINE + newline,
        )

    def spin_log(self, newline=b"\r\n"):
        return good_log(newline).replace(
            CPU_LOCAL_PASS,
            CPU_LOCAL_PASS + newline + CHECKER.SPINLOCK_PASS_LINE,
        )

    def test_spinlock_campaign_passes(self):
        self.assertTrue(CHECKER.analyze_log(self.spin_log(), True)["passed"])

    def test_soft_atomic_campaign_passes_after_spinlock_per_epoch(self):
        result = CHECKER.analyze_log(self.soft_atomic_log(), True, True)
        self.assertTrue(result["passed"])
        self.assertTrue(all(result["criteria"].values()))
        self.assertEqual(result["counts"]["soft_atomic_records"], 2)
        self.assertEqual(result["counts"]["soft_atomic_pass_reports"], 2)
        self.assertEqual(result["counts"]["soft_atomic_malformed_reports"], 0)

    def test_soft_atomic_requires_spinlock_and_does_not_change_old_path(self):
        old_log = self.spin_log()
        old_result = CHECKER.analyze_log(old_log, True)
        self.assertTrue(old_result["passed"])
        self.assertNotIn("two_soft_atomic_passes", old_result["criteria"])
        self.assertNotIn("soft_atomic_records", old_result["counts"])
        self.assertFalse(CHECKER.analyze_log(old_log, True, True)["passed"])
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "soft.log"
            log.write_bytes(self.soft_atomic_log())
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    CHECKER.main(["--soft-atomic", str(log)])
            self.assertEqual(raised.exception.code, 2)

    def test_soft_atomic_missing_duplicate_failure_and_malformed_records_fail(self):
        line = CHECKER.SOFT_ATOMIC_PASS_LINE
        valid = self.soft_atomic_log()
        cases = (
            valid.replace(line + b"\r\n", b"", 1),
            valid.replace(line, line + b"\r\n" + line, 1),
            valid + b"[smp-e3] SOFT-ATOMIC FAIL; timeout\r\n",
            valid.replace(line, line.replace(b"refusal=ok", b"refusal=o"), 1),
            valid + b"[smp-e3] SOFT-ATOMIC PASS; incomplete\r\n",
        )
        for data in cases:
            with self.subTest(data=data[-180:]):
                result = CHECKER.analyze_log(data, True, True)
                self.assertFalse(result["passed"])
                self.assertFalse(
                    result["criteria"]["no_soft_atomic_failure_or_malformed_record"]
                    and result["criteria"]["two_soft_atomic_passes"]
                )

    def test_soft_atomic_records_must_follow_spinlock_and_precede_ipi_each_epoch(self):
        spin = CHECKER.SPINLOCK_PASS_LINE
        soft = CHECKER.SOFT_ATOMIC_PASS_LINE
        ipi = E2.STAGE_LINES["ipi_fence"]
        valid = self.soft_atomic_log()
        before_spin = valid.replace(
            spin + b"\r\n" + soft + b"\r\n",
            soft + b"\r\n" + spin + b"\r\n",
            1,
        )
        after_ipi = valid.replace(
            soft + b"\r\n" + ipi,
            ipi + b"\r\n" + soft,
            1,
        )
        both_in_first_epoch = valid.replace(
            spin + b"\r\n" + soft + b"\r\n" + ipi,
            spin + b"\r\n" + soft + b"\r\n" + soft + b"\r\n" + ipi,
            1,
        )
        second_soft = both_in_first_epoch.rfind(soft + b"\r\n")
        both_in_first_epoch = (
            both_in_first_epoch[:second_soft]
            + both_in_first_epoch[second_soft + len(soft + b"\r\n"):]
        )
        for data in (before_spin, after_ipi, both_in_first_epoch):
            with self.subTest(data=data[:220]):
                result = CHECKER.analyze_log(data, True, True)
                self.assertFalse(
                    result["criteria"][
                        "soft_atomic_pass_once_after_spinlock_before_ipi_per_epoch"
                    ]
                )
                self.assertFalse(result["passed"])

    def test_soft_atomic_accepts_lf_records(self):
        result = CHECKER.analyze_log(self.soft_atomic_log(b"\n"), True, True)
        self.assertTrue(result["passed"])

    def test_cpu_local_only_does_not_qualify_spinlock(self):
        self.assertFalse(CHECKER.analyze_log(good_log(), True)["passed"])

    def test_spinlock_missing_duplicate_and_failure_rejected(self):
        line = CHECKER.SPINLOCK_PASS_LINE
        for data in (self.spin_log().replace(line + b"\r\n", b"", 1),
                     self.spin_log().replace(line, line + b"\r\n" + line, 1),
                     self.spin_log() + b"[smp-e3] SPINLOCK FAIL; timeout\r\n"):
            self.assertFalse(CHECKER.analyze_log(data, True)["passed"])

    def test_spinlock_wrong_counts_or_before_cpu_local_rejected(self):
        line = CHECKER.SPINLOCK_PASS_LINE
        for data in (self.spin_log().replace(b"writes=8192", b"writes=4096", 1),
                     self.spin_log().replace(CPU_LOCAL_PASS + b"\r\n" + line,
                                             line + b"\r\n" + CPU_LOCAL_PASS, 1)):
            self.assertFalse(CHECKER.analyze_log(data, True)["passed"])

    def test_complete_e3_log_passes_with_crlf(self):
        result = CHECKER.analyze_log(good_log())
        self.assertTrue(result["passed"])
        self.assertTrue(all(result["criteria"].values()))
        self.assertEqual(result["counts"]["cpu_local_pass_reports"], 2)
        self.assertEqual(result["counts"]["cpu_local_fail_reports"], 0)

    def test_complete_log_accepts_lf(self):
        self.assertTrue(CHECKER.analyze_log(good_log(b"\n"))["passed"])

    def test_ordinary_e2_log_without_e3_markers_does_not_qualify(self):
        e2_only = b"\r\n".join(
            line for line in good_log().split(b"\r\n")
            if line and line != CPU_LOCAL_PASS
        )
        e2_result = E2.analyze_log(e2_only)
        self.assertTrue(e2_result["passed"])
        result = CHECKER.analyze_log(e2_only)
        self.assertFalse(result["criteria"]["two_cpu_local_passes"])
        self.assertFalse(result["passed"])

    def test_missing_cpu_local_marker_fails(self):
        data = good_log().replace(CPU_LOCAL_PASS + b"\r\n", b"", 1)
        result = CHECKER.analyze_log(data)
        self.assertFalse(result["criteria"]["two_cpu_local_passes"])
        self.assertFalse(result["passed"])

    def test_duplicate_cpu_local_marker_fails(self):
        data = good_log().replace(
            E2.STAGE_LINES["atomics"] + b"\r\n" + CPU_LOCAL_PASS + b"\r\n",
            E2.STAGE_LINES["atomics"] + b"\r\n" + CPU_LOCAL_PASS + b"\r\n"
            + CPU_LOCAL_PASS + b"\r\n",
            1,
        )
        result = CHECKER.analyze_log(data)
        self.assertEqual(result["counts"]["cpu_local_pass_reports"], 3)
        self.assertFalse(result["criteria"]["two_cpu_local_passes"])

    def test_cpu_local_marker_must_be_after_atomics_and_before_ipi_each_epoch(self):
        data = good_log().replace(
            E2.STAGE_LINES["atomics"] + b"\r\n" + CPU_LOCAL_PASS + b"\r\n"
            + E2.STAGE_LINES["ipi_fence"],
            E2.STAGE_LINES["atomics"] + b"\r\n" + E2.STAGE_LINES["ipi_fence"]
            + b"\r\n" + CPU_LOCAL_PASS,
            1,
        )
        result = CHECKER.analyze_log(data)
        self.assertFalse(result["criteria"]["cpu_local_pass_once_between_atomics_and_ipi_per_epoch"])
        self.assertFalse(result["passed"])

    def test_wrong_iterations_or_harts_fail(self):
        bad_iteration = good_log().replace(CPU_LOCAL_PASS, CPU_LOCAL_PASS.replace(b"8192", b"8191"), 1)
        bad_harts = good_log().replace(CPU_LOCAL_PASS, CPU_LOCAL_PASS.replace(b"harts=0,1", b"harts=0"), 1)
        for data in (bad_iteration, bad_harts):
            with self.subTest(data=data[:180]):
                result = CHECKER.analyze_log(data)
                self.assertFalse(result["criteria"]["two_cpu_local_passes"])
                self.assertFalse(result["passed"])

    def test_cpu_local_failure_marker_fails_even_with_pass_tail(self):
        data = b"[smp-e3] CPU-LOCAL FAIL; harts=0,1\r\n" + good_log()
        result = CHECKER.analyze_log(data)
        self.assertEqual(result["counts"]["cpu_local_fail_reports"], 1)
        self.assertFalse(result["criteria"]["no_cpu_local_failure_marker"])
        self.assertFalse(result["passed"])

    def test_e2_failure_marker_invalidates_e3_log(self):
        result = CHECKER.analyze_log(b"[smp-e2] ATOMICS FAIL\r\n" + good_log())
        self.assertFalse(result["criteria"]["e2_log_passes"])
        self.assertFalse(result["passed"])

    def test_extra_malformed_release_line_fails_exact_epoch_count(self):
        data = good_log().replace(
            b"[smp-e2] PRIMITIVES PASS",
            b"[smp] release state=bad\r\n[smp-e2] PRIMITIVES PASS",
        )
        result = CHECKER.analyze_log(data)
        self.assertEqual(result["counts"]["e2_release_lines"], 3)
        self.assertFalse(result["criteria"]["exactly_two_e2_release_epochs"])
        self.assertFalse(result["passed"])

    def test_cli_checks_each_supplied_log_path(self):
        with tempfile.TemporaryDirectory() as directory:
            good = Path(directory) / "good.log"
            bad = Path(directory) / "bad.log"
            good.write_bytes(good_log())
            bad.write_bytes(good_log().replace(CPU_LOCAL_PASS + b"\r\n", b"", 1))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = CHECKER.main([str(good), str(bad)])
            self.assertEqual(status, 1)
            self.assertIn('"passed": true', output.getvalue())
            self.assertIn('"passed": false', output.getvalue())

    def test_cli_soft_atomic_requires_and_uses_both_switches(self):
        with tempfile.TemporaryDirectory() as directory:
            good = Path(directory) / "soft.log"
            good.write_bytes(self.soft_atomic_log())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = CHECKER.main(["--spinlock", "--soft-atomic", str(good)])
            self.assertEqual(status, 0)
            self.assertIn('"soft_atomic_pass_reports": 2', output.getvalue())


if __name__ == "__main__":
    unittest.main()
