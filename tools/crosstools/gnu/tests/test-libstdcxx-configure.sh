#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
source_root=$(CDPATH='' cd "$test_dir/../../../.." && pwd)
patch_file=$source_root/tools/crosstools/gnu/gcc-16.2.0-aros.diff

if [ "$#" -ne 1 ]; then
	printf '%s\n' "Usage: $0 /path/to/gcc-16.2.0.tar.xz" >&2
	exit 2
fi
gcc_archive=$1
if [ ! -f "$gcc_archive" ] || [ ! -r "$gcc_archive" ]; then
	printf '%s\n' "FAIL: GCC source archive is not readable: $gcc_archive" >&2
	exit 1
fi
if [ ! -f "$patch_file" ]; then
	printf '%s\n' "FAIL: GCC AROS patch not found: $patch_file" >&2
	exit 1
fi

task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/aros-gnu-libstdcxx-configure.XXXXXX")
task_tmp=$(CDPATH='' cd -P "$task_tmp" && pwd -P)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
tree=$task_tmp/tree
mkdir -p "$tree/libstdc++-v3"

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

assert_contains_file() {
	label=$1
	file=$2
	needle=$3
	if ! grep -Fq -- "$needle" "$file"; then
		printf 'FAIL: %s; missing <%s> in %s\n' "$label" "$needle" "$file" >&2
		exit 1
	fi
}

assert_not_contains_file() {
	label=$1
	file=$2
	needle=$3
	if grep -Fq -- "$needle" "$file"; then
		printf 'FAIL: %s; unexpected <%s> in %s\n' "$label" "$needle" "$file" >&2
		exit 1
	fi
}

extract_member() {
	member=$1
	destination=$2
	if ! tar -xOf "$gcc_archive" "gcc-16.2.0/$member" > "$destination"; then
		fail "could not extract GCC archive member gcc-16.2.0/$member"
	fi
}

extract_patch_section() {
	wanted=$1
	destination=$2
	if ! awk -v wanted="$wanted" '
		!active && index($0, wanted) == 1 { active = 1 }
		active && /^diff -ruN / && index($0, wanted) != 1 { exit }
		active { print }
		END { if (!active) exit 1 }
	' "$patch_file" > "$destination"; then
		fail "could not extract patch section $wanted"
	fi
}

extract_member libstdc++-v3/configure "$tree/libstdc++-v3/configure"
extract_member libstdc++-v3/crossconfig.m4 "$tree/libstdc++-v3/crossconfig.m4"
extract_member libstdc++-v3/configure.ac "$tree/libstdc++-v3/configure.ac"
extract_member libstdc++-v3/acinclude.m4 "$tree/libstdc++-v3/acinclude.m4"

extract_patch_section \
	'diff -ruN gcc-16.2.0/libstdc++-v3/configure ' \
	"$task_tmp/libstdcxx-configure.patch"
extract_patch_section \
	'diff -ruN gcc-16.2.0/libstdc++-v3/crossconfig.m4 ' \
	"$task_tmp/libstdcxx-crossconfig.patch"
patch --batch --forward --fuzz=0 -s -p1 -d "$tree" \
	< "$task_tmp/libstdcxx-configure.patch"
patch --batch --forward --fuzz=0 -s -p1 -d "$tree" \
	< "$task_tmp/libstdcxx-crossconfig.patch"

configure=$tree/libstdc++-v3/configure
crossconfig=$tree/libstdc++-v3/crossconfig.m4
configure_ac=$tree/libstdc++-v3/configure.ac
acinclude=$tree/libstdc++-v3/acinclude.m4

for obsolete_macro in \
	GLIBCXX_CHECK_COMPLEX_MATH_SUPPORT \
	GLIBCXX_CHECK_WCHAR_T_SUPPORT
do
	assert_not_contains_file 'patched crossconfig has no retired macro calls' \
		"$crossconfig" "$obsolete_macro"
	assert_not_contains_file 'patched configure has no unexpanded shell macro calls' \
		"$configure" "$obsolete_macro"
done

if ! awk '
	/^  \*-aros\*\)$/ { in_aros = 1 }
	in_aros { print }
	in_aros && /^    ;;$/ { complete = 1; exit }
	END { if (!complete) exit 1 }
' "$crossconfig" > "$task_tmp/aros-crossconfig.m4"; then
	fail 'could not isolate the AROS crossconfig M4 case'
fi
assert_contains_file 'AROS crossconfig keeps its header probes' \
	"$task_tmp/aros-crossconfig.m4" 'AC_CHECK_HEADERS([nan.h'
assert_contains_file 'AROS crossconfig keeps its known math capabilities' \
	"$task_tmp/aros-crossconfig.m4" 'AC_DEFINE(HAVE_ACOSF)'
assert_contains_file 'AROS crossconfig keeps its final math capability' \
	"$task_tmp/aros-crossconfig.m4" 'AC_DEFINE(HAVE_TANL)'
assert_not_contains_file 'AROS crossconfig does not add native link probes' \
	"$task_tmp/aros-crossconfig.m4" 'GLIBCXX_CHECK_MATH_SUPPORT'

# These common GCC 16.2 checks replace the retired calls for every target.
assert_contains_file 'configure.ac keeps the current wchar check' \
	"$configure_ac" 'GLIBCXX_ENABLE_WCHAR_T([yes])'
