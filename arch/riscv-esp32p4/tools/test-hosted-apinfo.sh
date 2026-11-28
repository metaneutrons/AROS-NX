#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-apinfo-tests.XXXXXX")

cleanup()
{
	cleanup_status=$?
	trap - EXIT HUP INT TERM
	command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" \
		"$test_tmp/hosted_scan.o" "$test_tmp/undefined-imports.txt" || \
		cleanup_status=1
	rmdir "$test_tmp" || cleanup_status=1
	exit "$cleanup_status"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

ASAN_OPTIONS=halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1
export ASAN_OPTIONS UBSAN_OPTIONS

for test_opt in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-I"$test_root/network" -O"$test_opt" \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_scan.c" \
		"$(dirname -- "$0")/test-hosted-apinfo.c" \
		-o "$test_tmp/test-o$test_opt"
	"$test_tmp/test-o$test_opt"
done

"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -fno-stack-protector -O2 -c \
	"$test_root/network/hosted_scan.c" -o "$test_tmp/hosted_scan.o"
if ! "${NM:-nm}" -u "$test_tmp/hosted_scan.o" \
	> "$test_tmp/undefined-imports.txt"; then
	echo "nm failed for hosted_scan.o" >&2
	exit 1
fi
undefined_imports=$(awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }' \
	"$test_tmp/undefined-imports.txt")
if [ "$undefined_imports" != "p4_hosted_rpc_encode" ]; then
	echo "unexpected undefined imports in hosted_scan.o: $undefined_imports" >&2
	exit 1
fi
