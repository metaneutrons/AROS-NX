#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f "$test_dir/ab-pair-test"; rmdir "$test_dir"' EXIT HUP INT TERM
commoncrypto_flags=
if test "$(uname -s)" = Darwin; then
    commoncrypto_flags=-DAROS_AB_TEST_COMMONCRYPTO
fi
for opt in 0 2; do
    for flash in 16 32; do
        if test "$flash" = 32; then
            flash_define=-DAROS_AB_TEST_FLASH_32MB
        else
            flash_define=
        fi
        "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -O"$opt" \
            -fsanitize=address,undefined -fno-omit-frame-pointer \
            $flash_define \
            $commoncrypto_flags \
            -I"$repo/arch/riscv-esp32p4/bootloader" \
            "$repo/arch/riscv-esp32p4/tools/tests/test_ab_bootloader_pair.c" \
            -o "$test_dir/ab-pair-test"
        "$test_dir/ab-pair-test"
    done
done
