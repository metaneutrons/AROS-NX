#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
archive=${1:?Usage: test-cody-srcdir.sh GCC-source.tar.xz [native-genchecksum]}
generator=${2:-}
helper=$test_dir/../aros-cody-srcdir.awk
patch_file=$test_dir/../gcc-16.2.0-aros.diff
make_program=${MAKE:-make}
probe=$(mktemp -d "${TMPDIR:-/tmp}/aros-gcc-cody-srcdir.XXXXXX")

cleanup() {
	if [ "${AROS_KEEP_CODY_PROBE:-0}" = 1 ]; then
		printf '%s\n' "$probe"
	else
		rm -rf "$probe"
	fi
}
trap cleanup EXIT HUP INT TERM

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

assert_equal() {
	label=$1
	expected=$2
	actual=$3
	if [ "$expected" != "$actual" ]; then
		printf 'FAIL: %s\nexpected: %s\nactual:   %s\n' \
			"$label" "$expected" "$actual" >&2
		exit 1
	fi
}

expect_helper_failure() {
	label=$1
	source=$2
	declared=$3
	if env AROS_GCC_RELEASE_LAYOUT=yes \
		AROS_GCC_RELEASE_CODY_SRCDIR="$source" \
		AROS_GCC_RELEASE_PREFIX_MAPS="$declared" \
		awk -f "$helper" </dev/null >"$probe/helper.out" 2>"$probe/helper.err"; then
		fail "$label was accepted by the source mapper"
	fi
	if [ -s "$probe/helper.out" ]; then
		fail "$label emitted a partial mapped path"
	fi
}

expect_make_failure() {
	label=$1
	declared=$2
	helper_path=$3
	if (
		cd "$build_a"
		env AROS_GCC_RELEASE_LAYOUT=yes \
			AROS_GCC_RELEASE_PREFIX_MAPS="$declared" \
			AROS_GCC_RELEASE_CODY_SRCDIR_HELPER="$helper_path" \
			"$make_program" --no-print-directory --silent -n fatal.o
	) >"$probe/make-failure.log" 2>&1; then
		fail "$label did not stop the patched libcody rule"
	fi
	if ! grep -F 'unable to map libcody SRCDIR for release layout' "$probe/make-failure.log" >/dev/null; then
		printf 'FAIL: %s failed for an unexpected reason\n' "$label" >&2
		cat "$probe/make-failure.log" >&2
		exit 1
	fi
}

mkdir -p "$probe/a" "$probe/b"
tar -xf "$archive" -C "$probe/a" \
	gcc-16.2.0/libcody \
	gcc-16.2.0/install-sh \
	gcc-16.2.0/config.guess \
	gcc-16.2.0/config.sub
awk '
	/^diff -ruN / { selected = ($0 ~ /gcc-16[.]2[.]0\/libcody\/Makefile[.]in /) }
	selected { print }
' "$patch_file" >"$probe/cody.patch"
test -s "$probe/cody.patch" || fail 'could not select the libcody Makefile patch'
patch --batch --forward --fuzz=0 -p1 -d "$probe/a/gcc-16.2.0" <"$probe/cody.patch"
mkdir -p "$probe/b/gcc-16.2.0"
cp -R "$probe/a/gcc-16.2.0/libcody" "$probe/b/gcc-16.2.0/"
cp "$probe/a/gcc-16.2.0/install-sh" "$probe/a/gcc-16.2.0/config.guess" \
	"$probe/a/gcc-16.2.0/config.sub" "$probe/b/gcc-16.2.0/"

source_a=$probe/a/gcc-16.2.0/libcody
source_b=$probe/b/gcc-16.2.0/libcody
source_parent_a=$probe/a/gcc-16.2.0
source_parent_b=$probe/b/gcc-16.2.0
build_a=$probe/a/build
build_b=$probe/b/build
mkdir -p "$build_a" "$build_b"

