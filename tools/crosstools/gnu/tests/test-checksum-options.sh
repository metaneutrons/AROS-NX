#!/bin/sh
set -eu

# Execute the patched upstream checksum-options rule, not a copied shell rule.
# Optional argument 2 runs the actual native GCC genchecksum over the records.
test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
archive=${1:?Usage: test-checksum-options.sh GCC-source.tar.xz [native-genchecksum]}
generator=${2:-}
probe=$(mktemp -d "${TMPDIR:-/tmp}/aros-gcc-checksum-options.XXXXXX")
trap 'if [ "${AROS_KEEP_CHECKSUM_PROBE:-0}" = 1 ]; then printf "%s\n" "$probe"; else rm -rf "$probe"; fi' EXIT HUP INT TERM
mkdir -p "$probe/source/gcc" "$probe/a" "$probe/b"
tar -xOf "$archive" gcc-16.2.0/gcc/Makefile.in > "$probe/source/gcc/Makefile.in"
tar -xOf "$archive" gcc-16.2.0/move-if-change > "$probe/source/move-if-change"
chmod +x "$probe/source/move-if-change"
awk '
  /^diff -ruN / { selected = ($0 ~ /gcc-16[.]2[.]0\/gcc\/Makefile[.]in /) }
  selected { print }
' "$test_dir/../gcc-16.2.0-aros.diff" > "$probe/makefile.patch"
patch --batch --forward --fuzz=0 -p1 -d "$probe/source" < "$probe/makefile.patch"
awk '
  /^checksum-options:$/ { selected = 1 }
  selected && /^$/ { exit }
  selected { print }
' "$probe/source/gcc/Makefile.in" > "$probe/rule.mk"
test -s "$probe/rule.mk"

maps_a='-ffile-prefix-map=/producer/a/source=/usr/src/aros -fdebug-prefix-map=/producer/a/work=/usr/src/build -fmacro-prefix-map=/producer/a/tools=/usr/src/tools'
maps_b='-ffile-prefix-map=/producer/b/source=/usr/src/aros -fdebug-prefix-map=/producer/b/work=/usr/src/build -fmacro-prefix-map=/producer/b/tools=/usr/src/tools'
helper=$test_dir/../aros-checksum-options.awk
make_program=${MAKE:-make}

record() {
  directory=$1; declared=$2; flags=$3; layout=$4; helper_path=$5
  "$make_program" -B -s -C "$directory" -f "$probe/rule.mk" checksum-options \
    srcdir="$probe/source/gcc" AWK=awk LINKER=/usr/bin/c++ \
    ALL_LINKERFLAGS="$flags" LDFLAGS='-Wl,--as-needed' \
    AROS_GCC_RELEASE_LAYOUT="$layout" \
    AROS_GCC_RELEASE_PREFIX_MAPS="$declared" \
    AROS_GCC_RELEASE_CHECKSUM_HELPER="$helper_path"
}

record "$probe/a" "$maps_a" "$maps_a -fno-exceptions" yes "$helper"
record "$probe/b" "$maps_b" "$maps_b -fno-exceptions" yes "$helper"
cmp "$probe/a/checksum-options" "$probe/b/checksum-options"
if cmp -s "$probe/a/checksum-options.raw" "$probe/b/checksum-options.raw"; then
  echo 'FAIL: counterprobe physical roots were not distinct' >&2; exit 1
fi
grep -F -- '-ffile-prefix-map=/aros-recorded-root=/usr/src/aros' "$probe/a/checksum-options" >/dev/null
grep -F -- '-fdebug-prefix-map=/aros-recorded-root=/usr/src/build' "$probe/a/checksum-options" >/dev/null
grep -F -- '-fmacro-prefix-map=/aros-recorded-root=/usr/src/tools' "$probe/a/checksum-options" >/dev/null
grep -F -- '-fno-exceptions -Wl,--as-needed' "$probe/a/checksum-options" >/dev/null

