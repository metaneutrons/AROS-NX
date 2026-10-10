#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-ota-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/hosted_ota.o"; rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_ota.c" \
		"$test_root/network/test_hosted_ota.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -fno-stack-protector -O2 -c \
	"$test_root/network/hosted_ota.c" -o "$test_tmp/hosted_ota.o"
undefined_imports=$("${NM:-nm}" -u "$test_tmp/hosted_ota.o" | \
	awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }')
if [ "$undefined_imports" != "p4_hosted_rpc_encode" ]; then
	echo "unexpected undefined imports in hosted_ota.o: $undefined_imports" >&2
	exit 1
fi
