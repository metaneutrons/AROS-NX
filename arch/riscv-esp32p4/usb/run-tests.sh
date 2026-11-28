#!/bin/sh
# Source-bound host tests only. Does not enumerate/open any USB or serial device.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: sh run-tests.sh PINNED_TINYUSB_ARCHIVE NEW_OUTPUT" >&2
    exit 2
fi
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
archive=$1
output=$2
test ! -e "$output"
test ! -L "$output"
mkdir "$output"
export PYTHONDONTWRITEBYTECODE=1
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
TINYUSB_ARCHIVE="$archive" python3 "$src/test_prepare_stack.py"
python3 "$src/echo_probe.py" --self-test
python3 "$src/test_hs_recovery.py"
python3 "$src/prepare_stack.py" "$archive" "$output/tinyusb"
stack="$output/tinyusb/src"
remote="$src/../../../workbench/tools/RemoteDiag"
for optimization in 0 2; do
    cc=${CC:-clang}
    flags="-std=c11 -Wall -Wextra -Werror -O$optimization -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    "$cc" $flags -I"$src" "$src/test_echo.c" "$src/echo.c" -o "$output/echo-O$optimization"
    "$cc" $flags -I"$src" -I"$stack" "$src/test_descriptors.c" "$src/descriptors.c" -o "$output/descriptors-O$optimization"
    "$cc" $flags -I"$src" -I"$stack" "$src/test_osal.c" "$stack/common/tusb_fifo.c" -o "$output/osal-O$optimization"
    "$cc" $flags -DP4_USB_PHY_TEST -I"$src" "$src/test_hs_phy.c" "$src/hs_phy.c" -o "$output/phy-O$optimization"
    "$cc" $flags -I"$src" "$src/test_cdc_log.c" "$src/cdc_log.c" -o "$output/cdc-O$optimization"
    "$cc" $flags -I"$src" "$src/test_launch_diag.c" -o "$output/launcher-O$optimization"
    "$cc" $flags -DP4_USB_LAUNCH_ECHO -I"$src" "$src/test_launch_diag.c" -o "$output/echo-launcher-O$optimization"
    "$cc" $flags -DP4_USB_ECHO_HOST_TEST -Dmain=p4_usb_echo_app_main -I"$src" -c "$src/main.c" -o "$output/main-O$optimization.o"
    "$cc" $flags -I"$src" "$src/test_main.c" "$src/echo.c" "$output/main-O$optimization.o" -o "$output/main-O$optimization"
    "$cc" $flags -DRD_SERVICE_TEST -Dmain=rd_fixture_main -I"$remote" -I"$src" -c "$remote/test_service.c" -o "$output/remote-fakes-O$optimization.o"
    "$cc" $flags -DRD_SERVICE_TEST -I"$remote" -I"$src" "$src/test_composite.c" "$src/cdc_log.c" "$src/echo.c" "$remote/protocol.c" "$remote/service.c" "$remote/path.c" "$output/remote-fakes-O$optimization.o" -o "$output/composite-O$optimization"
    "$output/echo-O$optimization"
    "$output/descriptors-O$optimization"
    "$output/osal-O$optimization"
    "$output/phy-O$optimization"
    "$output/cdc-O$optimization"
    "$output/launcher-O$optimization"
    "$output/echo-launcher-O$optimization"
    "$output/main-O$optimization"
    "$output/composite-O$optimization"
done
echo 'USB echo host tests passed; no native enumeration/transfer is claimed'