maps_a="-ffile-prefix-map=$source_a/unmatched=/cody/specific -ffile-prefix-map=$source_parent_a=/cody/mapped -fdebug-prefix-map=$probe/a=/debug-only"
maps_b="-ffile-prefix-map=$source_b/unmatched=/cody/specific -ffile-prefix-map=$source_parent_b=/cody/mapped -fdebug-prefix-map=$probe/b=/debug-only"
semantic_flag=-DPRESERVE_CODY_SEMANTIC_FLAG=1
cxx=${CXX:-c++}

(
	cd "$build_a"
	env CXX="$cxx" CXXFLAGS="$maps_a $semantic_flag" "$source_a/configure"
)
(
	cd "$build_b"
	env CXX="$cxx" CXXFLAGS="$maps_b $semantic_flag" "$source_b/configure"
)

env AROS_GCC_RELEASE_LAYOUT=yes \
	AROS_GCC_RELEASE_PREFIX_MAPS="$maps_a" \
	AROS_GCC_RELEASE_CODY_SRCDIR_HELPER="$helper" \
	"$make_program" --no-print-directory -C "$build_a" fatal.o >"$probe/build-a.log" 2>&1
env AROS_GCC_RELEASE_LAYOUT=yes \
	AROS_GCC_RELEASE_PREFIX_MAPS="$maps_b" \
	AROS_GCC_RELEASE_CODY_SRCDIR_HELPER="$helper" \
	"$make_program" --no-print-directory -C "$build_b" fatal.o >"$probe/build-b.log" 2>&1

expected_define="-DSRCDIR='\"/cody/mapped/libcody\"'"
for build_log in "$probe/build-a.log" "$probe/build-b.log"; do
	grep -F -- "$expected_define" "$build_log" >/dev/null || {
		printf 'FAIL: patched fatal.o rule did not use the mapped SRCDIR\n' >&2
		cat "$build_log" >&2
		exit 1
	}
	grep -F -- "$semantic_flag" "$build_log" >/dev/null || fail 'the semantic host CXX flag disappeared'
	grep -F -- '=/cody/specific' "$build_log" >/dev/null || fail 'the earlier source-map destination disappeared'
	grep -F -- '=/cody/mapped' "$build_log" >/dev/null || fail 'the matching source-map destination disappeared'
	grep -F -- '=/debug-only' "$build_log" >/dev/null || fail 'the debug-only map disappeared from compiler flags'
done
cp "$build_a/fatal.o" "$probe/fatal-a.release.o"
cp "$build_b/fatal.o" "$probe/fatal-b.release.o"
if ! cmp -s "$probe/fatal-a.release.o" "$probe/fatal-b.release.o"; then
	printf 'FAIL: mapped fatal.o bytes differ across physical source roots\n' >&2
	shasum -a 256 "$probe/fatal-a.release.o" "$probe/fatal-b.release.o" >&2
	stat -f '%N %z bytes' "$probe/fatal-a.release.o" "$probe/fatal-b.release.o" >&2
	exit 1
fi
shasum -a 256 "$probe/fatal-a.release.o" "$probe/fatal-b.release.o" >"$probe/fatal-object.sha256"
wc -c "$probe/fatal-a.release.o" "$probe/fatal-b.release.o" >"$probe/fatal-object.sizes"
strings -a "$probe/fatal-a.release.o" | grep -F '/cody/mapped/libcody' >/dev/null ||
	fail 'fatal.o does not contain its mapped source prefix'

# Compare the actual checksum input library, not only its corrected object.
# ZERO_AR_DATE makes Darwin ar deterministic; GNU ar already defaults to D.
for copy in a b; do
	if [ "$copy" = a ]; then maps=$maps_a; build=$build_a; else maps=$maps_b; build=$build_b; fi
	env AROS_GCC_RELEASE_LAYOUT=yes ZERO_AR_DATE=1 \
		AROS_GCC_RELEASE_PREFIX_MAPS="$maps" \
		AROS_GCC_RELEASE_CODY_SRCDIR_HELPER="$helper" \
		"$make_program" --no-print-directory -C "$build" libcody.a >"$probe/library-$copy.log" 2>&1
	cp "$build/libcody.a" "$probe/libcody-$copy.release.a"
