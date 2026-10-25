#!/bin/sh
# Exercise the actual pure function-setup helper; not a hardware test.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-sdio-setup.XXXXXX")
trap 'rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; rmdir "$test_tmp"' EXIT HUP INT TERM
for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -O"$test_opt" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/sdio_setup.c" \
        "$test_root/network/test_sdio_setup.c" -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
