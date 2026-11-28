#!/bin/sh
# Stage owned sources into a separate candidate; never touches a board.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
candidate=$(CDPATH= cd -- "${1:?Usage: stage-discovery.sh PRIVATE_HOSTED_SOURCE}" && pwd)
test -f "$candidate/slave/main/CMakeLists.txt"
test -f "$candidate/common/proto/esp_hosted_rpc.proto"
test ! -e "$candidate/aros_extension"
collector="$candidate/aros_extension/arch/riscv-esp32p4/tools/c6-private-firmware"
boardtool="$candidate/aros_extension/arch/riscv-esp32p4/boardtool"
layout="$candidate/aros_extension/workbench/tools/ESP32Board"
mkdir -p "$collector" "$boardtool" "$layout"
for name in discovery.c discovery.h rpc_discovery.c rpc_discovery.h; do
    command cp "$script_dir/$name" "$collector/$name"
done
for name in discovery.c discovery.h discovery_wire.c discovery_wire.h; do
    command cp "$repo_root/arch/riscv-esp32p4/boardtool/$name" "$boardtool/$name"
done
for name in layout.c layout.h; do
    command cp "$repo_root/workbench/tools/ESP32Board/$name" "$layout/$name"
done
