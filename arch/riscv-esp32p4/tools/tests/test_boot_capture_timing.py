"""Host-only timing sidecar must preserve captured bytes and offsets."""
import importlib.util
import io
import json
import pathlib
import sys
import types
import unittest

sys.dont_write_bytecode = True
sys.modules.setdefault("serial", types.ModuleType("serial"))
spec = importlib.util.spec_from_file_location(
    "boot_capture", pathlib.Path(__file__).parents[1] / "reset-and-log.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CaptureTimingTest(unittest.TestCase):
    def test_raw_and_timing(self):
        raw, timing = io.BytesIO(), io.StringIO()
        output = capture.CaptureOutput(raw, timing)
        output.write(b"ROM\r\n", 0.75)
        output.write(b"\xffkernel\n", 1.25)
        self.assertEqual(raw.getvalue(), b"ROM\r\n\xffkernel\n")
        self.assertEqual([json.loads(line) for line in timing.getvalue().splitlines()],
                         [{"seconds": 0.75, "offset": 0, "bytes": 5},
                          {"seconds": 1.25, "offset": 5, "bytes": 8}])
        self.assertEqual(output.offset, 13)

    def test_default_raw_only(self):
        raw = io.BytesIO()
        output = capture.CaptureOutput(raw)
        output.write(b"unchanged", 2.0)
        self.assertEqual(raw.getvalue(), b"unchanged")


if __name__ == "__main__":
    unittest.main()
