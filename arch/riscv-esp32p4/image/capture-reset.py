#!/usr/bin/env python3
"""Reset the board and capture its console, without losing the first lines.

    python3 arch/riscv-esp32p4/image/capture-reset.py /dev/cu.usbmodemXXXX 45

`esptool --after hard-reset` resets the board and exits, so a reader attaching
afterwards has already missed whatever was printed in the gap.  That hid the
output of anything running before the package load.  This opens the port
first and pulses RTS itself, the line esptool uses to drive EN, so the capture
starts before the board does.

Needs pyserial; the ESP-IDF virtualenv has it, for example
/Users/<you>/.espressif/tools/python/v6.0/venv/bin/python.
"""

import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
# Reset with the port already open, so the earliest output cannot be lost in
# the gap between esptool exiting and a reader attaching.  RTS drives EN on
# this board, the same line esptool pulses.
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
s.reset_input_buffer()
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(8192)
    if d:
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
s.close()
