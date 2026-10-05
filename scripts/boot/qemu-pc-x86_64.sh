#!/bin/bash
#
# Boot the pc-x86_64 build in QEMU.
#
# The bootstrap is a multiboot image, so QEMU loads it the way GRUB would:
# -kernel is the bootstrap and -initrd is the comma-separated list of modules.
# Nothing else is needed; there is no bootable ISO yet, because grub is one of
# the untranspiled %build_with_configure declarations.
#
# Two things about the invocation are not obvious:
#
#   * " debug=serial" is what turns the boot console on at all. Both the
#     bootstrap and the kickstart print through con_Putc, which drops every
#     character unless BC_DEBUGENABLE is set, and
#     arch/all-native/bootconsole/common.c:79 sets it from `strstr(cmdline,
#     " debug")` -- the leading space included. Without the option a failing
#     boot looks like a black screen.
#   * only the kickstart is passed as a module by default. A package member
#     with a dangling external makes the ELF loader refuse the whole boot
#     (OPEN-POINTS 27h), so adding the packages currently replaces whatever
#     the boot would have shown with a loader panic. Pass them explicitly with
#     MODULES= once that is fixed.
#
# Usage:
#   scripts/boot/qemu-pc-x86_64.sh [-b <build-dir>] [-o <out-prefix>]
#                                  [-w <seconds>] [-- <extra qemu args>]
#
# Writes <out-prefix>.serial (the boot console) and <out-prefix>.png (the VGA
# screen at -w seconds), so a run that ends in a reset still leaves both.
# Add `-- -d int` for an exception trace, or `-- -d in_asm,int` to see the code
# a fault came from.

set -u

build="build/pc-x86_64"
out="qemu-pc-x86_64"
wait_seconds=12
memory=512
# Short, because a unix socket path is limited to 104 bytes and a build
# directory under a long path exceeds it.
monitor="${TMPDIR:-/tmp}/aros-qmp.sock"

while [ $# -gt 0 ]; do
    case "$1" in
        -b) build="$2"; shift 2 ;;
        -o) out="$2"; shift 2 ;;
        -w) wait_seconds="$2"; shift 2 ;;
        -m) memory="$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

boot="$build/SYS/boot"
if [ ! -f "$boot/pc/bootstrap" ] || [ ! -f "$boot/pc/kernel" ]; then
    echo "no bootstrap or kickstart in $boot/pc" >&2
    exit 1
fi

modules="${MODULES:-$boot/pc/kernel}"
rm -f "$monitor" "$out.ppm" "$out.png" "$out.serial"

qemu-system-x86_64 \
    -m "$memory" -no-reboot -display none \
    -qmp "unix:$monitor,server,nowait" \
    -serial "file:$out.serial" \
    -append " debug=serial" \
    -kernel "$boot/pc/bootstrap" \
    -initrd "$modules" \
    "$@" &
qemu_pid=$!

# The screendump goes through QMP rather than the monitor, so the wait and the
# request are one script and no terminal is involved.
python3 - "$monitor" "$out.ppm" "$wait_seconds" <<'PY'
import json, socket, sys, time

monitor, out, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])
connection = None
for _ in range(50):
    try:
        connection = socket.socket(socket.AF_UNIX)
        connection.connect(monitor)
        break
    except OSError:
        time.sleep(0.2)
if connection is None:
    sys.exit("qemu did not open the monitor socket")

stream = connection.makefile("rw")
try:
    stream.readline()
    stream.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    stream.flush()
    stream.readline()
    time.sleep(wait)
    stream.write(json.dumps(
        {"execute": "screendump", "arguments": {"filename": out}}) + "\n")
    stream.flush()
    print(stream.readline().strip())
    for command in ("info status", "info registers"):
        stream.write(json.dumps({
            "execute": "human-monitor-command",
            "arguments": {"command-line": command}}) + "\n")
        stream.flush()
        print(json.loads(stream.readline()).get("return", ""))
    stream.write(json.dumps({"execute": "quit"}) + "\n")
    stream.flush()
except (BrokenPipeError, OSError):
    # The guest reset and -no-reboot ended the process before the wait was
    # over. The serial log is the record in that case.
    print("qemu exited before the screendump")
PY

wait "$qemu_pid" 2>/dev/null

if [ -f "$out.ppm" ]; then
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$out.ppm" --out "$out.png" >/dev/null 2>&1
    elif command -v magick >/dev/null 2>&1; then
        magick "$out.ppm" "$out.png"
    fi
fi

echo "--- $out.serial"
tr -d '\000' < "$out.serial"
