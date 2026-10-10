#!/bin/sh
# Exercise the pure ESP-Hosted C6 startup host-configuration encoder.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-config-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; rmdir "$test_tmp"' EXIT HUP INT TERM
for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -O"$test_opt" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/hosted_init.c" \
        "$test_root/network/hosted_wire.c" \
        "$test_root/network/hosted_config.c" \
        "$test_root/network/test_hosted_config.c" -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
