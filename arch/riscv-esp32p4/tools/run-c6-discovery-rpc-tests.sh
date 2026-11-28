#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
port_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
repo_root=$(CDPATH= cd -- "$port_dir/../.." && pwd)
source_root=${1:?Usage: run-c6-discovery-rpc-tests.sh PRIVATE_GENERATED_HOSTED_SOURCE}

proto_dir=$source_root/common/proto
protobuf_c_dir=$source_root/common/protobuf-c
if [ ! -f "$proto_dir/esp_hosted_rpc.pb-c.h" ] ||
   [ ! -f "$protobuf_c_dir/protobuf-c/protobuf-c.c" ] ||
   [ ! -f "$protobuf_c_dir/protobuf-c/protobuf-c.h" ]; then
    printf '%s\n' "generated schema or vendored protobuf-c runtime not found under $source_root" >&2
    exit 2
fi

boardtool_dir=$port_dir/boardtool
layout_dir=$repo_root/workbench/tools/ESP32Board
firmware_dir=$script_dir/c6-private-firmware
fixture_dir=$script_dir/c6-discovery-fixture
network_dir=$port_dir/network
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/c6-discovery-rpc-tests.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-clang}

build_and_run() {
    optimization=$1
    tag=O$optimization
    common_flags="-std=c11 -Wall -Wextra -Werror -g -O$optimization -fsanitize=address,undefined -fno-omit-frame-pointer"

    compile_source() {
        source_file=$1
        object_file=$2
        shift 2
        "$cc" $common_flags \
            -I"$fixture_dir" -I"$firmware_dir" -I"$boardtool_dir" \
            -I"$layout_dir" -I"$network_dir" -I"$proto_dir" \
            -I"$protobuf_c_dir" "$@" -c "$source_file" -o "$object_file"
    }

    handler_object=$tmp_dir/$tag.rpc_discovery.o
    wire_object=$tmp_dir/$tag.discovery_wire.o
    validator_object=$tmp_dir/$tag.discovery.o
    serial_object=$tmp_dir/$tag.hosted_serial.o
    hosted_wire_object=$tmp_dir/$tag.hosted_wire.o
    generated_object=$tmp_dir/$tag.esp_hosted_rpc.pb-c.o
    runtime_object=$tmp_dir/$tag.protobuf-c.o
    test_object=$tmp_dir/$tag.test.o

    # Interpose only the production handler's allocation calls. The generated
    # protobuf code and test harness use the host allocator directly.
    compile_source "$firmware_dir/rpc_discovery.c" "$handler_object" \
        -DESP_ERR_NO_MEM=0x101 \
        -Dmalloc=c6_test_malloc -Dcalloc=c6_test_calloc -Dfree=c6_test_free
    compile_source "$boardtool_dir/discovery_wire.c" "$wire_object"
    compile_source "$boardtool_dir/discovery.c" "$validator_object"
    compile_source "$network_dir/hosted_serial.c" "$serial_object"
    compile_source "$network_dir/hosted_wire.c" "$hosted_wire_object"
    compile_source "$proto_dir/esp_hosted_rpc.pb-c.c" "$generated_object"
    compile_source "$protobuf_c_dir/protobuf-c/protobuf-c.c" "$runtime_object"
    compile_source "$script_dir/test-c6-discovery-rpc.c" "$test_object"

    "$cc" $common_flags \
        "$handler_object" "$wire_object" "$validator_object" \
        "$serial_object" "$hosted_wire_object" "$generated_object" \
        "$runtime_object" "$test_object" -o "$tmp_dir/$tag"
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$tmp_dir/$tag" "$tag"
}

build_and_run 0
build_and_run 2
