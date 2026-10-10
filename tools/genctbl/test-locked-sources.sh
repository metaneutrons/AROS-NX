#!/bin/bash
# Exercise the real source Makefile through the native Rust fetch bridge.
set -euo pipefail
if [ "$#" -ne 3 ]; then
    echo "usage: $0 AROS_CLI GNU_SOURCE_LOCK VERIFIED_UCD_ZIP" >&2
    exit 2
fi
cli=$1
lock=$2
archive=$3
source_root=$(cd "$(dirname "$0")/../.." && pwd)
proof_root=$(mktemp -d "${TMPDIR:-/tmp}/aros-genctbl-locked.XXXXXX")
echo "Retained source proof: $proof_root"
mkdir "$proof_root/cache" "$proof_root/top" "$proof_root/deny-network"
cp "$archive" "$proof_root/cache/UCD.zip"
printf '#!/bin/sh\nexit 97\n' > "$proof_root/deny-network/curl"
chmod +x "$proof_root/deny-network/curl"
export PATH="$proof_root/deny-network:$PATH"
export AROS_TOOLCHAIN_FETCH_LOCK="$lock"
export AROS_TOOLCHAIN_FETCH_CACHE="$proof_root/cache"
export AROS_TOOLCHAIN_FETCH_UPSTREAM="$source_root/scripts/fetch.sh"
export AROS_TOOLCHAIN_FETCH_LEDGER="$proof_root/usage.log"
: > "$AROS_TOOLCHAIN_FETCH_LEDGER"
make_program=${MAKE_PROGRAM:-gmake}
"$make_program" -C "$source_root/tools/genctbl" -f Makefile -j 4 all \
    "SRCDIR=$source_root" "TOP=$proof_root/top" "GENCTBL=$proof_root/genctbl" \
    "GENDIR=$proof_root/generated" "GENINCDIR=$source_root/compiler/include" \
    "PORTSSOURCEDIR=$proof_root/cache" "HOST_CC=cc" "HOST_CFLAGS=-O2 -Wall -Werror" \
    "FETCH=$cli toolchain __metamake-fetch" "ECHO=echo"
test "$(cat "$AROS_TOOLCHAIN_FETCH_LEDGER")" = UCD.zip
test -f "$proof_root/generated/ucd/.ucd-16.0.0-ready"
mkdir "$proof_root/tables"
"$proof_root/genctbl" "$proof_root/generated/ucd" "$proof_root/tables" en_US.ISO8859-1 --emit-c
test -s "$proof_root/tables/en_US_ISO8859-1.c"

# Run the actual top-level generator dependency and stdc locale rules. Only
# the MetaMake directory macro is expanded here; no replacement fetch or
# generator recipe is supplied by the fixture.
awk '
    /^\$\(GENCTBL\):/ { selected=1 }
    selected && /^\$\(FLEXCAT\):/ { exit }
    selected { print }
' "$source_root/Makefile.in" > "$proof_root/consumer.mk"
awk '
    /^\$\(GENDIR\)\/\$\(CURDIR\)\/defaults:/ { selected=1 }
    selected && /^#MM$/ { exit }
    selected {
        if ($0 ~ /^[[:space:]]+%mkdirs_q /) {
            sub(/^[[:space:]]+%mkdirs_q /, "\t@mkdir -p ")
        }
        print
    }
' "$source_root/compiler/crt/stdc/mmakefile.src" >> "$proof_root/consumer.mk"
consumer_args=(--no-print-directory -f "$proof_root/consumer.mk" -j 4
    "SRCDIR=$source_root" "TOP=$proof_root/top" "GENCTBL=$proof_root/sdk-genctbl"
    "GENDIR=$proof_root/sdk-generated" "CURDIR=compiler/crt/stdc"
    "GENINCDIR=$source_root/compiler/include" "PORTSSOURCEDIR=$proof_root/cache"
    "HOST_CC=cc" "HOST_CFLAGS=-O2 -Wall -Werror"
    "FETCH=$cli toolchain __metamake-fetch" "ECHO=echo" "CALL=")
sdk_table="$proof_root/sdk-generated/compiler/crt/stdc/defaults/en_GB_ISO8859-1.c"
"$make_program" "${consumer_args[@]}" "$sdk_table"
test -s "$sdk_table"
test ! -e "$proof_root/cache/UnicodeData.txt"
test ! -e "$proof_root/cache/SpecialCasing.txt"
sdk_digest=$(shasum -a 256 "$sdk_table" | cut -d ' ' -f 1)

# Losing one generated input must recover from the checked archive even if
# the ready stamp remains. Retain the original files instead of deleting them.
mv "$proof_root/sdk-generated/ucd/SpecialCasing.txt" "$proof_root/lost-SpecialCasing.txt"
mv "$sdk_table" "$proof_root/first-sdk-table.c"
"$make_program" "${consumer_args[@]}" "$sdk_table"
test -s "$proof_root/sdk-generated/ucd/SpecialCasing.txt"
test "$(shasum -a 256 "$sdk_table" | cut -d ' ' -f 1)" = "$sdk_digest"

# A modified cached archive must fail before any data, ready stamp or use record.
mkdir "$proof_root/bad-cache"
printf 'corrupt archive\n' > "$proof_root/bad-cache/UCD.zip"
export AROS_TOOLCHAIN_FETCH_CACHE="$proof_root/bad-cache"
export AROS_TOOLCHAIN_FETCH_LEDGER="$proof_root/bad-usage.log"
: > "$AROS_TOOLCHAIN_FETCH_LEDGER"
if "$make_program" -C "$source_root/tools/genctbl" -f Makefile \
    "$proof_root/bad-generated/ucd/UnicodeData.txt" \
    "SRCDIR=$source_root" "TOP=$proof_root/top" "GENDIR=$proof_root/bad-generated" \
    "PORTSSOURCEDIR=$proof_root/bad-cache" "FETCH=$cli toolchain __metamake-fetch" \
    "ECHO=echo" > "$proof_root/bad.stdout" 2> "$proof_root/bad.stderr"; then
    echo "modified Unicode archive was accepted" >&2
    exit 1
fi
test ! -e "$proof_root/bad-generated/ucd/UnicodeData.txt"
test ! -e "$proof_root/bad-generated/ucd/.ucd-16.0.0-ready"
test ! -s "$AROS_TOOLCHAIN_FETCH_LEDGER"
grep -q 'missing, unsafe or changed locked source archive' "$proof_root/bad.stderr"
# The normal stdc route must reject the same corrupted input, without a loose
# text cache or a stale generator making the fixture pass accidentally.
if "$make_program" "${consumer_args[@]}" \
    "GENCTBL=$proof_root/bad-sdk-genctbl" "GENDIR=$proof_root/bad-sdk-generated" \
    "PORTSSOURCEDIR=$proof_root/bad-cache" \
    "$proof_root/bad-sdk-generated/compiler/crt/stdc/defaults/en_GB_ISO8859-1.c" \
    > "$proof_root/bad-sdk.stdout" 2> "$proof_root/bad-sdk.stderr"; then
    echo "stdc accepted a modified Unicode archive" >&2
    exit 1
fi
test ! -e "$proof_root/bad-sdk-generated/compiler/crt/stdc/defaults/en_GB_ISO8859-1.c"
test ! -e "$proof_root/bad-sdk-generated/ucd/UnicodeData.txt"
test ! -s "$AROS_TOOLCHAIN_FETCH_LEDGER"
grep -q 'missing, unsafe or changed locked source archive' "$proof_root/bad-sdk.stderr"
echo "Verified UCD, stdc generation, incremental recovery and corrupt-input rejection passed."
