"""Hardware-free tests for smp-retained-reset.py."""

import importlib.util
import json
import sys
import tempfile
import unittest
from collections import deque
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "smp-retained-reset.py"
SPEC = importlib.util.spec_from_file_location("smp_retained_reset", SCRIPT)
HARNESS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = HARNESS
SPEC.loader.exec_module(HARNESS)


def boot_log(inherited_reset=0, inherited_clock=0x10, tail=b""):
    return (
        b"ESP-ROM:esp32p4-eco2-20240710\r\n"
        b"rst:0x17 (CHIP_USB_UART_RESET),boot:0x20f (SPI_FAST_FLASH_BOOT)\r\n"
        b"[psram]  chip   32 MB at 200 MHz, vendor 0x0000000d, "
        b"a word written and read back after 2 attempts\r\n"
        b"[psram]  calib  entry 0x00000000 done here, 1 attempt\r\n"
        b"[psram]  calibrated, running at 200 MHz\r\n"
        b"[psram]  command timeouts 0, FSM recoveries 0\r\n"
        b"[psram]  window 0x48000000 mapped, one word per megabyte verified\r\n"
        b"[flash] pkg   loaded 35 modules, reserved 4286924 bytes of PSRAM\r\n"
        + f"[smp] early inherited reset=0x{inherited_reset:08x} "
          f"clock=0x{inherited_clock:08x}\r\n".encode()
        + b"[smp] early isolation reset=0x00000100 clock=0x00000000 "
        b"boot=0x00000000\r\n"
        + HARNESS.SUPPRESSED_LINE + b"\r\n"
        + b"[smp] release state=0x00000001 hart=0x00000001 "
        b"echo=0xe1000002 cause=0x00000000 pc=0x00000000 "
        b"result=0x00000001 reset=clear clock=on\r\n"
        + HARNESS.RETAINED_PASS_LINE + b"\r\n"
        + HARNESS.READY_LINE + b"\r\n"
        + tail
    )


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds

    def advance(self, seconds):
        self.now += seconds


class FakeSerial:
    def __init__(self, responses, clock):
        self.responses = deque(responses)
        self.clock = clock
        self.timeout = 0.1
        self.pending = b""
        self.asserted = False
        self.events = []
        self.closed = False

    def reset_input_buffer(self):
        self.events.append(("reset_input_buffer",))
        self.pending = b""

    def setDTR(self, value):
        self.events.append(("DTR", value))

    def setRTS(self, value):
        self.events.append(("RTS", value))
        if value:
            self.asserted = True
        elif self.asserted:
            self.asserted = False
            self.pending = self.responses.popleft() if self.responses else b""

    def read(self, size):
        if self.pending:
            chunk, self.pending = self.pending[:size], self.pending[size:]
            return chunk
        self.clock.advance(self.timeout)
        return b""

    def close(self):
        self.closed = True


class FailingDeassertSerial(FakeSerial):
    def setRTS(self, value):
        was_asserted = self.asserted
        super().setRTS(value)
        if was_asserted and not value:
            raise OSError("simulated failure after RTS deassert edge")


