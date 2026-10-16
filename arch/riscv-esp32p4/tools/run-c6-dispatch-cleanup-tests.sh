#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf '%s\n' "usage: $0 C6_CANDIDATE_SOURCE_ROOT" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
candidate_root=$(CDPATH= cd -- "$1" && pwd)
candidate_source=$candidate_root/slave/main/slave_control.c
proto_dir=$candidate_root/common/proto
protobuf_c_dir=$candidate_root/common/protobuf-c
common_include_dir=$candidate_root/common/include

if [ ! -f "$candidate_source" ] ||
   [ ! -f "$proto_dir/esp_hosted_rpc.pb-c.h" ] ||
   [ ! -f "$protobuf_c_dir/protobuf-c/protobuf-c.h" ] ||
   [ ! -f "$common_include_dir/adapter.h" ]; then
    printf '%s\n' "candidate source root is missing slave/main/slave_control.c or generated protocol headers: $candidate_root" >&2
    exit 2
fi

signature_count=$(grep -c '^esp_err_t data_transfer_handler(' "$candidate_source" || true)
if [ "$signature_count" -ne 1 ]; then
    printf '%s\n' "expected exactly one data_transfer_handler definition in $candidate_source" >&2
    exit 2
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/c6-dispatch-cleanup-tests.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM
handler_include=$tmp_dir/data-transfer-handler.extracted.inc
sed -n '/^esp_err_t data_transfer_handler(/,/^}/p' "$candidate_source" > "$handler_include"
if [ ! -s "$handler_include" ] || ! grep -q '^}' "$handler_include"; then
    printf '%s\n' "failed to extract data_transfer_handler from $candidate_source" >&2
    exit 2
fi

cc=${CC:-clang}
handler_test=$script_dir/test-c6-dispatch-cleanup.c

build_and_run() {
    optimization=$1
    tag=O$optimization
    common_flags="-std=c11 -Wall -Wextra -Werror -Wno-enum-conversion -Wno-unused-parameter -g -O$optimization -fsanitize=address,undefined -fno-omit-frame-pointer"
    handler_object=$tmp_dir/$tag.handler.o
    test_object=$tmp_dir/$tag.test.o

    # Compile only the extracted production function with the shim preamble
    # and calloc interposer. Protobuf and dispatcher stubs use the host heap.
    "$cc" $common_flags -I"$script_dir/c6-discovery-fixture" \
        -I"$proto_dir" -I"$protobuf_c_dir" -I"$common_include_dir" -I"$tmp_dir" \
        -DC6_DISPATCH_HANDLER_SHIM=1 -DC6_BOARD_DISCOVERY_VERSION=2 \
        -Dcalloc=c6_dispatch_outbuf_calloc \
        -c "$handler_test" -o "$handler_object"
    "$cc" $common_flags -I"$script_dir/c6-discovery-fixture" \
        -I"$proto_dir" -I"$protobuf_c_dir" -I"$common_include_dir" -I"$tmp_dir" \
        -c "$handler_test" -o "$test_object"
    "$cc" $common_flags "$handler_object" "$test_object" -o "$tmp_dir/$tag"
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$tmp_dir/$tag"
}

build_and_run 0
build_and_run 2