done
cmp "$probe/libcody-a.release.a" "$probe/libcody-b.release.a" ||
	fail 'mapped complete libcody archives differ across physical source roots'
shasum -a 256 "$probe/libcody-a.release.a" "$probe/libcody-b.release.a" >"$probe/library.sha256"
wc -c "$probe/libcody-a.release.a" "$probe/libcody-b.release.a" >"$probe/library.sizes"

if [ -n "$generator" ]; then
	"$generator" "$probe/fatal-a.release.o" >"$probe/fatal-a.checksum"
	"$generator" "$probe/fatal-b.release.o" >"$probe/fatal-b.checksum"
	cmp "$probe/fatal-a.checksum" "$probe/fatal-b.checksum" ||
		fail 'native genchecksum differs for byte-identical mapped fatal.o objects'
	"$generator" "$probe/libcody-a.release.a" >"$probe/library-a.checksum"
	"$generator" "$probe/libcody-b.release.a" >"$probe/library-b.checksum"
	cmp "$probe/library-a.checksum" "$probe/library-b.checksum" ||
		fail 'native genchecksum differs for byte-identical complete libcody archives'
fi

printf '%s\n' \
	'#include "internal.hh"' \
	'int main ()' \
	'{' \
	'  Cody::AssertFailed (Cody::Location (__FILE__, 271));' \
	'}' >"$probe/assertion-probe.cc"
cp "$probe/assertion-probe.cc" "$source_a/assertion.cc"
cp "$probe/assertion-probe.cc" "$source_b/assertion.cc"
cmp "$source_a/assertion.cc" "$source_b/assertion.cc" || fail 'assertion sources differ across roots'

compile_assertion_probe() {
	map_flags=$1
	source_root=$2
	build_root=$3
	output=$4
	(
		cd "$build_root"
		# The maps are assembled from validated, space-free temporary roots.
		# shellcheck disable=SC2086
		"$cxx" -std=c++11 $map_flags "$semantic_flag" -include config.h \
			-I"$source_root" "$source_root/assertion.cc" fatal.o -o "$output"
	)
}
compile_assertion_probe "$maps_a" "$source_a" "$build_a" "$probe/assertion-a"
compile_assertion_probe "$maps_b" "$source_b" "$build_b" "$probe/assertion-b"

run_assertion_probe() {
	binary=$1
	output=$2
	set +e
	# A core file is not useful here; the signal status and stderr are the assertions.
	# shellcheck disable=SC3045
	(ulimit -c 0; "$binary") >"$output" 2>&1
	status=$?
	set -e
	assert_equal 'Cody assertion terminates with SIGABRT status' 134 "$status"
	printf '%s\n' "$status" >"$output.status"
	grep -F 'CODYlib: assertion failed at assertion.cc:271' "$output" >/dev/null || {
		printf 'FAIL: assertion diagnostic lost its compiler-mapped prefix trim\n' >&2
		cat "$output" >&2
		exit 1
	}
}
run_assertion_probe "$probe/assertion-a" "$probe/assertion-a.out"
run_assertion_probe "$probe/assertion-b" "$probe/assertion-b.out"

ambiguous_maps_a="-ffile-prefix-map=$source_a=/cody/specific -fmacro-prefix-map=$source_parent_a=/cody/mapped -fdebug-prefix-map=$probe/a=/debug-only"
expect_helper_failure 'ambiguous overlapping file/macro maps' "$source_a" "$ambiguous_maps_a"
if ! grep -F 'ambiguous diagnostic namespace' "$probe/helper.err" >/dev/null; then
	fail 'ambiguous overlapping maps failed without the expected diagnostic'
fi
same_result_maps_a="-ffile-prefix-map=$source_a=/cody/mapped/libcody -fmacro-prefix-map=$source_parent_a=/cody/mapped -fdebug-prefix-map=$probe/a=/debug-only"
assert_equal 'overlapping file/macro maps that agree are accepted' \
	'/cody/mapped/libcody' \
	"$(env AROS_GCC_RELEASE_LAYOUT=yes AROS_GCC_RELEASE_CODY_SRCDIR="$source_a" \
		AROS_GCC_RELEASE_PREFIX_MAPS="$same_result_maps_a" awk -f "$helper" </dev/null)"
