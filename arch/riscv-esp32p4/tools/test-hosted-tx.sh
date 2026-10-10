#!/bin/sh
# Exercise the bounded single-frame ESP-Hosted FIFO transmitter.
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d /tmp/p4-hosted-tx-tests.XXXXXX)
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/hosted_tx.o"; command rmdir "$test_tmp"' EXIT HUP INT TERM
for test_opt in 0 2; do
    cc -std=c99 -Wall -Wextra -Werror -pedantic -O"$test_opt" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$test_root/network/hosted_fifo.c" \
        "$test_root/network/sdio_commands.c" \
        "$test_root/network/hosted_wire.c" \
        "$test_root/network/hosted_tx.c" \
        "$test_root/network/test_hosted_tx.c" -o "$test_tmp/test-o$test_opt"
    "$test_tmp/test-o$test_opt"
done

cc -std=c99 -Wall -Wextra -Werror -pedantic -O2 \
    -ffreestanding -fno-stack-protector \
    -c "$test_root/network/hosted_tx.c" -o "$test_tmp/hosted_tx.o"
if ! nm -u "$test_tmp/hosted_tx.o" | awk '
    NF == 0 { next }
    {
        symbol = $NF
        sub(/^_/, "", symbol)
        if (symbol == "p4_hosted_decode" ||
            symbol == "p4_hosted_fifo_finish" ||
            symbol == "p4_hosted_fifo_tx")
            next
        bad = 1
    }
    END { exit bad }
'; then
    echo "hosted_tx.o has an unexpected undefined symbol" >&2
    exit 1
fi
