#!/usr/bin/env python3
#
#   Capture the console of a running board without resetting it, with host
#   timestamps, and flag every ROM banner (a reboot) as it appears.
#
#   Copyright (C) 2026, The AROS Development Team. All rights reserved.
#
#   usage: passive-log.py <serial-device> <seconds> <log>
#
#   The D1001 console is the chip's internal USB-Serial-JTAG.  Its DTR/RTS
#   lines drive the chip reset and the boot strap, so opening or closing
#   the port can reset the board: macOS raises both lines on open and drops
#   both on close (HUPCL), and either change may pass through a reset
#   state.  This leaves the lines asserted, the state pyserial opens with,
#   and clears HUPCL so the close does not drop them; later opens then see
#   no transition at all.  The first open after a close that did drop the
#   lines may still reset the board, which the log then shows at t=0.
#
#   Each chunk is prefixed by "[host +SSSS.s]" on its own line when more
#   than a second passed since the previous one, so a reboot can be dated.
#   Needs pyserial (uv run --with pyserial ...).
#

import sys
import termios
import time

import serial

BANNER = b"ESP-ROM:"


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: passive-log.py <serial-device> <seconds> <log>")
    port, seconds, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]

    s = serial.Serial(port, 115200, timeout=0.2)
    attrs = termios.tcgetattr(s.fileno())
    attrs[2] &= ~termios.HUPCL
    termios.tcsetattr(s.fileno(), termios.TCSANOW, attrs)

    start = time.time()
    last = 0.0
    tail = b""
    banners = []
    with open(path, "wb") as out:
        out.write(b"[host start %s]\n" % time.strftime("%H:%M:%S").encode())
        while time.time() - start < seconds:
            try:
                data = s.read(4096)
            except serial.SerialException as e:
                out.write(b"\n[host +%.1f read error %s]\n"
                          % (time.time() - start, str(e).encode()))
                out.flush()
                time.sleep(0.5)
                try:
                    s.close()
                    s = serial.Serial(port, 115200, timeout=0.2)
                except serial.SerialException:
                    pass
                continue
            if not data:
                continue
            now = time.time() - start
            if now - last > 1.0:
                out.write(b"\n[host +%.1f]\n" % now)
            last = now
            window = tail + data
            # tail is one byte shorter than the banner, so a hit is new
            if BANNER in window:
                banners.append(now)
                out.write(b"\n[host +%.1f ROM banner: reboot]\n" % now)
            tail = window[-(len(BANNER) - 1):]
            out.write(data)
            out.flush()
        out.write(b"\n[host end +%.1f, %d ROM banner(s) at %s]\n"
                  % (time.time() - start, len(banners),
                     ", ".join("%.1f" % b for b in banners).encode()))
    s.close()
    print("%d ROM banner(s)%s" % (len(banners),
          (" at " + ", ".join("%.1f s" % b for b in banners)) if banners else ""))


if __name__ == "__main__":
    main()
