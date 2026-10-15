#!/bin/sh
set -eu

if test "$#" -ne 1 || ! test -f "$1"; then
    printf '%s\n' 'Usage: test-script-relocation.sh /absolute/binutils-build/libiberty/libiberty.a' >&2
    exit 2
fi
test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
task_tmp=$(mktemp -d /tmp/aros-gnu-script-relocation.XXXXXX)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
host_cc=${HOST_CC:-cc}
"$host_cc" -Wall -Wextra -Werror "$test_dir/fixture/script-prefix-probe.c" \
    "$1" -o "$task_tmp/probe"

for triple in riscv32-aros riscv64-aros; do
    package="$task_tmp/relocated package/$triple"
    mkdir -p "$package/$triple/lib/ldscripts"
    : > "$package/$triple-ld"
    printf '%s\n' 'script fixture, not a linked compiler payload' > "$package/$triple/lib/ldscripts/probe.x"
    if "$task_tmp/probe" "$package/$triple-ld" /aros-toolchain "/aros-toolchain/$triple/lib"; then
        printf '%s\n' 'FAIL: slash-free flat bindir unexpectedly resolved the script fixture' >&2
        exit 1
    else
        probe_status=$?
        if test "$probe_status" -ne 1; then
            printf '%s\n' 'FAIL: baseline probe did not reach the script lookup' >&2
            exit 1
        fi
    fi
    "$task_tmp/probe" "$package/$triple-ld" /aros-toolchain/ "/aros-toolchain/$triple/lib"
done
printf '%s\n' 'GNU flat script-prefix relocation probes passed.'
