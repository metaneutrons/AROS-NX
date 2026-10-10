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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("seconds", type=float)
    parser.add_argument("--timing", help="new JSONL host-receipt sidecar path")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("seconds must be positive")
    port, seconds = args.port, args.seconds
    # Refuse to overwrite evidence. Buffered ROM head bytes share one receipt
    # time, so the sidecar cannot resolve sub-0.6-second early boot phases.
    timing = open(args.timing, "x", encoding="utf-8") if args.timing else None
    output = CaptureOutput(sys.stdout.buffer, timing)
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
    main()
