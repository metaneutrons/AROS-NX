#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
boardtool_dir="$repo_root/arch/riscv-esp32p4/boardtool"
layout_dir="$repo_root/workbench/tools/ESP32Board"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/c6-discovery-wire-tests.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-clang}

build_and_run() {
    optimization=$1
    tag="O$optimization"
    common_flags="-std=c11 -Wall -Wextra -Werror -g -O$optimization -fsanitize=address,undefined -fno-omit-frame-pointer"

    # Compile the production wire and validator translation units directly.
    # This host test does not link the layout parser or any IDF implementation.
    $cc $common_flags \
        -I"$boardtool_dir" -I"$layout_dir" -I"$script_dir" \
        "$boardtool_dir/discovery_wire.c" \
        "$boardtool_dir/discovery.c" \
        "$script_dir/test-c6-discovery-wire.c" \
        -o "$tmp_dir/$tag"

    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$tmp_dir/$tag" "$tag"
}

build_and_run 0
build_and_run 2
