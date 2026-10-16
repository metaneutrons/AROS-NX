#!/bin/sh
# Exercise the owned bounded Ethernet-frame FIFO.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-frames-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; command rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
        -O"$test_opt" -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/hosted_frames.c" \
        "$test_root/network/test_hosted_frames.c" \
        -o "$test_tmp/test-o$test_opt"
    ASAN_OPTIONS=halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$test_tmp/test-o$test_opt"
done
