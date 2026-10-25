#!/bin/sh
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SOURCE_FILE="$PORT_DIR/sdmmc/clock_plan.c"
HEADER_FILE="$PORT_DIR/sdmmc/clock_plan.h"
TEST_FILE="$PORT_DIR/network/test_clock_plan.c"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/p4-clock-plan.XXXXXX")
trap 'command rm -rf "$TMP_DIR"' EXIT HUP INT TERM

CC=${CC:-cc}
COMMON_FLAGS="-std=c99 -Wall -Wextra -Werror -Wpedantic -pedantic-errors -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wundef -Wcast-qual -Wwrite-strings -g -fsanitize=address,undefined -fno-omit-frame-pointer"

for OPT_LEVEL in 0 2; do
    BINARY="$TMP_DIR/test-clock-plan-O$OPT_LEVEL"
    # Intentional word splitting supplies the individual compiler options.
    # shellcheck disable=SC2086
    "$CC" $COMMON_FLAGS "-O$OPT_LEVEL" "$SOURCE_FILE" "$TEST_FILE" \
        -o "$BINARY"
    ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$BINARY"
done

if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$HEADER_FILE" "$SOURCE_FILE" "$TEST_FILE" \
        "$SCRIPT_DIR/test-clock-plan.sh"
elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$HEADER_FILE" "$SOURCE_FILE" "$TEST_FILE" \
        "$SCRIPT_DIR/test-clock-plan.sh"
else
    printf '%s\n' 'SHA-256 utility unavailable; test runs completed.'
fi
