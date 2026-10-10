#!/bin/sh
# Exercise asynchronous Hosted TX submission and owned-envelope correlation.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-pending-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; command rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -O"$test_opt" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/hosted_fifo.c" \
        "$test_root/network/sdio_commands.c" \
        "$test_root/network/hosted_wire.c" \
        "$test_root/network/hosted_tx.c" \
        "$test_root/network/hosted_rpc.c" \
        "$test_root/network/hosted_serial.c" \
        "$test_root/network/hosted_request_ids.c" \
        "$test_root/network/hosted_association.c" \
        "$test_root/network/hosted_pending.c" \
        "$test_root/network/test_hosted_pending.c" \
        -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done
