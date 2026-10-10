#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-board-update-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/update.o"; rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer -I"$test_root/boardtool" \
		"$test_root/boardtool/update.c" \
		"$test_root/tools/test-board-update.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -fno-stack-protector -O2 \
	-c "$test_root/boardtool/update.c" -o "$test_tmp/update.o"
undefined_imports=$("${NM:-nm}" -u "$test_tmp/update.o" | \
	awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }')
if [ -n "$undefined_imports" ]; then
	echo "unexpected undefined imports in update.o: $undefined_imports" >&2
	exit 1
fi
