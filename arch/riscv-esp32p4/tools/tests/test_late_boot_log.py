"""Host-only tests for the passive late-boot serial reader."""

import importlib.util
import io
import pathlib
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stderr

sys.dont_write_bytecode = True
sys.modules.setdefault("serial", types.ModuleType("serial"))
spec = importlib.util.spec_from_file_location(
    "late_boot_log", pathlib.Path(__file__).parents[1] / "late-boot-log.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        self.now += 0.1
        return self.now


class FakeSerial:
    def __init__(self, events, fail_read=False, fail_open=False):
        self.events = events
        self._dtr = None
        self._rts = None
        self._port = None
        self.fail_read = fail_read
        self.fail_open = fail_open
        self.closed = False
        self.reads = 0

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, state):
        self._dtr = state
        self.events.append(("dtr", state))

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, state):
        self._rts = state
        self.events.append(("rts", state))

    @property
    def port(self):
        return self._port

    @port.setter
    def port(self, value):
        self._port = value
        self.events.append(("port", value))

    def open(self):
        self.events.append(("open", self._dtr, self._rts, self._port))
        if self.fail_open:
            raise OSError("open failed")

    def read(self, _size):
        self.reads += 1
        if self.fail_read:
            raise OSError("read failed")
        return b"ESP-ROM: buffered\r\n" if self.reads == 1 else b""

    def fileno(self):
        return 42

    def close(self):
        self.events.append(("close",))
        self.closed = True


class FakeSerialAPI:
    def __init__(self, *, fail_read=False, fail_open=False):
        self.events = []
        self.reader = None
        self.fail_read = fail_read
        self.fail_open = fail_open

    def Serial(self, **kwargs):
        self.events.append(("construct", kwargs.copy()))
        if kwargs.get("port") is not None:
            raise AssertionError("Serial must be constructed with port=None")
        self.reader = FakeSerial(self.events, self.fail_read, self.fail_open)
        return self.reader


class FakeTermios:
    HUPCL = 0x4000
    TCSANOW = 0

    def __init__(self, events):
        self.events = events
        self.attributes = [0, 0, self.HUPCL, 0, 0, 0, []]

    def tcgetattr(self, _fd):
        self.events.append(("tcgetattr",))
        return self.attributes.copy()

    def tcsetattr(self, _fd, when, attributes):
        self.events.append(("tcsetattr", when, attributes[2]))
        self.attributes = attributes


class LateBootLogTest(unittest.TestCase):
    def make_path(self, directory):
        return pathlib.Path(directory) / "capture.bin"

    def test_warm_profile_line_states_precede_port_assignment_and_open(self):
        api = FakeSerialAPI()
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_path(directory)
            count, _elapsed = capture.capture(
                "/dev/fake", 0.25, path, serial_api=api,
                platform="linux",
                monotonic=FakeClock(), wall_time="test-start")

            self.assertEqual(count, len(b"ESP-ROM: buffered\r\n"))
            events = api.events
            self.assertEqual(events[0][0], "construct")
            self.assertEqual(events[0][1]["port"], None)
            self.assertLess(events.index(("dtr", True)), events.index(("port", "/dev/fake")))
            self.assertLess(events.index(("rts", True)), events.index(("port", "/dev/fake")))
            self.assertLess(events.index(("port", "/dev/fake")),
                            events.index(("open", True, True, "/dev/fake")))
            self.assertLess(events.index(("open", True, True, "/dev/fake")),
                            events.index(("close",)))
            self.assertIn(("dtr", True), events)
            self.assertIn(("rts", True), events)
            contents = path.read_bytes()
            self.assertIn(b"[host start test-start]\n", contents)
            self.assertIn(b"ESP-ROM: buffered\r\n", contents)
            self.assertIn(b"[host end +", contents)
            self.assertIn(b"status=ok", contents)

    def test_mac_clears_hupcl_after_open(self):
        api = FakeSerialAPI()
        termios = FakeTermios(api.events)
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_path(directory)
            capture.capture("/dev/fake", 0.25, path, serial_api=api,
                            platform="darwin", termios_module=termios,
                            monotonic=FakeClock(), wall_time="test-start")

        opened = api.events.index(("open", True, True, "/dev/fake"))
        cleared = api.events.index(("tcsetattr", FakeTermios.TCSANOW, 0))
        self.assertLess(opened, cleared)
        self.assertEqual(termios.attributes[2] & FakeTermios.HUPCL, 0)

    def test_existing_output_is_refused_before_serial_open(self):
        api = FakeSerialAPI()
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_path(directory)
            path.write_bytes(b"keep this evidence")
            with self.assertRaises(FileExistsError):
                capture.capture("/dev/fake", 0.25, path, serial_api=api,
                                monotonic=FakeClock(), wall_time="test-start")
            self.assertEqual(path.read_bytes(), b"keep this evidence")
            self.assertEqual(api.events, [])

    def test_read_error_closes_port_writes_error_footer_and_main_fails(self):
        api = FakeSerialAPI(fail_read=True)
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_path(directory)
            result = capture.main(
                ["/dev/fake", "0.25", str(path)], serial_api=api,
                platform="linux",
                monotonic=FakeClock(), wall_time="test-start")

            self.assertNotEqual(result, 0)
            self.assertTrue(api.reader.closed)
            contents = path.read_bytes()
            self.assertIn(b"status=error", contents)
            self.assertIn(b"read failed", contents)
            self.assertEqual(api.events[-1], ("close",))

    def test_open_error_closes_port_writes_error_footer_and_main_fails(self):
        api = FakeSerialAPI(fail_open=True)
        with tempfile.TemporaryDirectory() as directory:
            path = self.make_path(directory)
            with redirect_stderr(io.StringIO()):
                result = capture.main(
                    ["/dev/fake", "0.25", str(path)], serial_api=api,
                    platform="linux", monotonic=FakeClock(),
                    wall_time="test-start")

            self.assertNotEqual(result, 0)
            self.assertTrue(api.reader.closed)
            contents = path.read_bytes()
            self.assertIn(b"status=error", contents)
            self.assertIn(b"open failed", contents)
            self.assertEqual(api.events[-1], ("close",))

    def test_invalid_durations_are_rejected(self):
        for duration in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(duration=duration):
                with redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as result:
                        capture.main(["/dev/fake", duration, "unused.bin"])
                self.assertNotEqual(result.exception.code, 0)


if __name__ == "__main__":
    unittest.main()
