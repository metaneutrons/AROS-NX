#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 PRIVATE_SOURCE_ROOT" >&2
    exit 2
fi
source_file=$1/slave/main/slave_control.c
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_tmp=$(mktemp -d)
trap 'rm -f "$test_tmp/macro.h" "$test_tmp/test"; rmdir "$test_tmp"' EXIT HUP INT TERM
awk '/^#define RPC_RESP_COPY_STR\(/ { copy=1 }
     /^#define RPC_RESP_COPY_BYTES_SRC_UNCHECKED/ { copy=0 }
     copy { print }' "$source_file" > "$test_tmp/macro.h"
test "$(grep -c '^#define RPC_RESP_COPY_STR(' "$test_tmp/macro.h")" = 1
for optimization in 0 2; do
    "${CC:-cc}" -D_GNU_SOURCE -std=c99 -Wall -Wextra -Werror \
        -O"$optimization" -fsanitize=address,undefined \
        -fno-omit-frame-pointer -include "$test_tmp/macro.h" \
        "$test_dir/test-string-response.c" -o "$test_tmp/test"
    ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$test_tmp/test"
done
