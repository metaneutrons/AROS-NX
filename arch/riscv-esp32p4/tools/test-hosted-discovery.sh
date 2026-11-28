#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf '%s\n' "Usage: $0 PRIVATE_GENERATED_HOSTED_SOURCE" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
port_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
repo_root=$(CDPATH= cd -- "$port_dir/../.." && pwd)
source_root=$1

proto_dir="$source_root/common/proto"
protobuf_c_dir="$source_root/common/protobuf-c"
if [ ! -f "$proto_dir/esp_hosted_rpc.pb-c.h" ] ||
   [ ! -f "$proto_dir/esp_hosted_rpc.pb-c.c" ] ||
   [ ! -f "$protobuf_c_dir/protobuf-c/protobuf-c.h" ] ||
   [ ! -f "$protobuf_c_dir/protobuf-c/protobuf-c.c" ]; then
    printf '%s\n' "generated schema or vendored protobuf-c runtime not found under $source_root" >&2
    exit 2
fi

network_dir="$port_dir/network"
boardtool_dir="$port_dir/boardtool"
layout_dir="$repo_root/workbench/tools/ESP32Board"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-discovery.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-clang}

build_and_run() {
    optimization=$1
    tag="O$optimization"
    "$cc" -std=c11 -Wall -Wextra -Werror -O"$optimization" -g \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$network_dir" -I"$boardtool_dir" -I"$layout_dir" \
        -I"$proto_dir" -I"$protobuf_c_dir" \
        "$network_dir/hosted_rpc.c" \
        "$network_dir/hosted_init.c" \
        "$network_dir/hosted_discovery.c" \
        "$boardtool_dir/discovery_wire.c" \
        "$boardtool_dir/discovery.c" \
        "$network_dir/test_hosted_discovery.c" \
        "$proto_dir/esp_hosted_rpc.pb-c.c" \
        "$protobuf_c_dir/protobuf-c/protobuf-c.c" \
        -o "$tmp_dir/$tag"
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$tmp_dir/$tag" "$tag"
}

build_and_run 0
build_and_run 2