class RetainedResetParserTests(unittest.TestCase):
    def test_complete_log_passes_and_records_attempt_count(self):
        result = HARNESS.analyze_log(boot_log(), True, 0.35)
        self.assertTrue(result["passed"])
        self.assertEqual(result["counts"]["psram_attempts"], [2])
        self.assertEqual(result["counts"]["bsp_35_module_reports"], 1)
        self.assertTrue(result["active_at_entry"])

    def test_rom_normalization_is_recorded_but_not_a_warm_recovery_failure(self):
        result = HARNESS.analyze_log(
            boot_log(inherited_reset=0x100, inherited_clock=0), True, 0.35
        )
        self.assertTrue(result["passed"])
        self.assertFalse(result["active_at_entry"])
        self.assertEqual(result["inherited_registers"]["reset_register"], 0x100)
        self.assertEqual(result["inherited_registers"]["clock_register"], 0)

    def test_missing_inherited_snapshot_fails(self):
        data = boot_log().replace(b"[smp] early inherited", b"[smp] inherited")
        result = HARNESS.analyze_log(data, True, 0.35)
        self.assertFalse(result["criteria"]["inherited_register_snapshot_present"])
        self.assertFalse(result["passed"])

    def test_bad_early_isolation_bits_or_boot_address_fail(self):
        for replacement in (
            b"reset=0x00000000 clock=0x00000000 boot=0x00000000",
            b"reset=0x00000100 clock=0x00000010 boot=0x00000000",
            b"reset=0x00000100 clock=0x00000000 boot=0x00000001",
        ):
            with self.subTest(replacement=replacement):
                data = boot_log().replace(
                    b"reset=0x00000100 clock=0x00000000 boot=0x00000000",
                    replacement,
                )
                result = HARNESS.analyze_log(data, True, 0.35)
                self.assertFalse(
                    result["criteria"]["early_isolation_confirmed_and_boot_address_cleared"]
                )

    def test_duplicate_banner_and_wrong_reset_mode_fail(self):
        duplicate = boot_log() + b"ESP-ROM:esp32p4-eco2-20240710\r\n"
        result = HARNESS.analyze_log(duplicate, True, 0.35)
        self.assertFalse(result["criteria"]["one_expected_rom_banner"])
        wrong_mode = boot_log().replace(b"boot:0x20f", b"boot:0x20e")
        result = HARNESS.analyze_log(wrong_mode, True, 0.35)
        self.assertFalse(result["criteria"]["usb_reset_and_spi_boot_mode"])

    def test_positive_report_requires_zero_cause_and_pc_and_order(self):
        bad_cause = boot_log().replace(
            b"echo=0xe1000002 cause=0x00000000",
            b"echo=0xe1000002 cause=0x00000001",
        )
        result = HARNESS.analyze_log(bad_cause, True, 0.35)
        self.assertFalse(result["criteria"]["positive_release_report"])
        late_rom = boot_log().replace(b"ESP-ROM:", b"", 1) + b"ESP-ROM:esp32p4-eco2-20240710\r\n"
        result = HARNESS.analyze_log(late_rom, True, 0.35)
        self.assertFalse(result["criteria"]["required_markers_unique_and_ordered"])

    def test_download_fatal_and_exec_console_are_rejected(self):
        cases = (
            (b"waiting for download", "no_download_mode"),
            (b"[smp] probe FAIL", "no_explicit_fatal_marker"),
            (b"[trap] exception", "no_explicit_fatal_marker"),
            (b"Guru Meditation Error", "no_explicit_fatal_marker"),
            (b"unsafe secondary", "no_explicit_fatal_marker"),
            (HARNESS.READY_LINE + b"\r\n[console] runtime output", "no_exec_console_after_retained_ready"),
        )
        for marker, criterion in cases:
            with self.subTest(marker=marker):
                result = HARNESS.analyze_log(boot_log(tail=b"\r\n" + marker), True, 0.35)
                self.assertFalse(result["criteria"][criterion])

    def test_missing_or_short_post_ready_window_fails(self):
        result = HARNESS.analyze_log(boot_log(), True, 0.29)
        self.assertFalse(result["criteria"]["post_ready_observation_at_least_300ms"])
        result = HARNESS.analyze_log(boot_log().replace(HARNESS.READY_LINE, b""), True, 0.35)
        self.assertFalse(result["criteria"]["retained_ready_marker"])

    def test_psram_timeout_and_missing_calibration_fail(self):
        data = boot_log().replace(
            b"command timeouts 0, FSM recoveries 0",
            b"command timeouts 1, FSM recoveries 0",
        ).replace(b"calibrated, running", b"NOT calibrated, running")
        result = HARNESS.analyze_log(data, True, 0.35)
        self.assertFalse(result["criteria"]["psram_command_timeouts_and_fsm_recoveries_zero"])
        self.assertFalse(result["criteria"]["psram_calibrated_running_200mhz"])


