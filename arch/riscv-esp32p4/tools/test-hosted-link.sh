#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/p4-hosted-link-tests.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2"; command rm -rf "$test_tmp/test-o0.dSYM" "$test_tmp/test-o2.dSYM"; rmdir "$test_tmp"' EXIT HUP INT TERM

for test_opt in 0 2; do
	"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -g -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_rpc.c" \
		"$test_root/network/hosted_association.c" \
		"$test_root/network/hosted_link.c" \
		"$test_root/tools/test-hosted-link.c" \
		-o "$test_tmp/test-o$test_opt"
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_tmp/test-o$test_opt"
done
