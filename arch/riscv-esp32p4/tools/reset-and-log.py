#!/usr/bin/env python3
#
#   Reset the board and capture its console from the first byte.
#
#   Copyright (C) 2026, The AROS Development Team. All rights reserved.
#
#   Why this exists: esptool has to own the serial port to reset the board,
#   and by the time it releases it and a reader opens it, the kernel's early
#   output is already gone.  On a boot that loads the module package the log
#   is 60 KB, the USB CDC buffer is far smaller, and everything printed
#   before exec - the clock report, the whole PSRAM bring-up - is overwritten
#   before a host can read it.  Several findings in this port were measured
#   twice because of that, and one register dump had to be moved into a later
#   report just to be readable.
#
#   This holds the port open and asserts the reset over the control lines
#   instead, so capture starts before the ROM prints its first line.
#
#   Needs pyserial.  With uv and no install:
#
#       uv run --with pyserial python arch/riscv-esp32p4/tools/reset-and-log.py \
#           /dev/cu.usbmodem1101 12
#
#   Prints to stdout; redirect or grep as needed.
#

import sys
import time
import argparse
import json
import math
import base64

import serial


class CaptureOutput:
    """Raw bytes plus optional host-receipt timing; not firmware timestamps."""

    def __init__(self, raw, timing=None):
        self.raw = raw
        self.timing = timing
        self.offset = 0

    def write(self, data, elapsed):
        self.raw.write(data)
        self.raw.flush()
        if self.timing is not None:
            self.timing.write(json.dumps({"seconds": elapsed,
                                         "offset": self.offset,
                                         "bytes": len(data)}) + "\n")
            self.timing.flush()
        self.offset += len(data)


def prepare_capture_port(reader):
    """Avoid a normal macOS close dropping control lines; open can still reset."""
    if sys.platform == "darwin":
        import termios
        attributes = termios.tcgetattr(reader.fileno())
        attributes[2] &= ~termios.HUPCL
        termios.tcsetattr(reader.fileno(), termios.TCSANOW, attributes)


