#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
source_dir=$(CDPATH= cd "$script_dir/.." && pwd)
build_dir=$(mktemp -d /tmp/rd-screen-tests.XXXXXX)
trap 'command rm -rf "$build_dir"' EXIT HUP INT TERM

for optimization in 0 2; do
    executable="$build_dir/screen-test-O$optimization"
    cc -std=c99 -Wall -Wextra -Werror -DRD_SCREEN_HOST_TEST \
        -fsanitize=address,undefined -fno-omit-frame-pointer -O"$optimization" \
        -I "$source_dir" \
        -I "$script_dir" \
        "$source_dir/screen_aros.c" \
        "$script_dir/screen_test_fake.c" \
        "$script_dir/screen_test.c" \
        -o "$executable"
    "$executable"
done