assert_contains_file 'configure.ac keeps the current C99 check' \
	"$configure_ac" 'GLIBCXX_ENABLE_C99([yes])'
assert_contains_file 'configure.ac keeps the current C99 TR1 check' \
	"$configure_ac" 'GLIBCXX_CHECK_C99_TR1'
assert_contains_file 'GCC 16.2 defines the current wchar check' \
	"$acinclude" 'AC_DEFUN([GLIBCXX_ENABLE_WCHAR_T]'
assert_contains_file 'GCC 16.2 defines the current C99 check' \
	"$acinclude" 'AC_DEFUN([GLIBCXX_ENABLE_C99]'
assert_contains_file 'GCC 16.2 defines the current C99 TR1 check' \
	"$acinclude" 'AC_DEFUN([GLIBCXX_CHECK_C99_TR1]'
assert_contains_file 'generated configure keeps the common wchar probe' \
	"$configure" 'for enabled wchar_t specializations'
assert_contains_file 'generated configure keeps the common C99 complex probe' \
	"$configure" 'for ISO C99 support in <complex.h> for C++98'
assert_contains_file 'generated configure keeps the common C99 TR1 complex probe' \
	"$configure" 'for ISO C99 support to TR1 in <complex.h>'

if ! awk '
	/^# Base decisions on target environment\.$/ { marker = 1; next }
	marker && /^case "\$\{host\}" in$/ { in_case = 1; next }
	in_case && /^  \*-aros\*\)$/ { in_aros = 1 }
	in_aros { print }
	in_aros && /^    ;;$/ { complete = 1; exit }
	END { if (!complete) exit 1 }
' "$configure" > "$task_tmp/aros-configure-case"; then
	fail 'could not isolate the generated AROS configure case'
fi
assert_contains_file 'generated AROS case keeps its math capabilities' \
	"$task_tmp/aros-configure-case" '#define HAVE_ACOSF 1'
assert_contains_file 'generated AROS case keeps its final math capability' \
	"$task_tmp/aros-configure-case" '#define HAVE_TANL 1'
assert_not_contains_file 'generated AROS case does not add native link probes' \
	"$task_tmp/aros-configure-case" 'GLIBCXX_CHECK_MATH_SUPPORT'

# Run the actual generated case with header checks stubbed. This exercises the
# selected shell branch without starting any compiler or configure probes.
mkdir -p "$task_tmp/positive"
{
	# shellcheck disable=SC2016
	printf '%s\n' \
		'#!/bin/sh' \
		'set -eu' \
		'host=riscv64-unknown-aros' \
		'LINENO=1' \
		'ac_includes_default=' \
		'as_echo=printf' \
		'as_tr_sh=to_shellvar' \
		'as_tr_cpp=to_macro' \
		'probe_log=$1' \
		'cd "$2"' \
		'to_shellvar() { tr "./-" "___"; }' \
		'to_macro() { LC_ALL=C tr "[:lower:]./" "[:upper:]__"; }' \
		'ac_fn_c_check_header_mongrel() {' \
		'  eval "$3=yes"' \
		'  printf "%s\\n" "$2" >> "$probe_log"' \
		'}' \
		'case "${host}" in'
	cat "$task_tmp/aros-configure-case"
	printf '%s\n' '  *) : ;;' 'esac'
} > "$task_tmp/run-aros-case.sh"
if ! /bin/sh "$task_tmp/run-aros-case.sh" \
	"$task_tmp/header-probes" "$task_tmp/positive" \
	> "$task_tmp/positive.stdout" 2> "$task_tmp/positive.stderr"; then
	cat "$task_tmp/positive.stderr" >&2
	fail 'generated AROS configure case failed in the shell fixture'
fi
if [ -s "$task_tmp/positive.stderr" ]; then
	cat "$task_tmp/positive.stderr" >&2
	fail 'generated AROS configure case emitted shell errors'
fi
probe_count=$(wc -l < "$task_tmp/header-probes" | tr -d '[:space:]')
if [ "$probe_count" != 12 ]; then
	fail "generated AROS header loop issued $probe_count probes, expected 12"
fi
assert_contains_file 'shell fixture executes AROS-specific definitions' \
	"$task_tmp/positive/confdefs.h" '#define HAVE_ACOSF 1'
assert_contains_file 'shell fixture executes the final AROS math definition' \
	"$task_tmp/positive/confdefs.h" '#define HAVE_TANL 1'

# Prove why leaving either old token in generated shell is fatal, separately
# from the successful patched-case execution above.
for obsolete_macro in \
	GLIBCXX_CHECK_COMPLEX_MATH_SUPPORT \
	GLIBCXX_CHECK_WCHAR_T_SUPPORT
do
	if /bin/sh -c "$obsolete_macro" > "$task_tmp/stale.stderr" 2>&1; then
		fail "stale macro counterprobe unexpectedly ran: $obsolete_macro"
	else
		stale_status=$?
	fi
	if [ "$stale_status" -ne 127 ]; then
		fail "stale macro counterprobe returned $stale_status, expected command-not-found status 127"
	fi
	assert_contains_file 'stale macro counterprobe identifies the unresolved command' \
		"$task_tmp/stale.stderr" "$obsolete_macro"
	assert_contains_file 'stale macro counterprobe reports command-not-found' \
		"$task_tmp/stale.stderr" 'not found'
done

printf '%s\n' 'GCC 16.2 AROS libstdc++ configure regression probes passed.'
