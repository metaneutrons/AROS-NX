#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_log_root=${TEST_LOG_ROOT:-${TMPDIR:-/tmp}}
if [ ! -d "$test_log_root" ]; then
	echo "test log root does not exist: $test_log_root" >&2
	exit 2
fi
test_tmp=$(mktemp -d "$test_log_root/p4-hosted-events.XXXXXX")
trap 'command rm -f "$test_tmp/test-o0" "$test_tmp/test-o2" "$test_tmp/hosted_events.o"' EXIT HUP INT TERM

for test_opt in 0 2; do
	if ! "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
		-O"$test_opt" -fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		"$test_root/network/hosted_events.c" \
		"$test_root/network/test_hosted_events.c" \
		-o "$test_tmp/test-o$test_opt" \
		>"$test_tmp/build-o$test_opt.log" 2>&1; then
		cat "$test_tmp/build-o$test_opt.log" >&2
		echo "O$test_opt build failed; logs: $test_tmp" >&2
		exit 1
	fi
	if ! "$test_tmp/test-o$test_opt" >"$test_tmp/run-o$test_opt.log" 2>&1; then
		cat "$test_tmp/run-o$test_opt.log" >&2
		echo "O$test_opt run failed; logs: $test_tmp" >&2
		exit 1
	fi
	cat "$test_tmp/run-o$test_opt.log"
done

if ! "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
	-ffreestanding -fno-builtin -fno-stack-protector -O2 -c \
	"$test_root/network/hosted_events.c" \
	-o "$test_tmp/hosted_events.o" \
	>"$test_tmp/build-freestanding.log" 2>&1; then
	cat "$test_tmp/build-freestanding.log" >&2
	echo "freestanding build failed; logs: $test_tmp" >&2
	exit 1
fi
undefined_imports=$("${NM:-nm}" -u "$test_tmp/hosted_events.o" | \
	awk 'NF { symbol = $NF; sub(/^_/, "", symbol); print symbol }')
if [ -n "$undefined_imports" ]; then
	printf '%s\n' "$undefined_imports" >"$test_tmp/freestanding.log"
	echo "unexpected hosted_events.o imports: $undefined_imports" >&2
	echo "freestanding check failed; logs: $test_tmp" >&2
	exit 1
fi
printf 'freestanding undefined imports: none\n' >"$test_tmp/freestanding.log"
cat "$test_tmp/freestanding.log"
echo "hosted auxiliary event tests passed; logs: $test_tmp"