changed_maps="-ffile-prefix-map=$source_a=/cody/changed/libcody -fmacro-prefix-map=$source_parent_a=/cody/changed -fdebug-prefix-map=$probe/a=/debug-only"
assert_equal 'a changed logical destination stays visible' \
	'/cody/changed/libcody' \
	"$(env AROS_GCC_RELEASE_LAYOUT=yes AROS_GCC_RELEASE_CODY_SRCDIR="$source_a" \
		AROS_GCC_RELEASE_PREFIX_MAPS="$changed_maps" awk -f "$helper" </dev/null)"

expect_helper_failure 'debug-only match' "$source_a" "-fdebug-prefix-map=$probe/a=/debug-only"
expect_helper_failure 'unmatched source map' "$source_a" '-ffile-prefix-map=/unrelated/source=/cody'
expect_helper_failure 'filesystem-root map' "$source_a" '-ffile-prefix-map=/=/cody'
expect_helper_failure 'relative source map' "$source_a" '-ffile-prefix-map=relative=/cody'
expect_helper_failure 'duplicate map' "$source_a" "-ffile-prefix-map=$source_a=/cody -ffile-prefix-map=$source_a=/cody"
expect_helper_failure 'conflicting same-kind source map' "$source_a" \
	"-ffile-prefix-map=$source_parent_a=/cody/one -ffile-prefix-map=$source_parent_a=/cody/two"
expect_helper_failure 'quoted source map' "$source_a" "-ffile-prefix-map=/producer/bad'quote=/cody"
control_maps=$(printf '%s\n' '-ffile-prefix-map=/producer/a=/cody' '-fdebug-prefix-map=/producer/b=/debug')
expect_helper_failure 'control character in map list' "$source_a" "$control_maps"
expect_helper_failure 'filesystem-root source directory' '/' '-ffile-prefix-map=/producer=/cody'

expect_make_failure 'debug-only release mapping' "-fdebug-prefix-map=$probe/a=/debug" "$helper"
expect_make_failure 'ambiguous release mapping' "$ambiguous_maps_a" "$helper"
expect_make_failure 'missing helper path' "$maps_a" "$probe/missing-helper.awk"
expect_make_failure 'missing helper variable' "$maps_a" ''

rm -f "$build_a/fatal.o"
env AROS_GCC_RELEASE_LAYOUT=no \
	AROS_GCC_RELEASE_PREFIX_MAPS="$maps_a" \
	AROS_GCC_RELEASE_CODY_SRCDIR_HELPER= \
	"$make_program" --no-print-directory -C "$build_a" fatal.o >"$probe/classic.log" 2>&1
physical_define="-DSRCDIR='\"$source_a\"'"
grep -F -- "$physical_define" "$probe/classic.log" >/dev/null || {
	printf 'FAIL: classic rule did not keep the upstream physical SRCDIR\n' >&2
	cat "$probe/classic.log" >&2
	exit 1
}

# Preserve release archives above, then prove the comparison detects the old
# physical macro rather than passing regardless of diagnostic-path contents.
rm -f "$build_b/fatal.o"
for copy in a b; do
	if [ "$copy" = a ]; then build=$build_a; else build=$build_b; fi
	env AROS_GCC_RELEASE_LAYOUT=no ZERO_AR_DATE=1 \
		"$make_program" --no-print-directory -C "$build" libcody.a >"$probe/classic-library-$copy.log" 2>&1
	cp "$build/libcody.a" "$probe/libcody-$copy.classic.a"
done
set +e
cmp "$probe/libcody-a.classic.a" "$probe/libcody-b.classic.a" >"$probe/classic-library-comparison.log" 2>&1
classic_comparison_status=$?
set -e
assert_equal 'Classic complete archives retain physical-root differences' 1 "$classic_comparison_status"

printf '%s\n' 'Patched libcody SRCDIR object/library/checksum and Classic counterprobes passed.'
