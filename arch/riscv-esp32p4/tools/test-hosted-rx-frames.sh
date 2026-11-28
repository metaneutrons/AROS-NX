#!/bin/sh
# Exercise real RX routing of serial RPC and owned STA Ethernet frames.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-rx-frames-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; command rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
        -O"$test_opt" -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/hosted_rx.c" \
        "$test_root/network/hosted_frames.c" \
        "$test_root/network/hosted_inbox.c" \
        "$test_root/network/hosted_control.c" \
        "$test_root/network/hosted_request_ids.c" \
        "$test_root/network/hosted_tx.c" \
        "$test_root/network/hosted_fifo.c" \
        "$test_root/network/sdio_commands.c" \
        "$test_root/network/hosted_mac.c" \
        "$test_root/network/hosted_epoch_reassembly.c" \
        "$test_root/network/hosted_rpc.c" \
        "$test_root/network/hosted_serial.c" \
        "$test_root/network/hosted_wire.c" \
        "$test_root/network/hosted_init.c" \
        "$test_root/network/test_hosted_rx_frames.c" \
        -o "$test_tmp/test-o$test_opt"
    ASAN_OPTIONS=halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$test_tmp/test-o$test_opt"
done
