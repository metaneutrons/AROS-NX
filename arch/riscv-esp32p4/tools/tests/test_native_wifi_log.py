"""Host-only tests for the passive native WiFi diagnostic parser."""

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
TOOL = pathlib.Path(__file__).parents[1] / "check-native-wifi-log.py"
SPEC = importlib.util.spec_from_file_location("native_wifi_log", TOOL)
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


def valid_lines(*, ap_count=3, record_count=None):
    if record_count is None and ap_count is not None:
        record_count = min(ap_count, 8)
    lines = [
        "[WiFi] slot1 probe ok=1 stage=8 cmd=52",
        "[WiFi] error=0 status=0",
        "[WiFi] setup attempted=1 ok=1 stage=9 error=0",
        "[WiFi] setup ioe=03 ready=03 block=512 polls=2",
        "[WiFi] restore ok=1 stage=7 error=0 mutated=1",
        "[WiFi] INIT attempted=1 ok=1 stage=6 error=0",
        "[WiFi] INIT polls=2 raw=00000000 counter=0 consumed=1",
        "[WiFi] INIT preopen=0 ambiguous=0 poisoned=0",
        "[WiFi] INIT chip=0d caps=0d bytes=64",
        "[WiFi] INIT extra frames=0 bytes=0 discarded=0",
        # CLEANUP_OK is enum value1; this profile requires all14 cleanup flags.
        "[WiFi] INIT cleanup ok=1 error=1 flags=00003fff",
        "[WiFi] reply attempted=1 ok=1 stage=8 error=0 polls=1",
        "[WiFi] reply token=00000001 logical=35 bus=512 write=1",
        "[WiFi] reply started=1 complete=1 bytes=512 sent=1 poison=0",
        "[WiFi] WLAN attempted=1f valid=1f written=1f",
        "[WiFi] WLAN init=1 mode=1 start=1 stop=1 deinit=1 cleanup=1",
        "[WiFi] WLAN status init=0 mode=0 start=0 stop=0 deinit=0",
        "[WiFi] WLAN error init=0 mode=0 start=0 stop=0 deinit=0",
        "[WiFi] MAC attempted=1 ok=1 error=0 polls=4",
        "[WiFi] MAC matched=1 status=0 events=3 unmatched=0",
        "[WiFi] MAC valid=1 10:bd:a3:9b:bc:34",
    ]
    if ap_count is not None:
        empty = int(ap_count == 0)
        lines.extend((
            "[WiFi] SCAN attempted=1 ok=1 aps=%d records=%d empty_workaround=%d cleanup=1"
            % (ap_count, record_count, empty),
            "[WiFi] SCAN valid start=1 count=1 records=%d stop=0" % (1 - empty),
            "[WiFi] SCAN status start=0 count=0 records=0 stop=0",
            "[WiFi] SCAN error start=0 count=0 records=0 stop=0 polls=4",
        ))
    # All 24 reports are required. Shared-controller configuration registers
    # 0..5 must restore; status/command/input registers may change dynamically.
    for index in range(24):
        before = 0x1000 + index
        after = before if index < 6 else before ^ 0x55
        lines.append("[WiFi] reg%d before=%08x after=%08x" %
                     (index, before, after))
    return lines


def capture(lines=None, *, duration="31.250", byte_override=None,
            payload_override=None):
    if payload_override is None:
        payload = "\n".join(valid_lines() if lines is None else lines) + "\n"
    else:
        payload = payload_override
    byte_count = len(payload.encode("utf-8"))
    if byte_override is not None:
        byte_count = byte_override
    return ("[host start 2026-10-10T12:00:00]\n" + payload + "\n" +
            "[host end +%ss bytes=%d status=ok]\n" % (duration, byte_count))


def changed_line(lines, old, new):
    result = list(lines)
    index = result.index(old)
    result[index] = new
    return result


