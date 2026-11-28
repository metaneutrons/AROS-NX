#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
fixture_dir="$script_dir/c6-discovery-fixture"
collector_dir="$script_dir/c6-private-firmware"
boardtool_dir="$repo_root/arch/riscv-esp32p4/boardtool"
layout_dir="$repo_root/workbench/tools/ESP32Board"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/c6-discovery-tests.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-clang}

build_and_run() {
    variant=$1
    optimization=$2
    variant_flags=
    if [ "$variant" = virtual ]; then
        variant_flags=-DCONFIG_EFUSE_VIRTUAL
    fi

    common_flags="-std=c11 -Wall -Wextra -Werror -g -O$optimization -fsanitize=address,undefined -fno-omit-frame-pointer"
    allocation_flags="-Dmalloc=c6_test_malloc -Dcalloc=c6_test_calloc -Drealloc=c6_test_realloc -Dfree=c6_test_free"
    tag="$variant-O$optimization"

    # These are the production translation units, compiled directly from the
    # repository. Only IDF headers and API implementations are mocked.
    $cc $common_flags $variant_flags $allocation_flags \
        -I"$fixture_dir" -I"$boardtool_dir" -I"$collector_dir" -I"$layout_dir" \
        -include "$fixture_dir/c6_alloc.h" \
        -c "$collector_dir/discovery.c" -o "$tmp_dir/$tag-collector.o"
    $cc $common_flags $variant_flags $allocation_flags \
        -I"$fixture_dir" -I"$boardtool_dir" -I"$layout_dir" \
        -include "$fixture_dir/c6_alloc.h" \
        -c "$boardtool_dir/discovery.c" -o "$tmp_dir/$tag-validator.o"
    $cc $common_flags $variant_flags $allocation_flags \
        -I"$fixture_dir" -I"$layout_dir" \
        -include "$fixture_dir/c6_alloc.h" \
        -c "$layout_dir/layout.c" -o "$tmp_dir/$tag-layout.o"
    $cc $common_flags $variant_flags \
        -I"$fixture_dir" -I"$boardtool_dir" -I"$layout_dir" -I"$script_dir" \
        -c "$script_dir/test-c6-discovery.c" -o "$tmp_dir/$tag-test.o"
    $cc $common_flags "$tmp_dir/$tag-collector.o" "$tmp_dir/$tag-validator.o" \
        "$tmp_dir/$tag-layout.o" "$tmp_dir/$tag-test.o" -o "$tmp_dir/$tag"

    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$tmp_dir/$tag"
}

for variant in normal virtual; do
    for optimization in 0 2; do
        build_and_run "$variant" "$optimization"
    done
done
