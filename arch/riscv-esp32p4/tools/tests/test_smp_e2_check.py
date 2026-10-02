"""Pure log-parser tests; no pyserial or hardware is required."""

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "smp-e2-check.py"
SPEC = importlib.util.spec_from_file_location("smp_e2_check", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)


SUPPRESSED = (
    b"[smp] suppressed state=0x00000000 hart=0x00000000 "
    b"echo=0x00000000 cause=0x00000000 pc=0x00000000 "
    b"result=0x00000000 reset=held clock=off"
)
MISA_REPORT = b"[smp-e2] misa0=0x40901125 misa1=0x40901125"
RETRIES_REPORT = b"[smp-e2] concurrent retries0=0x00000004 retries1=0x00000002"


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
            CHECKER.STAGE_LINES["psram"],
            MISA_REPORT,
            RETRIES_REPORT,
            CHECKER.STAGE_LINES["atomics"],
            CHECKER.STAGE_LINES["ipi_fence"],
            CHECKER.STAGE_LINES["cache"],
        ))
        lines.append(release(echo))
    lines.extend((
        b"[smp-e2] PRIMITIVES PASS; epochs=2 secondary stopped",
        b"[smp-e2] READY; primary parked before Exec",
    ))
    return newline.join(lines) + newline


class E2LogCheckTests(unittest.TestCase):
    def test_complete_two_epoch_log_passes_with_crlf(self):
        result = CHECKER.analyze_log(good_log())
        self.assertTrue(result["passed"])
        self.assertTrue(all(result["criteria"].values()))
        self.assertEqual(result["counts"]["stage_reports"], {
            "psram": 2, "atomics": 2, "ipi_fence": 2, "cache": 2,
        })
        self.assertEqual(result["counts"]["misa_reports"], [
            {"misa0": 0x40901125, "misa1": 0x40901125},
            {"misa0": 0x40901125, "misa1": 0x40901125},
        ])
        self.assertEqual([entry["total"] for entry in result["counts"]["concurrent_retry_reports"]], [6, 6])

    def test_complete_log_accepts_lf(self):
        self.assertTrue(CHECKER.analyze_log(good_log(b"\n"))["passed"])

    def test_missing_stage_fails(self):
        data = good_log().replace(CHECKER.STAGE_LINES["cache"] + b"\r\n", b"", 1)
        result = CHECKER.analyze_log(data)
        self.assertFalse(result["criteria"]["every_primitive_stage_once_per_epoch"])
        self.assertFalse(result["passed"])

    def test_duplicate_stage_fails(self):
        data = good_log().replace(
            CHECKER.STAGE_LINES["psram"] + b"\r\n",
            CHECKER.STAGE_LINES["psram"] + b"\r\n"
            + CHECKER.STAGE_LINES["psram"] + b"\r\n",
            1,
        )
        self.assertFalse(CHECKER.analyze_log(data)["passed"])

    def test_misa_reports_must_be_present_and_have_a_extension_on_both_harts(self):
        missing = good_log().replace(MISA_REPORT + b"\r\n", b"", 1)
        wrong_hart_isa = good_log().replace(
            MISA_REPORT,
            b"[smp-e2] misa0=0x40901125 misa1=0x40901124",
            1,
        )
        wrong_primary_isa = good_log().replace(
            MISA_REPORT,
            b"[smp-e2] misa0=0x40901124 misa1=0x40901125",
            1,
        )
        for data in (missing, wrong_hart_isa, wrong_primary_isa):
            with self.subTest(data=data[:180]):
                result = CHECKER.analyze_log(data)
                self.assertFalse(result["criteria"]["two_misa_reports_with_a_extension"])
                self.assertFalse(result["passed"])

    def test_concurrent_retry_reports_must_be_present_and_nonzero_per_epoch(self):
        missing = good_log().replace(RETRIES_REPORT + b"\r\n", b"", 1)
        zero_total = good_log().replace(
            RETRIES_REPORT,
            b"[smp-e2] concurrent retries0=0x00000000 retries1=0x00000000",
            1,
        )
        for data in (missing, zero_total):
            with self.subTest(data=data[:180]):
                result = CHECKER.analyze_log(data)
                self.assertFalse(result["criteria"]["two_concurrent_retry_reports_with_contention"])
                self.assertFalse(result["passed"])

    def test_wrong_release_status_or_nonce_fails(self):
        for bad in (
            release(b"0xe1000002").replace(b"reset=held", b"reset=clear"),
            release(b"0xe1000002").replace(b"result=0x00000001", b"result=0x00000000"),
            release(b"0xe2000003").replace(b"echo=0xe2000003", b"echo=0xe2000004"),
        ):
            with self.subTest(bad=bad):
                data = good_log().replace(release(b"0xe1000002"), bad, 1)
                self.assertFalse(CHECKER.analyze_log(data)["passed"])

    def test_wrong_suppressed_result_fails(self):
        data = good_log().replace(b"result=0x00000000", b"result=0x00000001", 1)
        self.assertFalse(CHECKER.analyze_log(data)["passed"])

    def test_reordered_stages_fail(self):
        data = good_log().replace(
            CHECKER.STAGE_LINES["psram"] + b"\r\n"
            + MISA_REPORT + b"\r\n",
            MISA_REPORT + b"\r\n"
            + CHECKER.STAGE_LINES["psram"] + b"\r\n",
            1,
        )
        self.assertFalse(CHECKER.analyze_log(data)["criteria"]["two_epochs_in_stage_order"])

    def test_failed_then_passed_is_still_failure(self):
        data = b"[smp-e2] ATOMICS FAIL\r\n" + good_log()
        result = CHECKER.analyze_log(data)
        self.assertFalse(result["criteria"]["no_fail_fault_or_unsafe_marker"])
        self.assertFalse(result["passed"])

    def test_sram_only_log_does_not_qualify(self):
        data = (
            b"[smp-e2] SRAM PASS; epochs=2 exchanges=130\r\n"
            b"[smp-e2] READY; primary parked before Exec\r\n"
        )
        self.assertFalse(CHECKER.analyze_log(data)["passed"])

    def test_fault_and_trap_markers_fail_even_with_pass_tail(self):
        for marker in (b"fault in worker", b"[trap] exception", b"Guru Meditation", b"unsafe secondary"):
            with self.subTest(marker=marker):
                result = CHECKER.analyze_log(marker + b"\r\n" + good_log())
                self.assertFalse(result["criteria"]["no_fail_fault_or_unsafe_marker"])

    def test_missing_or_misordered_final_markers_fail(self):
        missing_ready = good_log().replace(b"[smp-e2] READY; primary parked before Exec\r\n", b"")
        self.assertFalse(CHECKER.analyze_log(missing_ready)["passed"])
        early_ready = good_log().replace(
            b"[smp-e2] PRIMITIVES PASS; epochs=2 secondary stopped\r\n"
            b"[smp-e2] READY; primary parked before Exec\r\n",
            b"[smp-e2] READY; primary parked before Exec\r\n"
            b"[smp-e2] PRIMITIVES PASS; epochs=2 secondary stopped\r\n",
        )
        self.assertFalse(CHECKER.analyze_log(early_ready)["criteria"]["ready_after_final_pass_once"])

    def test_cli_checks_each_supplied_log_path(self):
        with tempfile.TemporaryDirectory() as directory:
            good = Path(directory) / "good.log"
            bad = Path(directory) / "bad.log"
            good.write_bytes(good_log())
            bad.write_bytes(good_log().replace(CHECKER.STAGE_LINES["cache"] + b"\r\n", b"", 1))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = CHECKER.main([str(good), str(bad)])
            self.assertEqual(status, 1)
            self.assertIn('"passed": true', output.getvalue())
            self.assertIn('"passed": false', output.getvalue())


if __name__ == "__main__":
    unittest.main()