class NativeWifiLogTest(unittest.TestCase):
    def test_valid_passive_initialization_report_is_scoped(self):
        result = checker.validate(capture())
        self.assertEqual(result["mac"], "10:bd:a3:9b:bc:34")
        self.assertIsNone(result["ap_count"])
        self.assertIsNone(result["record_count"])
        self.assertIsNone(result["empty_workaround"])
        self.assertIn("diagnostic only", result["scope"])

    def test_scan_positive_count_reports_exact_bounded_records(self):
        result = checker.validate(capture(valid_lines(ap_count=13)),
                                 require_scan=True)
        self.assertEqual(result["ap_count"], 13)
        self.assertEqual(result["record_count"], 8)
        self.assertFalse(result["empty_workaround"])
        self.assertIn("diagnostic only", result["scope"])

    def test_scan_zero_count_is_explicit_workaround_not_root_fix_claim(self):
        result = checker.validate(capture(valid_lines(ap_count=0)),
                                 require_scan=True)
        self.assertEqual(result["ap_count"], 0)
        self.assertEqual(result["record_count"], 0)
        self.assertTrue(result["empty_workaround"])
        self.assertIn("diagnostic only", result["scope"])

    def test_scan_is_not_optional_when_requested(self):
        with self.assertRaises(ValueError):
            checker.validate(capture(valid_lines(ap_count=None)),
                             require_scan=True)

    def test_all_required_reports_and_24_unique_registers_are_enforced(self):
        lines = valid_lines()
        missing = [line for line in lines if not line.startswith("[WiFi] reg23 ")]
        with self.assertRaises(ValueError):
            checker.validate(capture(missing))
        duplicated = lines + ["[WiFi] reg7 before=00001007 after=0000105c"]
        with self.assertRaises(ValueError):
            checker.validate(capture(duplicated))
        duplicate_setup = lines + ["[WiFi] setup attempted=1 ok=1 stage=9 error=0"]
        with self.assertRaises(ValueError):
            checker.validate(capture(duplicate_setup))

    def test_only_shared_configuration_registers_must_restore(self):
        lines = valid_lines()
        lines = changed_line(lines,
            "[WiFi] reg2 before=00001002 after=00001002",
            "[WiFi] reg2 before=00001002 after=00001003")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))
        # Dynamic status/command/input registers are intentionally not compared.
        self.assertIsNotNone(checker.validate(capture(valid_lines())))

    def test_wlan_masks_and_every_stage_status_error_are_required(self):
        lines = changed_line(valid_lines(),
            "[WiFi] WLAN attempted=1f valid=1f written=1f",
            "[WiFi] WLAN attempted=1f valid=0f written=1f")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))
        lines = changed_line(valid_lines(),
            "[WiFi] WLAN init=1 mode=1 start=1 stop=1 deinit=1 cleanup=1",
            "[WiFi] WLAN init=1 mode=1 start=0 stop=1 deinit=1 cleanup=1")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))
        lines = changed_line(valid_lines(),
            "[WiFi] WLAN status init=0 mode=0 start=0 stop=0 deinit=0",
            "[WiFi] WLAN status init=0 mode=0 start=0 stop=1 deinit=0")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))

    def test_init_reply_and_cleanup_must_be_unambiguous(self):
        bad_reports = (
            ("[WiFi] INIT preopen=0 ambiguous=0 poisoned=0",
             "[WiFi] INIT preopen=1 ambiguous=0 poisoned=0"),
            ("[WiFi] INIT chip=0d caps=0d bytes=64",
             "[WiFi] INIT chip=0c caps=0d bytes=64"),
            ("[WiFi] reply started=1 complete=1 bytes=512 sent=1 poison=0",
             "[WiFi] reply started=1 complete=0 bytes=0 sent=0 poison=0"),
            ("[WiFi] INIT cleanup ok=1 error=1 flags=00003fff",
             "[WiFi] INIT cleanup ok=1 error=0 flags=00003fff"),
            ("[WiFi] INIT cleanup ok=1 error=1 flags=00003fff",
             "[WiFi] INIT cleanup ok=1 error=1 flags=00001fff"),
            ("[WiFi] INIT cleanup ok=1 error=1 flags=00003fff",
             "[WiFi] INIT cleanup ok=1 error=1 flags=00007fff"),
            ("[WiFi] restore ok=1 stage=7 error=0 mutated=1",
             "[WiFi] restore ok=0 stage=7 error=1 mutated=1"),
        )
        for old, new in bad_reports:
            with self.subTest(old=old):
                with self.assertRaises(ValueError):
                    checker.validate(capture(changed_line(valid_lines(), old, new)))

    def test_mac_must_be_matched_nonzero_unicast(self):
        for mac in ("00:00:00:00:00:00", "03:00:00:00:00:01"):
            lines = changed_line(valid_lines(),
                "[WiFi] MAC valid=1 10:bd:a3:9b:bc:34",
                "[WiFi] MAC valid=1 " + mac)
            with self.subTest(mac=mac), self.assertRaises(ValueError):
                checker.validate(capture(lines))
        lines = changed_line(valid_lines(),
            "[WiFi] MAC matched=1 status=0 events=3 unmatched=0",
            "[WiFi] MAC matched=0 status=0 events=3 unmatched=0")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))
        lines = valid_lines() + ["[WiFi] MAC valid=0 02:00:00:00:00:01"]
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))

    def test_scan_counts_validity_and_cleanup_must_agree(self):
        cases = (
            ("[WiFi] SCAN attempted=1 ok=1 aps=3 records=3 empty_workaround=0 cleanup=1",
             "[WiFi] SCAN attempted=1 ok=1 aps=3 records=2 empty_workaround=0 cleanup=1"),
            ("[WiFi] SCAN valid start=1 count=1 records=1 stop=0",
             "[WiFi] SCAN valid start=1 count=0 records=1 stop=0"),
            ("[WiFi] SCAN valid start=1 count=1 records=1 stop=0",
             "[WiFi] SCAN valid start=1 count=1 records=1 stop=1"),
            ("[WiFi] SCAN attempted=1 ok=1 aps=3 records=3 empty_workaround=0 cleanup=1",
             "[WiFi] SCAN attempted=1 ok=0 aps=3 records=3 empty_workaround=0 cleanup=1"),
        )
        for old, new in cases:
            with self.subTest(old=old):
                with self.assertRaises(ValueError):
                    checker.validate(capture(changed_line(valid_lines(), old, new)),
                                     require_scan=True)

    def test_missing_duplicate_and_truncated_capture_envelopes_fail(self):
        good = capture()
        with self.assertRaises(ValueError):
            checker.validate(good.replace("[host start ", "[host begin ", 1))
        with self.assertRaises(ValueError):
            checker.validate(good.replace("[host end ", "[host done ", 1))
        with self.assertRaises(ValueError):
            checker.validate(good + good)
        with self.assertRaises(ValueError):
            checker.validate(good[:-30])

    def test_duration_byte_count_and_footer_status_are_checked(self):
        with self.assertRaises(ValueError):
            checker.validate(capture(duration="29.999"))
        with self.assertRaises(ValueError):
            checker.validate(capture(byte_override=1))
        with self.assertRaises(ValueError):
            checker.validate(capture().replace("status=ok", "status=error"))

    def test_integer_overflow_and_out_of_range_scan_counts_fail(self):
        lines = changed_line(valid_lines(),
            "[WiFi] reply attempted=1 ok=1 stage=8 error=0 polls=1",
            "[WiFi] reply attempted=1 ok=1 stage=8 error=0 polls=4294967296")
        with self.assertRaises(ValueError):
            checker.validate(capture(lines))
        negative = changed_line(valid_lines(),
            "[WiFi] slot1 probe ok=1 stage=8 cmd=52",
            "[WiFi] slot1 probe ok=1 stage=-1 cmd=52")
        with self.assertRaises(ValueError):
            checker.validate(capture(negative))
        for count, records in ((-1, 0), (65536, 8)):
            with self.subTest(count=count), self.assertRaises(ValueError):
                checker.validate(capture(valid_lines(ap_count=count,
                                                     record_count=records)),
                                 require_scan=True)

    def test_fault_reset_and_rom_signatures_reject_passive_capture(self):
        for marker in (
                "ESP-ROM: reset", "WDT_SYS_RESET", "panic: fault",
                "[trap] fatal kernel trap", "*** alert ***",
                "rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0xc",
                "W (49) boot.esp32p4: CPU has been reset by WDT."):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                checker.validate(capture(valid_lines() + [marker]))

    def test_cli_scan_emits_scoped_json_not_wlan_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.log"
            path.write_text(capture(valid_lines(ap_count=2)), encoding="utf-8")
            result = subprocess.run([sys.executable, str(TOOL), str(path), "--scan"],
                                    check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        decoded = json.loads(result.stdout)
        self.assertEqual(decoded["ap_count"], 2)
        self.assertIn("diagnostic only", decoded["scope"])
        self.assertNotIn("WLAN accepted", result.stdout)

    def test_cli_rejects_failed_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.log"
            bad = changed_line(valid_lines(),
                "[WiFi] MAC valid=1 10:bd:a3:9b:bc:34",
                "[WiFi] MAC valid=1 03:00:00:00:00:01")
            path.write_text(capture(bad), encoding="utf-8")
            result = subprocess.run([sys.executable, str(TOOL), str(path)],
                                    check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("native diagnostic rejected:", result.stderr)


if __name__ == "__main__":
    unittest.main()
