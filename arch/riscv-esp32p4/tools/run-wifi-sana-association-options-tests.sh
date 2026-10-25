#!/bin/sh
# Host sanitizer tests for the production SANA association-options header.
# This exercises the option-copy helper only; it does not qualify BeginIO,
# request lifetime, the native owner queue, radio behavior, or hardware.
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
test_file="$test_root/tools/test-wifi-sana-association-options.c"
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-wifi-association-options.XXXXXX")
trap 'command rm -rf "$test_tmp"' EXIT HUP INT TERM

cc=${CC:-cc}
common_flags="-std=c99 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-omit-frame-pointer"
UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS ASAN_OPTIONS

for optimization in 0 2; do
    # Intentional word splitting passes compiler flags as separate arguments.
    # shellcheck disable=SC2086
    "$cc" $common_flags "-O$optimization" \
        -isystem "$repo_root/compiler/include" \
        "$test_file" -o "$test_tmp/test-o$optimization"

    result=$("$test_tmp/test-o$optimization")
    if [ "$result" != "PASS: 167 cases, 584 checks" ]; then
        printf '%s\n' "$result" >&2
        echo "test binary did not report the expected complete count at -O$optimization" >&2
        exit 1
    fi
    printf '%s\n' "$result"
done
