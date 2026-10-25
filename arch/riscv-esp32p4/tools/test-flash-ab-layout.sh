#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -f "$test_dir/ab-test"; rmdir "$test_dir"' EXIT HUP INT TERM
for opt in 0 2; do
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -O"$opt" \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$repo/arch/riscv-esp32p4/kernel" \
        "$repo/arch/riscv-esp32p4/kernel/flash_ab_layout.c" \
        "$repo/arch/riscv-esp32p4/tools/tests/test_flash_ab_layout.c" \
        -o "$test_dir/ab-test"
    "$test_dir/ab-test"
done
