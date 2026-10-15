#!/bin/sh
# Build the separate remote grant process and touch-launchable consent gate.
# No firmware/images/ports are changed. Uses only the configured P4 toolchain.
set -eu
export LC_ALL=C
test "$#" -eq 2 || { echo 'usage: build-port-client.sh AROS_BUILD NEW_OUTPUT' >&2; exit 2; }
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(CDPATH= cd -- "$1" && pwd)
out=$2
test ! -e "$out" && test ! -L "$out"
mkdir "$out"
target="$build/bin/esp32p4-riscv-smp"
config="$target/gen/config/target.cfg"
cc=$(sed -n 's/^TARGET_CPP[[:space:]]*:=[[:space:]]*$(strip \(.*\) -E)$/\1/p' "$config")
test -n "$cc" && test -x "$cc"
sdk="$target/AROS/Developer"
remote="$src/../../../workbench/tools/RemoteDiag"
set -- "$src/remote_client.c" "$remote/protocol.c" "$remote/service.c" \
    "$remote/path.c" "$remote/screen_aros.c" "$remote/files_aros.c" "$remote/bridge_aros.c"
i=0
for file in "$@"; do
    "$cc" --sysroot="$sdk" -std=gnu11 -O2 -Wall -Wextra -Werror \
        -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany \
        -ffile-prefix-map="$out"=usb-port-build -ffile-prefix-map="$src"=aros-usb-port \
        -ffile-prefix-map="$remote"=aros-remote-diag \
        -isystem "$target/gen/include" -I"$sdk/include" -I"$src" -I"$remote" \
        -c "$file" -o "$out/obj-$i.o"
    i=$((i+1))
done
"$cc" --sysroot="$sdk" -L"$sdk/lib" "$out"/obj-*.o -o "$out/USBRemote"
"$cc" --sysroot="$sdk" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -DP4_USB_LAUNCH_PORT -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany \
    -ffile-prefix-map="$src"=aros-usb-port -isystem "$target/gen/include" \
    -I"$sdk/include" -L"$sdk/lib" "$src/launch_diag.c" -o "$out/USBRemoteStart"
shasum -a 256 "$out/USBRemote" "$out/USBRemoteStart"
wc -c "$out/USBRemote" "$out/USBRemoteStart"