class RetainedResetHarnessTests(unittest.TestCase):
    def make_factory(self, responses, clock):
        instances = []

        def factory(port, baudrate, timeout):
            self.assertEqual(port, "fake-port")
            self.assertEqual(baudrate, HARNESS.BAUD_RATE)
            self.assertEqual(timeout, HARNESS.READ_TIMEOUT_SECONDS)
            instance = FakeSerial(responses, clock)
            instances.append(instance)
            return instance

        return factory, instances

    def test_one_open_and_exactly_one_pulse_per_seed_and_warm_boot(self):
        clock = FakeClock()
        factory, instances = self.make_factory([boot_log()] * 3, clock)
        with tempfile.TemporaryDirectory() as directory:
            summary = HARNESS.run_campaign(
                "fake-port", directory, cycles=2, timeout_seconds=1,
                serial_factory=factory, monotonic=clock.monotonic, sleep=clock.sleep,
            )
            self.assertEqual(summary["status"], "warm_recovery_passed")
            self.assertEqual(len(instances), 1)
            serial = instances[0]
            self.assertTrue(serial.closed)
            self.assertEqual(serial.events.count(("RTS", True)), 3)
            self.assertEqual(serial.events.count(("RTS", False)), 3)
            control_lines = [event for event in serial.events if event[0] in {"DTR", "RTS"}]
            self.assertEqual(
                control_lines,
                [("DTR", False), ("RTS", True), ("DTR", False), ("RTS", False)] * 3,
            )
            self.assertEqual(clock.sleeps, [0.1, 0.15] * 3)
            self.assertEqual(summary["transition_counts"]["warm_boots_passed"], 2)
            self.assertEqual(summary["transition_counts"]["active_at_entry_successors"], 2)
            self.assertFalse(summary["acceptance"]["e1_20_warm_reset_recovery_gate_passed"])
            for boot in summary["boots"]:
                raw = Path(directory) / boot["raw_log"]["filename"]
                self.assertEqual(boot["raw_log"]["size_bytes"], raw.stat().st_size)
                self.assertEqual(boot["raw_log"]["sha256"], __import__("hashlib").sha256(raw.read_bytes()).hexdigest())
            disk_summary = json.loads((Path(directory) / "summary.json").read_text())
            self.assertEqual(disk_summary["transition_counts"]["seed_pulses_issued"], 1)
            self.assertEqual(disk_summary["transition_counts"]["warm_pulses_issued"], 2)

    def test_silent_seed_aborts_without_retry_and_preserves_empty_log(self):
        clock = FakeClock()
        factory, instances = self.make_factory([b""], clock)
        with tempfile.TemporaryDirectory() as directory:
            summary = HARNESS.run_campaign(
                "fake-port", directory, cycles=2, timeout_seconds=0.3,
                serial_factory=factory, monotonic=clock.monotonic, sleep=clock.sleep,
            )
            self.assertEqual(summary["status"], "failed")
            self.assertEqual(len(instances), 1)
            serial = instances[0]
            self.assertEqual(serial.events.count(("RTS", True)), 1)
            self.assertEqual(summary["transition_counts"]["seed_pulses_issued"], 1)
            self.assertEqual(summary["transition_counts"]["warm_pulses_issued"], 0)
            self.assertEqual((Path(directory) / "seed.log").read_bytes(), b"")
            self.assertEqual(summary["boots"][0]["raw_log"]["size_bytes"], 0)

    def test_download_aborts_without_substitute_boot(self):
        clock = FakeClock()
        factory, instances = self.make_factory([b"DOWNLOAD\r\n"], clock)
        with tempfile.TemporaryDirectory() as directory:
            summary = HARNESS.run_campaign(
                "fake-port", directory, cycles=1, timeout_seconds=1,
                serial_factory=factory, monotonic=clock.monotonic, sleep=clock.sleep,
            )
            self.assertEqual(summary["status"], "failed")
            self.assertEqual(instances[0].events.count(("RTS", True)), 1)
            self.assertEqual(summary["boots"][0]["raw_log"]["size_bytes"], len(b"DOWNLOAD\r\n"))
            self.assertEqual(summary["transition_counts"]["warm_pulses_issued"], 0)

    def test_incomplete_pulse_stage_is_logged_and_never_counted_as_completed(self):
        clock = FakeClock()
        instances = []

        def factory(port, baudrate, timeout):
            instance = FailingDeassertSerial([boot_log()], clock)
            instances.append(instance)
            return instance

        with tempfile.TemporaryDirectory() as directory:
            summary = HARNESS.run_campaign(
                "fake-port", directory, cycles=1, timeout_seconds=1,
                serial_factory=factory, monotonic=clock.monotonic, sleep=clock.sleep,
            )
            self.assertEqual(summary["status"], "failed")
            pulse = summary["boots"][0]["pulse"]
            self.assertTrue(pulse["attempted"])
            self.assertTrue(pulse["rts_asserted"])
            self.assertFalse(pulse["rts_deasserted"])
            self.assertFalse(pulse["completed"])
            self.assertEqual(summary["transition_counts"]["seed_pulse_attempts"], 1)
            self.assertEqual(summary["transition_counts"]["seed_pulses_completed"], 0)
            self.assertEqual(summary["transition_counts"]["ambiguous_or_incomplete_pulses"], 1)
            self.assertEqual(
                (Path(directory) / "seed.log").read_bytes(), boot_log()
            )

    def test_existing_evidence_is_not_overwritten(self):
        clock = FakeClock()
        factory, instances = self.make_factory([], clock)
        with tempfile.TemporaryDirectory() as directory:
            summary_path = Path(directory) / "summary.json"
            summary_path.write_text("keep", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                HARNESS.run_campaign(
                    "fake-port", directory, cycles=1, serial_factory=factory,
                    monotonic=clock.monotonic, sleep=clock.sleep,
                )
            self.assertEqual(summary_path.read_text(encoding="utf-8"), "keep")
            self.assertEqual(instances, [])


if __name__ == "__main__":
    unittest.main()
