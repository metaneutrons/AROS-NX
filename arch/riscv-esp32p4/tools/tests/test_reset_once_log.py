"""Host-only tests for the one-reset serial capture mode."""

import base64
import collections
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import types
import unittest


sys.dont_write_bytecode = True
sys.modules.setdefault("serial", types.ModuleType("serial"))
spec = importlib.util.spec_from_file_location(
    "reset_once_log", pathlib.Path(__file__).parents[1] / "reset-and-log.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class FakeClock:
    def __init__(self, events):
        self.now = 0.0
        self.events = events

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.events.append(("sleep", seconds))
        self.now += seconds

    def advance(self, seconds):
        self.now += seconds


class FakeSerial:
    def __init__(self, events, clock, responses=(), prebuffer=b"",
                 fail_read=False, prebuffer_chunk_size=4096,
                 prebuffer_delay=0.0):
        self.events = events
        self.clock = clock
        self.responses = collections.deque(responses)
        self.prebuffer = bytearray(prebuffer)
        self.fail_read = fail_read
        self.prebuffer_chunk_size = prebuffer_chunk_size
        self.prebuffer_delay = prebuffer_delay
        self.prebuffer_read_timeouts = []
        self.pending = b""
        self.timeout = 0.2
        self.closed = False
        self._dtr = None
        self._rts = None
        self._port = None

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._dtr = value
        self.events.append(("dtr", value))

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self._rts = value
        self.events.append(("rts", value))

    @property
    def port(self):
        return self._port

    @port.setter
    def port(self, value):
        self._port = value
        self.events.append(("port", value))

    @property
    def in_waiting(self):
        return len(self.prebuffer)

    def open(self):
        self.events.append(("open", self._port, self._dtr, self._rts))

    def setDTR(self, value):
        self.events.append(("setDTR", value))

    def setRTS(self, value):
        self.events.append(("setRTS", value))

    def reset_input_buffer(self):
        self.events.append(("reset_input_buffer",))
        self.prebuffer.clear()
        self.pending = b""

    def read(self, size):
        self.events.append(("read", size))
        if self.fail_read:
            self.clock.advance(self.timeout)
            raise OSError("read failed")
        if self.prebuffer:
            self.prebuffer_read_timeouts.append(self.timeout)
            if self.prebuffer_delay > self.timeout:
                self.clock.advance(self.timeout)
                return b""
            count = min(size, self.prebuffer_chunk_size)
            data = bytes(self.prebuffer[:count])
            del self.prebuffer[:count]
            self.clock.advance(self.prebuffer_delay)
            return data
        if self.pending:
            data, self.pending = self.pending[:size], self.pending[size:]
            return data
        if self.responses:
            response = self.responses.popleft()
            if response:
                self.pending = response
                data, self.pending = self.pending[:size], self.pending[size:]
                return data
        self.clock.advance(self.timeout)
        return b""

    def close(self):
        self.events.append(("close",))
        self.closed = True


class FakeSerialAPI:
    def __init__(self, clock, *, responses=(), prebuffer=b"", fail_read=False):
        self.events = []
        clock.events = self.events
        self.clock = clock
        self.responses = responses
        self.prebuffer = prebuffer
        self.fail_read = fail_read
        self.prebuffer_chunk_size = 4096
        self.prebuffer_delay = 0.0
        self.reader = None

    def Serial(self, **kwargs):
        self.events.append(("construct", kwargs.copy()))
        self.reader = FakeSerial(self.events, self.clock, self.responses,
                                 self.prebuffer, self.fail_read,
                                 self.prebuffer_chunk_size,
                                 self.prebuffer_delay)
        return self.reader


class ResetOnceLogTest(unittest.TestCase):
    def invoke(self, directory, *, seconds=0.6, responses=(), prebuffer=b"",
               fail_read=False, name="attempt.jsonl"):
        clock = FakeClock([])
        api = FakeSerialAPI(clock, responses=responses, prebuffer=prebuffer,
                            fail_read=fail_read)
        raw, timing = io.BytesIO(), io.StringIO()
        output = capture.CaptureOutput(raw, timing)
        journal = pathlib.Path(directory) / name
        result = capture.capture_once(
            "/dev/fake", seconds, output, journal,
            serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
            prepare_port=lambda _reader: None)
        records = [json.loads(line) for line in journal.read_text().splitlines()]
        return result, api, raw, timing, journal, records

    def test_one_exact_reset_captures_raw_bytes_and_full_post_reset_budget(self):
        payload = b"ESP-ROM: normal application boot\r\n"
        with tempfile.TemporaryDirectory() as directory:
            result, api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=1.0, responses=[payload])

        self.assertEqual(result, 0)
        self.assertEqual(raw.getvalue(), payload)
        self.assertTrue(api.reader.closed)
        self.assertEqual(api.events[0], (
            "construct", {"port": None, "baudrate": 115200, "timeout": 0.2}))
        self.assertLess(api.events.index(("dtr", False)),
                        api.events.index(("open", "/dev/fake", False, False)))
        self.assertLess(api.events.index(("rts", False)),
                        api.events.index(("open", "/dev/fake", False, False)))
        self.assertEqual(
            [event for event in api.events if event[0] in ("setDTR", "setRTS", "sleep")],
            [("setDTR", False), ("setRTS", True), ("sleep", 0.1),
             ("setDTR", False), ("setRTS", False)])
        self.assertNotIn(("reset_input_buffer",), api.events)
        self.assertGreaterEqual(records[-1]["elapsed"], 1.0)
        self.assertEqual([item["event"] for item in records], [
            "start", "open", "reset_assert", "reset_release", "chunk", "end"])
        self.assertEqual(records[-2]["phase"], "post_reset")
        self.assertEqual(base64.b64decode(records[-2]["base64"]), payload)
        self.assertEqual(records[-1]["status"], "ok")
        self.assertEqual(records[-1]["boot"], "unclassified")
        self.assertEqual(records[-1]["bytes"], len(payload))

    def test_silent_capture_uses_entire_budget_and_closes_after_one_pulse(self):
        with tempfile.TemporaryDirectory() as directory:
            result, api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=0.8)

        self.assertEqual(result, 1)
        self.assertEqual(raw.getvalue(), b"")
        self.assertTrue(api.reader.closed)
        self.assertGreaterEqual(records[-1]["elapsed"], 0.8)
        self.assertEqual(records[-1]["boot"], "no_data")
        self.assertEqual(records[-1]["bytes"], 0)
        self.assertEqual(api.events.count(("setRTS", True)), 1)
        self.assertEqual(api.events.count(("setRTS", False)), 1)
        self.assertEqual(api.events[-1], ("close",))

    def test_download_banner_is_retained_and_classified_without_retry(self):
        payload = b"ROM: waiting for download\r\n"
        with tempfile.TemporaryDirectory() as directory:
            result, api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=0.8, responses=[payload])

        self.assertEqual(result, 1)
        self.assertEqual(raw.getvalue(), payload)
        self.assertEqual(records[-1]["boot"], "download")
        self.assertEqual(records[-1]["bytes"], len(payload))
        self.assertEqual(api.events.count(("setRTS", True)), 1)

    def test_delayed_first_bytes_do_not_trigger_another_reset(self):
        payload = b"delayed boot output"
        with tempfile.TemporaryDirectory() as directory:
            result, api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=1.0,
                responses=[None, None, None, payload])

        self.assertEqual(result, 0)
        self.assertEqual(raw.getvalue(), payload)
        self.assertEqual(api.events.count(("setRTS", True)), 1)
        self.assertGreaterEqual(records[-1]["elapsed"], 1.0)

    def test_pre_reset_snapshot_is_preserved_with_phase_and_offsets(self):
        prior = b"bytes buffered before reset"
        later = b"bytes after reset"
        with tempfile.TemporaryDirectory() as directory:
            result, _api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=0.6, responses=[later], prebuffer=prior)

        self.assertEqual(result, 0)
        self.assertEqual(raw.getvalue(), prior + later)
        chunks = [item for item in records if item["event"] == "chunk"]
        self.assertEqual([item["phase"] for item in chunks],
                         ["pre_reset", "post_reset"])
        self.assertEqual([base64.b64decode(item["base64"]) for item in chunks],
                         [prior, later])
        self.assertEqual(chunks[0]["offset"], 0)
        self.assertEqual(chunks[1]["offset"], len(prior))
        self.assertEqual(records[-1]["bytes"], len(later))
        self.assertEqual(records[-1]["total_bytes"], len(prior) + len(later))

    def test_snapshot_accepts_exactly_64_kib_and_refuses_larger_without_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            result, api, raw, _timing, _journal, records = self.invoke(
                directory, seconds=0.2, prebuffer=b"x" * 65536)

        self.assertEqual(result, 1)
        self.assertEqual(len(raw.getvalue()), 65536)
        self.assertEqual(records[-1]["total_bytes"], 65536)
        self.assertEqual(records[-1]["bytes"], 0)
        self.assertEqual(api.events.count(("setRTS", True)), 1)

        with tempfile.TemporaryDirectory() as directory:
            clock = FakeClock([])
            api = FakeSerialAPI(clock, prebuffer=b"x" * 65537)
            raw = io.BytesIO()
            journal = pathlib.Path(directory) / "too-large.jsonl"
            with self.assertRaisesRegex(RuntimeError, "exceeds 65536"):
                capture.capture_once(
                    "/dev/fake", 0.2, capture.CaptureOutput(raw), journal,
                    serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
                    prepare_port=lambda _reader: None)
            records = [json.loads(line) for line in journal.read_text().splitlines()]

        self.assertTrue(api.reader.closed)
        self.assertEqual(raw.getvalue(), b"")
        self.assertNotIn(("setRTS", True), api.events)
        self.assertEqual(records[-1]["status"], "error")
        self.assertEqual(records[-1]["boot"], "no_data")

    def test_trickling_pre_reset_snapshot_times_out_without_pulsing(self):
        # One byte arrives every 125 ms. Sixteen bytes arrive during the
        # fixed two-second drain budget; the remaining byte stays buffered.
        with tempfile.TemporaryDirectory() as directory:
            clock = FakeClock([])
            api = FakeSerialAPI(clock, prebuffer=b"abcdefghijklmnopq")
            api.prebuffer_chunk_size = 1
            api.prebuffer_delay = 0.125
            raw = io.BytesIO()
            journal = pathlib.Path(directory) / "trickling.jsonl"
            with self.assertRaisesRegex(RuntimeError, "drain timed out"):
                capture.capture_once(
                    "/dev/fake", 0.2, capture.CaptureOutput(raw), journal,
                    serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
                    prepare_port=lambda _reader: None)
            records = [json.loads(line) for line in journal.read_text().splitlines()]

        retained = b"abcdefghijklmnop"
        self.assertEqual(raw.getvalue(), retained)
        self.assertTrue(api.reader.closed)
        self.assertNotIn(("setRTS", True), api.events)
        self.assertNotIn(("reset_input_buffer",), api.events)
        self.assertAlmostEqual(clock.now, 2.0)
        self.assertEqual(len(api.reader.prebuffer_read_timeouts), 16)
        for index, timeout in enumerate(api.reader.prebuffer_read_timeouts):
            remaining = 2.0 - index * 0.125
            self.assertLessEqual(timeout, remaining)
            self.assertAlmostEqual(timeout, min(0.2, remaining))
        chunks = [item for item in records if item["event"] == "chunk"]
        self.assertEqual([item["phase"] for item in chunks], ["pre_reset"] * 16)
        self.assertEqual(b"".join(base64.b64decode(item["base64"])
                                   for item in chunks), retained)
        self.assertEqual(records[-1]["event"], "end")
        self.assertTrue(records[-1]["complete"])
        self.assertEqual(records[-1]["status"], "error")
        self.assertIn("timed out", records[-1]["error"])
        self.assertEqual(records[-1]["total_bytes"], len(retained))
        self.assertEqual(records[-1]["bytes"], 0)

    def test_existing_journal_is_refused_before_serial_construction(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = pathlib.Path(directory) / "attempt.jsonl"
            journal.write_text("preserve this evidence")
            clock = FakeClock([])
            api = FakeSerialAPI(clock)
            with self.assertRaises(FileExistsError):
                capture.capture_once(
                    "/dev/fake", 0.2, capture.CaptureOutput(io.BytesIO()), journal,
                    serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
                    prepare_port=lambda _reader: None)
            self.assertEqual(journal.read_text(), "preserve this evidence")
        self.assertEqual(api.events, [])

    def test_read_error_closes_device_and_writes_error_end_record(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = FakeClock([])
            api = FakeSerialAPI(clock, fail_read=True)
            journal = pathlib.Path(directory) / "error.jsonl"
            with self.assertRaisesRegex(OSError, "read failed"):
                capture.capture_once(
                    "/dev/fake", 0.2, capture.CaptureOutput(io.BytesIO()), journal,
                    serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
                    prepare_port=lambda _reader: None)
            records = [json.loads(line) for line in journal.read_text().splitlines()]

        self.assertTrue(api.reader.closed)
        self.assertEqual(api.events[-1], ("close",))
        self.assertEqual(records[-1]["event"], "end")
        self.assertTrue(records[-1]["complete"])
        self.assertEqual(records[-1]["status"], "error")
        self.assertEqual(records[-1]["error"], "read failed")

    def test_interrupted_reset_pulse_releases_lines_closes_and_writes_error(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = FakeClock([])
            api = FakeSerialAPI(clock)
            journal = pathlib.Path(directory) / "interrupted-reset.jsonl"

            def interrupted_sleep(seconds):
                api.events.append(("sleep", seconds))
                raise KeyboardInterrupt("pulse interrupted")

            with self.assertRaisesRegex(KeyboardInterrupt, "pulse interrupted"):
                capture.capture_once(
                    "/dev/fake", 0.2, capture.CaptureOutput(io.BytesIO()), journal,
                    serial_api=api, clock=clock.monotonic, sleep=interrupted_sleep,
                    prepare_port=lambda _reader: None)
            records = [json.loads(line) for line in journal.read_text().splitlines()]

        self.assertTrue(api.reader.closed)
        self.assertEqual(
            [event for event in api.events
             if event[0] in ("setDTR", "setRTS", "sleep")],
            [("setDTR", False), ("setRTS", True), ("sleep", 0.1),
             ("setDTR", False), ("setRTS", False)])
        self.assertEqual(api.events[-1], ("close",))
        self.assertEqual([item["event"] for item in records],
                         ["start", "open", "reset_assert", "end"])
        self.assertTrue(records[-1]["complete"])
        self.assertEqual(records[-1]["status"], "error")
        self.assertEqual(records[-1]["error"], "pulse interrupted")

    def test_invalid_duration_is_rejected_before_journal_or_serial_open(self):
        for seconds in (0, -1, float("nan"), float("inf")):
            with self.subTest(seconds=seconds), tempfile.TemporaryDirectory() as directory:
                clock = FakeClock([])
                api = FakeSerialAPI(clock)
                journal = pathlib.Path(directory) / "attempt.jsonl"
                with self.assertRaises(ValueError):
                    capture.capture_once(
                        "/dev/fake", seconds, capture.CaptureOutput(io.BytesIO()), journal,
                        serial_api=api, clock=clock.monotonic, sleep=clock.sleep,
                        prepare_port=lambda _reader: None)
                self.assertFalse(journal.exists())
                self.assertEqual(api.events, [])


if __name__ == "__main__":
    unittest.main()
