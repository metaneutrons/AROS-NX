#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-request-ids.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/hosted_request_ids.o"; rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_request_ids.c" \
		"$test_root/network/test_hosted_request_ids.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done

"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -O2 -c \
	"$test_root/network/hosted_request_ids.c" \
	-o "$test_tmp/hosted_request_ids.o"
undefined_imports=$("${NM:-nm}" -u "$test_tmp/hosted_request_ids.o" | \
	awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }')
if [ -n "$undefined_imports" ]; then
	echo "unexpected undefined imports in hosted_request_ids.o: $undefined_imports" >&2
	exit 1
fi
