#!/usr/bin/env python3
"""Passively capture a finite window from an already-running board.

This tool does not issue a reset, deliberately toggle control lines, reconnect,
or retry. Its current retrieval profile requests DTR and RTS asserted (True)
before opening the selected port. That profile has been warm-verified on a
JC1060, but is not universally safe: opening the port can still reset hardware.
For every cold retrieval, verify the reported `now` value against the user's
power-on/desktop observation; a reset on open invalidates that cold measurement.
An ESP-ROM banner may also have been buffered from an earlier boot, so seeing
one does not establish that this open caused a fresh boot. HUPCL is cleared on
macOS after open to avoid dropping the lines on normal close.

The file contains host start/end markers around the unmodified serial bytes.
The elapsed footer uses the host monotonic clock; it is not a firmware phase
timestamp.
"""

import argparse
import datetime
import math
import sys
import time

import serial


BAUD_RATE = 115200
READ_CHUNK_SIZE = 4096
READ_TIMEOUT_SECONDS = 0.2


def positive_finite_duration(value):
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("duration must be a number") from error
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be finite and positive")
    return seconds


def clear_hupcl(port, termios_module=None):
    """Prevent a normal macOS close from dropping the modem-control lines."""
    if termios_module is None:
        import termios as termios_module

    attributes = termios_module.tcgetattr(port.fileno())
    attributes[2] &= ~termios_module.HUPCL
    termios_module.tcsetattr(port.fileno(), termios_module.TCSANOW, attributes)


def capture(port, seconds, path, *, serial_api=None, platform=None,
            termios_module=None, monotonic=None, wall_time=None):
    """Capture serial bytes to a new file; raise on any capture failure."""
    if serial_api is None:
        serial_api = serial
    if platform is None:
        platform = sys.platform
    if monotonic is None:
        monotonic = time.monotonic
    if wall_time is None:
        wall_time = datetime.datetime.now().astimezone().isoformat(timespec="seconds")

    # Exclusive creation prevents an accidental overwrite and happens before
    # touching the serial device.
    with open(path, "xb") as output:
        output.write(("[host start %s]\n" % wall_time).encode("utf-8"))
        output.flush()
        start = monotonic()
        byte_count = 0
        reader = None
        failure = None

        try:
            reader = serial_api.Serial(port=None, baudrate=BAUD_RATE,
                                       timeout=min(READ_TIMEOUT_SECONDS, seconds))
            # Request the warm-verified JC1060 retrieval profile before open.
            reader.dtr = True
            reader.rts = True
            reader.port = port
            reader.open()
            if platform == "darwin":
                clear_hupcl(reader, termios_module)

            deadline = start + seconds
            while monotonic() < deadline:
                data = reader.read(READ_CHUNK_SIZE)
                if data:
                    output.write(data)
                    output.flush()
                    byte_count += len(data)
        except BaseException as error:
            failure = error
        finally:
            if reader is not None:
                try:
                    reader.close()
                except BaseException as error:
                    if failure is None:
                        failure = error

            elapsed = max(0.0, monotonic() - start)
            status = "ok" if failure is None else "error"
            footer = "\n[host end +%.3fs bytes=%d status=%s" % (
                elapsed, byte_count, status)
            if failure is not None:
                footer += " error=%r" % str(failure)
            footer += "]\n"
            output.write(footer.encode("utf-8", errors="replace"))
            output.flush()

        if failure is not None:
            raise failure
        return byte_count, elapsed


def main(argv=None, *, serial_api=None, platform=None, termios_module=None,
         monotonic=None, wall_time=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial device path")
    parser.add_argument("seconds", type=positive_finite_duration,
                        help="finite positive capture duration")
    parser.add_argument("log", help="new raw capture file (must not exist)")
    args = parser.parse_args(argv)

    try:
        byte_count, elapsed = capture(
            args.port, args.seconds, args.log, serial_api=serial_api,
            platform=platform, termios_module=termios_module,
            monotonic=monotonic, wall_time=wall_time)
    except Exception as error:
        print("late-boot-log: capture failed: %s" % error, file=sys.stderr)
        return 1

    print("captured %d bytes in %.3f s to %s" %
          (byte_count, elapsed, args.log))
    return 0


if __name__ == "__main__":
    sys.exit(main())