def capture_once(port, seconds, output, journal_path, *, serial_api=serial,
                 clock=time.monotonic, sleep=time.sleep,
                 prepare_port=prepare_capture_port):
    """One reset, all received bytes retained; not a boot acceptance checker.

    Journal timestamps are host receipt times. Buffered bytes before the pulse
    belong to an unknown prior execution and cannot establish reset causality.
    Zero bytes/download mode reject the observation without another reset.
    """
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("seconds must be positive and finite")
    with open(journal_path, "x", encoding="utf-8") as journal:
        reader = None
        failure = None
        cleanup_errors = []
        start = clock()
        released = None
        reset_asserted = False
        count = 0
        head = bytearray()

        def record(event, **fields):
            journal.write(json.dumps(dict(event=event, monotonic=clock(),
                                          **fields)) + "\n")
            journal.flush()

        def received(data, phase):
            nonlocal count
            if not data:
                return
            record("chunk", phase=phase, offset=output.offset, bytes=len(data),
                   base64=base64.b64encode(data).decode("ascii"))
            output.write(data, clock() - start)
            if phase == "post_reset":
                count += len(data)
                # Bounded prefix is classification only; full bytes remain in
                # raw output and the journal, including unsuccessful boots.
                head.extend(data[:max(0, 65536 - len(head))])

        record("start", port=port, duration=seconds, attempts=1)
        try:
            reader = serial_api.Serial(port=None, baudrate=115200, timeout=0.2)
            reader.dtr = False
            reader.rts = False
            reader.port = port
            record("open", dtr=False, rts=False)
            reader.open()
            prepare_port(reader)
            # Preserve a bounded snapshot instead of flushing unread evidence.
            remaining = reader.in_waiting
            if remaining > 65536:
                raise RuntimeError("pre-reset buffer exceeds 65536 bytes; no reset")
            drain_deadline = clock() + 2.0
            while remaining:
                if clock() >= drain_deadline:
                    raise RuntimeError("pre-reset buffer drain timed out; no reset")
                reader.timeout = min(0.2, drain_deadline - clock())
                data = reader.read(min(4096, remaining))
                if not data:
                    raise RuntimeError("pre-reset buffer read stopped; no reset")
                received(data, "pre_reset")
                remaining -= len(data)
            reader.setDTR(False)
            reset_asserted = True
            reader.setRTS(True)
            record("reset_assert", dtr=False, rts=True)
            sleep(0.1)
            reader.setDTR(False)
            reader.setRTS(False)
            reset_asserted = False
            released = clock()
            record("reset_release", dtr=False, rts=False)
            deadline = released + seconds
            while clock() < deadline:
                reader.timeout = min(0.2, max(0.0, deadline - clock()))
                received(reader.read(4096), "post_reset")
        except BaseException as error:
            failure = error
        finally:
            if reader is not None:
                if reset_asserted:
                    for release_line in (reader.setDTR, reader.setRTS):
                        try:
                            release_line(False)
                        except BaseException as error:
                            cleanup_errors.append(str(error))
                            if failure is None:
                                failure = error
                try:
                    reader.close()
                except BaseException as error:
                    cleanup_errors.append(str(error))
                    if failure is None:
                        failure = error
            boot = ("no_data" if count == 0 else "download" if
                    b"waiting for download" in head or b"DOWNLOAD" in head
                    else "unclassified")
            record("end", complete=True, status="ok" if failure is None else "error",
                   error=None if failure is None else str(failure), boot=boot,
                   cleanup_errors=cleanup_errors,
                   bytes=count, total_bytes=output.offset,
                   elapsed=clock() - (released if released is not None else start))
        if failure is not None:
            raise failure
        return 0 if boot == "unclassified" else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("seconds", type=float)
    parser.add_argument("--timing", help="new JSONL host-receipt sidecar path")
    parser.add_argument("--once", action="store_true",
                        help="one reset with full-duration capture; never retry")
    parser.add_argument("--attempt-log", help="new exclusive full-byte JSONL journal for --once")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("seconds must be positive")
    if args.once != bool(args.attempt_log):
        parser.error("--once and --attempt-log must be used together")
    port, seconds = args.port, args.seconds
    # Refuse to overwrite evidence. Buffered ROM head bytes share one receipt
    # time, so the sidecar cannot resolve sub-0.6-second early boot phases.
    timing = open(args.timing, "x", encoding="utf-8") if args.timing else None
    output = CaptureOutput(sys.stdout.buffer, timing)
    if args.once:
        try:
            return capture_once(port, seconds, output, args.attempt_log)
        finally:
            if timing is not None:
                timing.close()
    s = serial.Serial(port, 115200, timeout=0.2)

    #   Reset, then confirm the board actually restarted before settling in to
    #   read.  The line toggles do not always take - roughly one attempt in
    #   six came back silent - and a silent capture is indistinguishable from a
    #   boot that produced no output, which is exactly the confusion this
    #   script exists to remove.  So it retries until the first byte arrives.
    #   esptool's hard reset, line for line: DTR low the whole time so IO0
    #   stays high and the ROM runs the application instead of its download
    #   stub, RTS pulsed to drive EN.  Getting this wrong fails in both
    #   directions - driving DTR high stopped the reset taking at all, and
    #   toggling it as part of the pulse left the board in the download stub.
    for attempt in range(6):
        s.reset_input_buffer()
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setDTR(False)
        s.setRTS(False)
        start = time.monotonic()
        time.sleep(0.15)

        first = s.read(1)
        if not first:
            continue

        #   Read enough of the ROM banner to see which way the board went.  It
        #   prints its reset cause and boot mode within the first few lines,
        #   and about half of these resets land in the download stub - the
        #   line states are right for a run boot and the ROM sometimes latches
        #   IO0 anyway.  Retrying is what makes this reliable; a 120-byte
        #   window was not enough to notice, and silent captures were being
        #   read as failed boots.
        head = first
        deadline = time.time() + 0.6
        while time.time() < deadline and b"\n" * 6 not in head:
            head += s.read(512)

        if b"waiting for download" in head or b"DOWNLOAD" in head:
            continue

        output.write(head, time.monotonic() - start)
        break
    else:
        sys.exit("board would not boot in six attempts; it kept entering "
                 "the ROM download stub")

    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            output.write(data, time.monotonic() - start)
    s.close()
    if timing is not None:
        timing.write(json.dumps({"complete": True,
                                 "seconds": time.monotonic() - start,
                                 "bytes": output.offset}) + "\n")
        timing.close()


if __name__ == "__main__":
    sys.exit(main())
