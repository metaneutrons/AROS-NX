#!/bin/sh

# A P4_SRAMCODE function may run while the external-memory cache is
# suspended. A direct call, jump or data reference back into the XIP window
# would then stop the hart before it could report the mistake. Keep this as
# a link check because `static inline` is not a promise: at -Os GCC may emit
# one out-of-line copy in .flash.text and call it from .sramtext.

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 OBJDUMP ELF" >&2
    exit 2
fi

objdump=$1
image=$2
disassembly=$("$objdump" -dr --section=.sramtext "$image")

# The external flash instruction/data window is 0x40000000-0x43ffffff.
# ROM calls at 0x4fc... and internal SRAM targets at 0x4ff... are expected.
bad=$(printf '%s\n' "$disassembly" |
    awk '/4[0-3][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] <[^>]*>/ { print }')

if [ -n "$bad" ]; then
    echo "error: .sramtext refers to the XIP flash window:" >&2
    printf '%s\n' "$bad" >&2
    exit 1
fi
