#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-control-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/hosted_control.o"; command rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_control.c" \
		"$test_root/network/hosted_tx.c" \
		"$test_root/network/hosted_fifo.c" \
		"$test_root/network/sdio_commands.c" \
		"$test_root/network/hosted_mac.c" \
		"$test_root/network/hosted_epoch_reassembly.c" \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_serial.c" \
		"$test_root/network/hosted_wire.c" \
		"$test_root/network/test_hosted_control.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done


"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic -O2 \
	-fno-stack-protector \
	-c "$test_root/network/hosted_control.c" \
	-o "$test_tmp/hosted_control.o"
if ! nm -u "$test_tmp/hosted_control.o" | awk '
	NF == 0 { next }
	{
		symbol = $NF
		sub(/^_/, "", symbol)
		if (symbol == "hosted_serial_decode" ||
		    symbol == "hosted_serial_encode" ||
		    symbol == "p4_hosted_decode" ||
		    symbol == "p4_hosted_encode" ||
		    symbol == "p4_hosted_fifo_finish" ||
		    symbol == "p4_hosted_fifo_rx" ||
		    symbol == "p4_hosted_reassembly_abort" ||
		    symbol == "p4_hosted_reassembly_feed" ||
		    symbol == "p4_hosted_reassembly_release" ||
		    symbol == "p4_hosted_reassembly_start" ||
		    symbol == "p4_hosted_reassembly_view" ||
		    symbol == "p4_hosted_mac_request" ||
		    symbol == "p4_hosted_mac_response" ||
		    symbol == "p4_hosted_rpc_decode" ||
		    symbol == "p4_hosted_tx_send")
			next
		bad = 1
	}
	END { exit bad }
'; then
	echo "hosted_control.o has an unexpected undefined symbol" >&2
	exit 1
fi
