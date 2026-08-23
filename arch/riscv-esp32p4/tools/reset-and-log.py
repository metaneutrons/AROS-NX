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

import serial


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: reset-and-log.py <serial-device> <seconds>")

    port, seconds = sys.argv[1], float(sys.argv[2])
    s = serial.Serial(port, 115200, timeout=0.2)

    #   Reset, then confirm the board actually restarted before settling in to
    #   read.  The line toggles do not always take - roughly one attempt in
    #   six came back silent - and a silent capture is indistinguishable from a
    #   boot that produced no output, which is exactly the confusion this
    #   script exists to remove.  So it retries until the first byte arrives.
    #   RTS alone is the reset, the same two writes esptool's hard-reset makes.
    #   DTR is left untouched on purpose: it selects boot mode, and driving it
    #   low as part of the reset left the board sitting in the ROM's download
    #   stub, which then needed esptool to recover.  Driving it high stopped
    #   the reset from taking at all.
    for attempt in range(6):
        s.reset_input_buffer()
        s.setRTS(True)
        time.sleep(0.05)
        s.setRTS(False)
        time.sleep(0.15)

        first = s.read(1)
        if not first:
            continue

        #   A board that came up in the download stub is not a boot; say so
        #   rather than reporting an empty capture.
        head = first + s.read(120)
        sys.stdout.buffer.write(head)
        if b"waiting for download" in head:
            sys.exit("\nboard entered the download stub, not a boot")
        break
    else:
        sys.exit("board did not restart after six attempts")

    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
