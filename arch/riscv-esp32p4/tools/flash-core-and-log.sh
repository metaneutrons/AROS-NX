#!/usr/bin/env bash
#
#   Write a verified core image to ota_0 (0x20000) of a development board
#   and capture the following boot from the first byte.
#
#   usage: [P4_BOARD=<board>] flash-core-and-log.sh <build-dir> <core.bin>
#              <log> <seconds> [port]
#
#   Only 0x20000 is written: the routine core offset covered by the standing
#   development-board authorization (AGENTS.md). The board is identified by
#   its MAC before the write, and the written range is verified by esptool.
#   A board is listed below only once its MAC is recorded and its own
#   authorization is in AGENTS.md; an unlisted or unrecorded board is refused.
#
set -euo pipefail

build=$1
core=$2
log=$3
seconds=$4
port=${5:-/dev/cu.usbmodem101}
board=${P4_BOARD:-d1001}
case "$board" in
    d1001)      mac="e8:f6:0a:e0:46:4c" ;;
    # JC1060P470C: MAC to be recorded at the first identification.
    jc1060p470c|jc1060wp470c) mac="" ;;
    *)          echo "unknown board '$board'" >&2; exit 1 ;;
esac
[ -n "$mac" ] || {
    echo "no recorded MAC for $board; identify the board first" >&2
    exit 1
}
esptool="$build/.venv/bin/esptool"
here=$(cd "$(dirname "$0")" && pwd)

id=$("$esptool" --chip esp32p4 --port "$port" --after no-reset chip-id 2>&1)
echo "$id" | grep -qi "MAC:[[:space:]]*$mac" || {
    echo "board on $port is not the $board ($mac)" >&2
    exit 1
}
size=$(stat -f %z "$core")
[ "$size" -le $((0x800000)) ] || { echo "core too large: $size" >&2; exit 1; }

"$esptool" --chip esp32p4 --port "$port" --before no-reset --after no-reset \
    write-flash 0x20000 "$core" > "$log.flash" 2>&1
grep -q "Hash of data verified" "$log.flash" || {
    echo "write not verified; see $log.flash" >&2
    exit 1
}
uv run --with pyserial python "$here/reset-and-log.py" "$port" "$seconds" > "$log" 2>&1
echo "core $(shasum -a 256 "$core" | cut -d' ' -f1) size $size"
echo "log $(stat -f %z "$log") bytes, sha256 $(shasum -a 256 "$log" | cut -d' ' -f1)"
