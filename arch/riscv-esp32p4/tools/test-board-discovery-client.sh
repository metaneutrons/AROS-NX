#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
boardtool_dir="$repo_root/arch/riscv-esp32p4/boardtool"
layout_dir="$repo_root/workbench/tools/ESP32Board"
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/p4-board-discovery-client.XXXXXX")
trap 'command rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-clang}

build_test() {
    client_source=$1
    output=$2
    optimization=$3

    "$cc" -std=c11 -Wall -Wextra -Werror -pedantic -g \
        -O"$optimization" -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        -I"$boardtool_dir" -I"$layout_dir" \
        "$client_source" \
        "$boardtool_dir/discovery_wire.c" \
        "$boardtool_dir/discovery.c" \
        "$script_dir/test-board-discovery-client.c" \
        -o "$output"
}

for optimization in 0 2; do
    build_test "$boardtool_dir/discovery_client.c" \
        "$tmp_dir/o$optimization" "$optimization"
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$tmp_dir/o$optimization" "O$optimization"
done

# Negative control: removing the accepted-release condition must fail because
# the refusal case would otherwise publish its candidate before confirmed release.
sed 's/if (state == BOARD_CLIENT_LEASE_RELEASED \&\& release_accepted) {/if (state == BOARD_CLIENT_LEASE_RELEASED) {/' \
    "$boardtool_dir/discovery_client.c" > "$tmp_dir/discovery_client-mutant.c"
if ! cmp -s "$boardtool_dir/discovery_client.c" "$tmp_dir/discovery_client-mutant.c"; then
    build_test "$tmp_dir/discovery_client-mutant.c" \
        "$tmp_dir/mutant" 2
    if ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$tmp_dir/mutant" mutant > "$tmp_dir/mutant.log" 2>&1; then
        printf '%s\n' "FAIL negative mutant unexpectedly passed" >&2
        exit 1
    fi
    printf '%s\n' "PASS negative mutant rejected: publication without accepted release"
else
    printf '%s\n' "FAIL negative mutant did not modify the client source" >&2
    exit 1
fi