if record "$probe/b" "$maps_b" "$maps_b -ffile-prefix-map=/unknown=/elsewhere" yes "$helper" 2>/dev/null; then
  echo 'FAIL: undeclared physical map accepted' >&2; exit 1
fi
if record "$probe/b" "$maps_b $maps_b" "$maps_b" yes "$helper" 2>/dev/null; then
  echo 'FAIL: duplicate declared map accepted' >&2; exit 1
fi
if record "$probe/b" "$maps_b -ffile-prefix-map=/producer/b/source=/contradictory" "$maps_b" yes "$helper" 2>/dev/null; then
  echo 'FAIL: conflicting declared destination accepted' >&2; exit 1
fi
if record "$probe/b" "$maps_b" '-fno-exceptions' yes "$helper" 2>/dev/null; then
  echo 'FAIL: missing declared maps accepted' >&2; exit 1
fi
if record "$probe/b" '-ffile-prefix-map=/=/target' '-ffile-prefix-map=/=/target' yes "$helper" 2>/dev/null; then
  echo 'FAIL: filesystem-root map accepted' >&2; exit 1
fi
if record "$probe/b" '-ffile-prefix-map=relative=/target' '-ffile-prefix-map=relative=/target' yes "$helper" 2>/dev/null; then
  echo 'FAIL: relative declared map accepted' >&2; exit 1
fi
if record "$probe/b" "$maps_b" "$maps_b" yes '' 2>/dev/null; then
  echo 'FAIL: release mode accepted missing helper' >&2; exit 1
fi

maps_changed='-ffile-prefix-map=/producer/b/source=/usr/src/changed -fdebug-prefix-map=/producer/b/work=/usr/src/build -fmacro-prefix-map=/producer/b/tools=/usr/src/tools'
record "$probe/b" "$maps_changed" "$maps_changed -fno-exceptions" yes "$helper"
if cmp -s "$probe/a/checksum-options" "$probe/b/checksum-options"; then
  echo 'FAIL: changed map destination was hidden' >&2; exit 1
fi
if [ -n "$generator" ]; then
  "$generator" "$probe/a/checksum-options" > "$probe/sum-a"
  "$generator" "$probe/b/checksum-options" > "$probe/sum-changed"
  if cmp -s "$probe/sum-a" "$probe/sum-changed"; then
    echo 'FAIL: actual GCC checksum hid changed destination' >&2; exit 1
  fi
  record "$probe/b" "$maps_b" "$maps_b -fno-exceptions" yes "$helper"
  "$generator" "$probe/b/checksum-options" > "$probe/sum-b"
  cmp "$probe/sum-a" "$probe/sum-b"
  printf '%s' '0123456789ABCDEFobject-semantic-A' > "$probe/object-a"
  printf '%s' '0123456789ABCDEFobject-semantic-B' > "$probe/object-b"
  "$generator" "$probe/object-a" "$probe/a/checksum-options" > "$probe/object-sum-a"
  "$generator" "$probe/object-b" "$probe/a/checksum-options" > "$probe/object-sum-b"
  if cmp -s "$probe/object-sum-a" "$probe/object-sum-b"; then
    echo 'FAIL: actual GCC checksum hid a raw input change' >&2; exit 1
  fi
fi
record "$probe/b" "$maps_b" "$maps_b -fexceptions" yes "$helper"
if cmp -s "$probe/a/checksum-options" "$probe/b/checksum-options"; then
  echo 'FAIL: semantic compiler flag was hidden' >&2; exit 1
fi
record "$probe/b" '' '-fno-exceptions' yes "$helper"
cmp "$probe/b/checksum-options.raw" "$probe/b/checksum-options"
record "$probe/b" "$maps_b" "$maps_b -fno-exceptions" no ''
cmp "$probe/b/checksum-options.raw" "$probe/b/checksum-options"
printf '%s\n' 'GCC checksum-options recipe and semantic counterprobes passed.'
