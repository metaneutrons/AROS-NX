#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WM_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$WM_ROOT/../../.." && pwd)
SRC_DIR="$WM_ROOT/src"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/security-summary-tests.XXXXXX")
trap 'command rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

RISCV_CC=${RISCV_CC-}
RISCV_GEN_INCLUDE=${RISCV_GEN_INCLUDE-}

if [ -n "$RISCV_CC" ]; then
	if [ -z "$RISCV_GEN_INCLUDE" ]; then
		echo "error: set both RISCV_CC and RISCV_GEN_INCLUDE for the optional ABI fixture" >&2
		exit 1
	fi
	ABI_CHECK=1
elif [ -n "$RISCV_GEN_INCLUDE" ]; then
	echo "error: set both RISCV_CC and RISCV_GEN_INCLUDE for the optional ABI fixture" >&2
	exit 1
else
	ABI_CHECK=0
fi

CC=${CC:-cc}
CFLAGS_COMMON="-std=c99 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined"

for OPT in 0 2; do
	BIN="$BUILD_DIR/test-security-summary-O$OPT"
	# Keep source files and the include path explicit so the test includes the
	# production helper and its normal common definitions, not a copied model.
	$CC $CFLAGS_COMMON -O"$OPT" \
		-I"$SRC_DIR" -I"$SRC_DIR/common" -I"$SRC_DIR/utils" \
		"$SCRIPT_DIR/test-security-summary.c" -o "$BIN"
	ASAN_OPTIONS=halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$BIN"
done

if [ "$ABI_CHECK" -eq 0 ]; then
	echo "security-summary ABI: skipped (set RISCV_CC and RISCV_GEN_INCLUDE to enable)"
else
	if [ ! -x "$RISCV_CC" ]; then
		echo "error: RISCV_CC is not executable: $RISCV_CC" >&2
		exit 1
	fi
	if [ ! -d "$RISCV_GEN_INCLUDE" ]; then
		echo "error: RISCV_GEN_INCLUDE is not a directory: $RISCV_GEN_INCLUDE" >&2
		exit 1
	fi
	"$RISCV_CC" -std=c99 -Wall -Wextra -Werror -fsyntax-only \
		-DSECURITY_SUMMARY_ABI_FIXTURE \
		-I"$REPO_ROOT/compiler/include" -I"$RISCV_GEN_INCLUDE" \
		"$SCRIPT_DIR/test-security-summary.c"
	echo "security-summary ABI: RV32 public headers compiled; this is not a target capability claim"
fi
