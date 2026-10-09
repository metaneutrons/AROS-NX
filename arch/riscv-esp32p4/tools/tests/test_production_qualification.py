import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("qualification", Path(__file__).parents[1] / "check-production-qualification.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def report(state="PASS", seconds=1800, count=1800):
    fields = [f"state={state}", f"duration={seconds} goal={seconds}",
              f"cpu0={count} cpu1={count}", "cpuerr0=0 cpuerr1=0", f"sdreads={count}",
              "sdmismatch=0", "sderrors=0", f"gfxupdates={count}", "reason=0 fixture=1"]
    return "\n".join("[pq] id=1234abcd " + item for item in fields)


class QualificationTests(unittest.TestCase):
    def test_complete_production(self):
        self.assertEqual(MODULE.validate(report())["gate"], "production")

    def test_repeated_identical_retained_result(self):
        self.assertEqual(MODULE.validate(report() + "\n" + report())["duration"], 1800)

    def test_smoke_is_not_production(self):
        smoke = report("SMOKE", 30, 30)
        self.assertEqual(MODULE.validate(smoke, smoke=True)["gate"], "smoke")
        with self.assertRaises(ValueError):
            MODULE.validate(smoke)

    def test_failure_and_duration(self):
        for data in (report("FAIL"), report().replace("duration=1800", "duration=1799"),
                     report().replace("reason=0", "reason=1"),
                     report().replace("sderrors=0", "sderrors=1"),
                     report().replace("sdmismatch=0", "sdmismatch=1"),
                     report().replace("cpuerr0=0", "cpuerr0=1"),
                     report().replace("cpuerr1=0", "cpuerr1=1"),
                     report().replace("fixture=1", "fixture=0")):
            with self.subTest(data=data), self.assertRaises(ValueError):
                MODULE.validate(data)

    def test_all_fields_are_mandatory(self):
        for line in report().splitlines():
            with self.subTest(line=line), self.assertRaises(ValueError):
                MODULE.validate(report().replace(line, ""))

    def test_reset_and_mixed_identity(self):
        for data in ("ESP-ROM:boot\n" + report(), report() + "\n" + report().replace("1234abcd", "5678abcd")):
            with self.assertRaises(ValueError):
                MODULE.validate(data)

    def test_contradictory_values(self):
        with self.assertRaises(ValueError):
            MODULE.validate(report() + "\n[pq] id=1234abcd cpu0=1801")

    def test_invalid_or_insufficient_counters(self):
        for key in ("cpu0", "cpu1", "sdreads", "gfxupdates"):
            for value in ("0", "899", "-1", "4294967296", "abc"):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    MODULE.validate(report().replace(f"{key}=1800", f"{key}={value}"))

    def test_capture_completion_required(self):
        body = "[host start test]\n" + report() + "\n"
        self.assertEqual(MODULE.validate_capture(body + "[host end +45.001s bytes=1000 status=ok]\n")["gate"], "production")
        for footer in ("", "[host end +45.0s bytes=1000 status=error]\n",
                       "[host end +39.0s bytes=1000 status=ok]\n",
                       "[host end +45.0s bytes=0 status=ok]\n"):
            with self.subTest(footer=footer), self.assertRaises(ValueError):
                MODULE.validate_capture(body + footer)

    def test_source_report_matches_validator(self):
        import re
        source = (Path(__file__).parents[2] / "qualification" / "production-qualify.c").read_text()
        formats = re.findall(r'bug\("(\[pq\][^"\n]*)"', source)
        self.assertEqual(len(formats), 9)
        emitted = set()
        for fmt in formats:
            emitted.update(re.findall(r"\b([a-z][a-z0-9]*)=%(?:lu|s)", fmt))
            maximum = fmt.replace("%08lx", "ffffffff").replace("%lu", "4294967295").replace("%s", "INCOMPLETE").replace("\\n", "\n")
            self.assertLessEqual(len(maximum.encode("utf-8")), 64)
        self.assertEqual(emitted, MODULE.FIELDS)


if __name__ == "__main__":
    unittest.main()
