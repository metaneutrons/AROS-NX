#!/bin/sh
# Standalone opt-in experiment. Does not update any BSP, image, SYS: or board.
set -eu
LC_ALL=C
export LC_ALL
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: sh build-prototype.sh AROS_BUILD PINNED_TINYUSB_ARCHIVE NEW_OUTPUT [diag]" >&2
    exit 2
fi
usb_src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(CDPATH= cd -- "$1" && pwd)
archive=$2
output=$3
mode=${4:-echo}
case "$mode" in echo) define=""; binary=USBEcho;; diag) define=-DP4_USB_DIAG=1; binary=USBDiag;; *) exit 2;; esac
diag_src=$(CDPATH= cd -- "$usb_src/../../../workbench/tools/RemoteDiag" && pwd)
test ! -e "$output"
test ! -L "$output"
mkdir "$output"
target="$build/bin/esp32p4-riscv-smp"
test -d "$target/gen/include"
# Extract the actual compiler identity from this configured build, not PATH.
config="$target/gen/config/target.cfg"
cc=$(sed -n 's/^TARGET_CPP[[:space:]]*:=[[:space:]]*$(strip \(.*\) -E)$/\1/p' "$config")
test -n "$cc"
test -x "$cc"
python3 "$usb_src/prepare_stack.py" "$archive" "$output/tinyusb"
"$cc" -dumpmachine
"$cc" -print-sysroot
stack="$output/tinyusb/src"
sdk="$target/AROS/Developer"
set -- "$usb_src/main.c" "$usb_src/hs_phy.c" "$usb_src/echo.c" "$usb_src/descriptors.c" \
    "$stack/tusb.c" "$stack/common/tusb_fifo.c" "$stack/device/usbd.c" \
    "$stack/class/vendor/vendor_device.c" \
    "$stack/portable/synopsys/dwc2/dwc2_common.c" \
    "$stack/portable/synopsys/dwc2/dcd_dwc2.c"
if [ "$mode" = diag ]; then
    set -- "$@" "$diag_src/protocol.c" "$diag_src/service.c" "$diag_src/path.c" \
        "$diag_src/screen_aros.c" "$diag_src/files_aros.c" "$diag_src/bridge_aros.c"
fi
index=0
for source in "$@"; do
    "$cc" --sysroot="$sdk" -std=gnu11 -O2 -Wall -Wextra -Werror $define \
        -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany \
        -ffile-prefix-map="$output"=usb-echo-build \
        -ffile-prefix-map="$usb_src"=aros-usb-echo \
        -ffile-prefix-map="$diag_src"=aros-remote-diag \
        -isystem "$target/gen/include" -I"$sdk/include" -I"$usb_src" -I"$stack" -I"$diag_src" \
        -c "$source" -o "$output/object-$index.o"
    index=$((index + 1))
done
"$cc" --sysroot="$sdk" -L"$sdk/lib" "$output"/object-*.o -o "$output/$binary"
test -s "$output/$binary"
shasum -a 256 "$output/$binary"
wc -c "$output/$binary"
