#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-open-rx-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; command rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_open.c" \
		"$test_root/network/hosted_rx.c" \
		"$test_root/network/hosted_fifo.c" \
		"$test_root/network/sdio_commands.c" \
		"$test_root/network/hosted_wire.c" \
		"$test_root/network/hosted_init.c" \
		"$test_root/network/hosted_inbox.c" \
		"$test_root/network/hosted_epoch_reassembly.c" \
		"$test_root/network/hosted_serial.c" \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/test_hosted_open_rx.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done
