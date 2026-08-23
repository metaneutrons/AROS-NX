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

    #   The reset the ROM's USB-Serial-JTAG bridge watches for: DTR held low
    #   while RTS is pulsed.  Same signalling esptool uses for --before
    #   usb-reset, without taking the port away from the reader.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)

    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
