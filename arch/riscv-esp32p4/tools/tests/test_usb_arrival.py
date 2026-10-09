"""Filesystem-only arrival observer fixtures; no hardware access."""
import importlib.util
import json
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "usb_arrival", pathlib.Path(__file__).parents[1] / "observe-usb-arrival.py")
observer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(observer)


class Clock:
    now = 100.0

    def read(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ArrivalTest(unittest.TestCase):
    def test_brackets_removal_and_arrival_without_open(self):
        clock = Clock()
        states = iter([True, False, True, True, True])
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "arrival.jsonl"
            observer.observe("/dev/fake", .06, log, monotonic=clock.read,
                             sleep=clock.sleep, probe=lambda _path: next(states))
            records = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual([r["event"] for r in records],
                         ["start", "removed", "appeared", "end"])
        for event in records[1:3]:
            self.assertLess(event["previous_sample_monotonic"], event["monotonic"])
            self.assertAlmostEqual(event["monotonic"] - event["previous_sample_monotonic"], .02)
        self.assertEqual(records[-1]["status"], "ok")

    def test_probe_error_is_not_false_removal(self):
        def fail(_path):
            raise PermissionError("probe denied")
        clock = Clock()
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "arrival.jsonl"
            with self.assertRaises(PermissionError):
                observer.observe("/dev/fake", .06, log, monotonic=clock.read,
                                 sleep=clock.sleep, probe=fail)
            records = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["status"], "error")

    def test_exclusive_output(self):
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "arrival.jsonl"
            log.write_text("preserve")
            with self.assertRaises(FileExistsError):
                observer.observe("/dev/fake", .06, log,
                                 probe=lambda _path: self.fail("must not probe"))
            self.assertEqual(log.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
