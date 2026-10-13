#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
patch_file=$test_dir/../gcc-16.2.0-aros.diff
gcc_source=${1:-${AROS_GCC_TEST_SOURCE:-}}

if [ "$#" -gt 1 ]; then
	printf '%s\n' "Usage: $0 [GCC-source-directory]" >&2
	exit 2
fi
if [ -z "$gcc_source" ]; then
	printf '%s\n' "Usage: $0 [GCC-source-directory] (or set AROS_GCC_TEST_SOURCE)" >&2
	exit 2
fi
if [ ! -d "$gcc_source" ]; then
	printf '%s\n' "FAIL: GCC source directory not found: $gcc_source" >&2
	exit 1
fi
gcc_source=$(CDPATH='' cd "$gcc_source" && pwd)
if [ ! -f "$patch_file" ]; then
	printf '%s\n' "FAIL: GCC AROS patch not found: $patch_file" >&2
	exit 1
fi

task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/aros-gnu-recorded-metadata.XXXXXX")
task_tmp=$(CDPATH='' cd -P "$task_tmp" && pwd -P)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
tree=$task_tmp/tree
mkdir -p "$tree/gcc" "$tree/fixincludes"

# These are the eight pre-existing files changed by the metadata patch. The
# ninth path, gcc/aros-config-record.awk, is created by applying the patch.
for relative_file in \
	gcc/configure.ac \
	gcc/configure \
	gcc/Makefile.in \
	fixincludes/configure.ac \
	fixincludes/configure \
	fixincludes/config.h.in \
	fixincludes/fixincl.c \
	fixincludes/mkheaders.in
do
	if [ ! -f "$gcc_source/$relative_file" ]; then
		printf '%s\n' "FAIL: expected GCC fixture source is missing: $relative_file" >&2
		exit 1
	fi
	cp "$gcc_source/$relative_file" "$tree/$relative_file"
done

metadata_patch=$task_tmp/metadata.patch
if ! awk '
	index($0, "diff -ruN gcc-16.2.0/gcc/aros-config-record.awk") == 1 {
		found = 1
	}
	found { print }
	END { if (!found) exit 1 }
' "$patch_file" > "$metadata_patch"; then
	printf '%s\n' 'FAIL: recorded-metadata patch section is not present yet' >&2
	exit 1
fi
patch --batch --forward --fuzz=0 -s -p1 -d "$tree" < "$metadata_patch"

fail() {
	printf 'FAIL: %s\n' "$1" >&2
	exit 1
}

assert_equal() {
	label=$1
	expected=$2
	actual=$3
	if [ "$actual" != "$expected" ]; then
		printf 'FAIL: %s\nexpected: <%s>\nactual:   <%s>\n' \
			"$label" "$expected" "$actual" >&2
		exit 1
	fi
}

assert_contains() {
	label=$1
	value=$2
	needle=$3
	case "$value" in
		*"$needle"*) ;;
		*) printf 'FAIL: %s; missing <%s>\nactual: <%s>\n' "$label" "$needle" "$value" >&2; exit 1 ;;
	esac
}

assert_not_contains() {
	label=$1
	value=$2
	needle=$3
	case "$value" in
		*"$needle"*) printf 'FAIL: %s; unexpected <%s>\nactual: <%s>\n' "$label" "$needle" "$value" >&2; exit 1 ;;
		*) ;;
	esac
}

record_helper=$tree/gcc/aros-config-record.awk
build_root="$task_tmp/build.[release] \\literal"
source_root="$build_root/source (nested) +\\root"
configure_input="--build-root=$build_root/output --source-root=$source_root/compiler"
mapped_arguments=$(printf '%s' "$configure_input" | \
	AROS_GCC_RELEASE_BUILD_ROOT="$build_root" \
	AROS_GCC_RELEASE_SOURCE_ROOT="$source_root" \
	awk -f "$record_helper")
expected_arguments='--build-root=/aros-build/output --source-root=/aros-source/compiler'
assert_equal 'literal longest-root-first remapping preserves spaces, backslashes, and regex characters' \
	"$expected_arguments" "$mapped_arguments"
configure_input_with_newlines="${configure_input}

"
mapped_arguments_with_newlines=$(printf '%sX' "$configure_input_with_newlines" | \
	AROS_GCC_RELEASE_BUILD_ROOT="$build_root" \
	AROS_GCC_RELEASE_SOURCE_ROOT="$source_root" \
	awk -f "$record_helper")
mapped_arguments_with_newlines=${mapped_arguments_with_newlines%X}
expected_arguments_with_newlines="${expected_arguments}

"
assert_equal 'record helper preserves trailing newlines through the X sentinel' \
	"$expected_arguments_with_newlines" "$mapped_arguments_with_newlines"

for invalid_root in relative/path /; do
	if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT="$invalid_root" \
		AROS_GCC_RELEASE_SOURCE_ROOT="$source_root" \
		awk -f "$record_helper" >/dev/null 2>&1; then
		fail "record helper accepted malformed build root <$invalid_root>"
	fi
	if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT="$build_root" \
		AROS_GCC_RELEASE_SOURCE_ROOT="$invalid_root" \
		awk -f "$record_helper" >/dev/null 2>&1; then
		fail "record helper accepted malformed source root <$invalid_root>"
	fi
done
if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT='' \
	AROS_GCC_RELEASE_SOURCE_ROOT="$source_root" \
	awk -f "$record_helper" >/dev/null 2>&1; then
	fail 'record helper accepted an empty build root'
fi
if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT="$build_root" \
	AROS_GCC_RELEASE_SOURCE_ROOT='' \
	awk -f "$record_helper" >/dev/null 2>&1; then
	fail 'record helper accepted an empty source root'
fi
newline_root="$task_tmp/newline
root"
if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT="$newline_root" \
	AROS_GCC_RELEASE_SOURCE_ROOT="$source_root" \
	awk -f "$record_helper" >/dev/null 2>&1; then
	fail 'record helper accepted a build root containing a newline'
fi
if printf '%s' test | AROS_GCC_RELEASE_BUILD_ROOT="$build_root" \
	AROS_GCC_RELEASE_SOURCE_ROOT="$newline_root" \
	awk -f "$record_helper" >/dev/null 2>&1; then
	fail 'record helper accepted a source root containing a newline'
fi

extract_configure_block() {
	input_file=$1
	output_file=$2
	if ! awk '
		/# Compile in configure arguments\./ { inside = 1 }
		inside && /gcc_BASEVER=/ { exit }
		inside && /changequote\(/ { next }
		inside { print }
		END { if (!inside) exit 1 }
	' "$input_file" > "$output_file"; then
		fail "could not extract configure-arguments block from $input_file"
	fi
}

normalize_configure_errors() {
	awk '
		/\|\| AC_MSG_ERROR/ { sub(/\|\| AC_MSG_ERROR.*/, "|| CONFIG_ERROR") }
		/\|\| as_fn_error/ { sub(/\|\| as_fn_error.*/, "|| CONFIG_ERROR") }
		{ print }
	' "$1"
}

configure_block=$task_tmp/configure-recorded-arguments.sh
configure_ac_block=$task_tmp/configure-ac-recorded-arguments.sh
extract_configure_block "$tree/gcc/configure" "$configure_block"
extract_configure_block "$tree/gcc/configure.ac" "$configure_ac_block"
normalized_configure=$(normalize_configure_errors "$configure_block")
normalized_configure_ac=$(normalize_configure_errors "$configure_ac_block")
assert_equal 'configure.ac and generated configure keep the recorded-arguments block in sync' \
	"$normalized_configure_ac" "$normalized_configure"

# The extracted Autoconf shell block consumes these variables through eval.
# shellcheck disable=SC2034,SC2154,SC2209,SC2329
run_configure_block() {
	run_dir=$1
	top_arguments=$2
	release_layout=$3
	build_path=$4
	source_path=$5
	previous_configargs=${6:-}
	mkdir -p "$run_dir"
	if [ -n "$previous_configargs" ]; then
		cp "$previous_configargs" "$run_dir/configargs.h"
	fi
	(
		cd "$run_dir"
		srcdir=$tree/gcc
		AWK='awk'
		thread_file=posix
		configure_default_options='{ { "record-test", "preserved" } }'
		TOPLEVEL_CONFIGURE_ARGUMENTS=$top_arguments
		AROS_GCC_RELEASE_LAYOUT=$release_layout
		AROS_GCC_RELEASE_BUILD_ROOT=$build_path
		AROS_GCC_RELEASE_SOURCE_ROOT=$source_path
		export AROS_GCC_RELEASE_BUILD_ROOT AROS_GCC_RELEASE_SOURCE_ROOT
		as_fn_error() {
			printf 'configure block rejected its input: %s\n' "$*" >&2
			exit 1
		}
		eval "$(cat "$configure_block")"
		printf '%s' "$TOPLEVEL_CONFIGURE_ARGUMENTS" > top-level-after
		printf '%s' "$gcc_config_arguments" > gcc-config-arguments-after
	)
}

configuration_arguments() {
	grep -F 'static const char configuration_arguments[] =' "$1" |
		sed 's/^.* = "//; s/";$//'
}

classic_arguments='--host=aarch64-apple-darwin --prefix=/classic/prefix'
run_configure_block "$task_tmp/classic" "$classic_arguments" no '' ''
classic_record=$(configuration_arguments "$task_tmp/classic/configargs.h")
assert_equal 'classic configure records TOPLEVEL arguments byte-for-byte' \
	"$classic_arguments" "$classic_record"
assert_equal 'configure block leaves classic TOPLEVEL arguments unchanged' \
	"$classic_arguments" "$(sed -n '1p' "$task_tmp/classic/top-level-after")"
assert_contains 'configure block initializes the thread model in configargs.h' \
	"$(sed -n '1,8p' "$task_tmp/classic/configargs.h")" 'thread_model[] = "posix";'
assert_contains 'configure block preserves configure defaults in configargs.h' \
	"$(sed -n '1,8p' "$task_tmp/classic/configargs.h")" 'record-test'

raw_release_arguments="--build-root=$build_root/output --source-root=$source_root/compiler"
run_configure_block "$task_tmp/release-first" "$raw_release_arguments" yes \
	"$build_root" "$source_root"
first_release_record=$(configuration_arguments "$task_tmp/release-first/configargs.h")
expected_release_arguments='--build-root=/aros-build/output --source-root=/aros-source/compiler'
assert_equal 'release configure records literal canonical build and source roots' \
	"$expected_release_arguments" "$first_release_record"
assert_not_contains 'release configure record excludes the physical build root' \
	"$first_release_record" "$build_root"
assert_not_contains 'release configure record excludes the physical source root' \
	"$first_release_record" "$source_root"
assert_equal 'release configure leaves raw TOPLEVEL arguments unchanged' \
	"$raw_release_arguments" "$(sed -n '1p' "$task_tmp/release-first/top-level-after")"

raw_release_arguments_with_newlines="${raw_release_arguments}

"
run_configure_block "$task_tmp/release-newlines" "$raw_release_arguments_with_newlines" yes \
	"$build_root" "$source_root"
printf '%s\n\n' "$raw_release_arguments" > "$task_tmp/expected-raw-release-arguments"
printf '%s\n\n' "$expected_release_arguments" > "$task_tmp/expected-mapped-release-arguments"
if ! cmp -s "$task_tmp/expected-raw-release-arguments" \
	"$task_tmp/release-newlines/top-level-after"; then
	fail 'release configure changed trailing newlines in TOPLEVEL arguments'
fi
if ! cmp -s "$task_tmp/expected-mapped-release-arguments" \
	"$task_tmp/release-newlines/gcc-config-arguments-after"; then
	fail 'release configure lost trailing newlines while remapping its recorded arguments'
fi

run_configure_block "$task_tmp/release-second" "$raw_release_arguments" yes \
	"$build_root" "$source_root" "$task_tmp/release-first/configargs.h"
second_release_record=$(configuration_arguments "$task_tmp/release-second/configargs.h")
assert_contains 'reconfigure history stores mapped raw arguments' \
	"$second_release_record" " : (reconfigured) $expected_release_arguments"
assert_equal 'second release configure leaves raw TOPLEVEL arguments unchanged' \
	"$raw_release_arguments" "$(sed -n '1p' "$task_tmp/release-second/top-level-after")"

run_configure_block "$task_tmp/release-third" "$raw_release_arguments" yes \
	"$build_root" "$source_root" "$task_tmp/release-second/configargs.h"
third_release_record=$(configuration_arguments "$task_tmp/release-third/configargs.h")
assert_equal 'identical repeated release configure arguments do not grow history' \
	"$second_release_record" "$third_release_record"

invalid_root_output=$task_tmp/release-invalid-root.out
if run_configure_block "$task_tmp/release-invalid-root" "$raw_release_arguments" yes \
	'relative/build-root' "$source_root" > "$invalid_root_output" 2>&1; then
	fail 'configure block accepted a relative release build root'
fi
assert_contains 'configure block reports a malformed release root' \
	"$(sed -n '1,4p' "$invalid_root_output")" 'must be absolute non-root paths'
if [ -f "$task_tmp/release-invalid-root/configargs.h" ]; then
	fail 'configure block wrote configargs.h after rejecting a malformed root'
fi

# Extract and run GCC's actual install-mkheaders rule with a make fixture. The
# physical build header directory remains separate from the installed SDK dir.
extract_install_system_header_block() {
	if ! awk '
		/^INSTALL_SYSTEM_HEADER_DIR=/ { capturing = 1 }
		capturing { print }
		capturing && /^fi$/ { found = 1; exit }
		END { if (!found) exit 1 }
	' "$tree/gcc/configure" > "$task_tmp/install-system-header-dir.sh"; then
		fail 'could not extract INSTALL_SYSTEM_HEADER_DIR from generated configure'
	fi
}

extract_install_system_header_block
# The generated configure block assigns this value through eval.
# shellcheck disable=SC2030,SC2034,SC2153
install_system_header_dir=$(
	(
		SYSTEM_HEADER_DIR=/physical-sdk/include
		NATIVE_SYSTEM_HEADER_DIR=/include
		AROS_GCC_RELEASE_LAYOUT=yes
		eval "$(cat "$task_tmp/install-system-header-dir.sh")"
		printf '%s' "$INSTALL_SYSTEM_HEADER_DIR"
	)
)
# shellcheck disable=SC2016
assert_equal 'release configure gives the install rule a neutral SDK header directory' \
	'$${sysroot_headers_suffix}$(NATIVE_SYSTEM_HEADER_DIR)' "$install_system_header_dir"

extract_install_mkheaders_rule() {
	if ! awk '
		/^install-mkheaders:/ { capturing = 1; found = 1 }
		capturing && /^[^[:space:]]/ && !/^install-mkheaders:/ { exit }
		capturing { print }
		END { if (!found) exit 1 }
	' "$tree/gcc/Makefile.in" > "$task_tmp/install-mkheaders-rule.mk"; then
		fail 'could not extract the install-mkheaders rule from gcc/Makefile.in'
	fi
}

extract_install_mkheaders_rule
rule_inputs=$task_tmp/install-rule-inputs
rule_source=$rule_inputs/source
rule_dest=$task_tmp/install-rule-dest
rule_itoolsdatadir=/lib/gcc/riscv64-aros/16.2.0/install-tools
rule_itoolsdir=/libexec/gcc/riscv64-aros/16.2.0/install-tools
mkdir -p "$rule_source" "$rule_inputs/include" \
	"$rule_dest$rule_itoolsdatadir" "$rule_dest$rule_itoolsdir"
printf '%s\n' 'generic limits' > "$rule_source/gsyslimits.h"
printf '%s\n' 'macro probe' > "$rule_inputs/macro_list"
printf '%s\n' '.;' > "$rule_inputs/fixinc_list"
printf '%s\n' 'installed limits' > "$rule_inputs/include/limits.h"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$rule_inputs/mkinstalldirs"
chmod +x "$rule_inputs/mkinstalldirs"
rule_makefile=$task_tmp/install-mkheaders-fixture.mk
{
	printf '%s\n' \
		'.PHONY: stmp-int-hdrs install-itoolsdirs macro_list fixinc_list' \
		'stmp-int-hdrs install-itoolsdirs macro_list fixinc_list:'
	awk '{ print }' "$task_tmp/install-mkheaders-rule.mk"
	# shellcheck disable=SC2016
	printf '\nprobe-layout:\n\t@printf "BUILD_SYSTEM_HEADER_DIR=%%s\\n" "$(BUILD_SYSTEM_HEADER_DIR)"\n\t@printf "SYSTEM_HEADER_DIR=%%s\\n" "$(SYSTEM_HEADER_DIR)"\n\t@printf "INSTALL_SYSTEM_HEADER_DIR=%%s\\n" "$(INSTALL_SYSTEM_HEADER_DIR)"\n'
} > "$rule_makefile"
make_bin=${MAKE:-make}
# shellcheck disable=SC2016
rule_output=$(
	cd "$rule_inputs"
	"$make_bin" --no-print-directory --silent -f "$rule_makefile" \
		install-mkheaders probe-layout \
		srcdir="$rule_source" DESTDIR="$rule_dest" \
		itoolsdatadir="$rule_itoolsdatadir" itoolsdir="$rule_itoolsdir" \
		mkinstalldirs='mkdir -p' INSTALL_DATA=cp INSTALL_SCRIPT=cp \
		SYSTEM_HEADER_DIR=/physical-sdk/include \
		BUILD_SYSTEM_HEADER_DIR=/physical-build/include \
		NATIVE_SYSTEM_HEADER_DIR=/include \
		INSTALL_SYSTEM_HEADER_DIR='$${sysroot_headers_suffix}$(NATIVE_SYSTEM_HEADER_DIR)' \
		OTHER_FIXINCLUDES_DIRS= STMP_FIXINC=yes
)
assert_contains 'install-mkheaders leaves the build header directory physical' \
	"$rule_output" 'BUILD_SYSTEM_HEADER_DIR=/physical-build/include'
assert_contains 'install-mkheaders leaves the build-system header variable physical' \
	"$rule_output" 'SYSTEM_HEADER_DIR=/physical-sdk/include'
assert_contains 'install-mkheaders expands the release install header directory to /include' \
	"$rule_output" 'INSTALL_SYSTEM_HEADER_DIR=/include'
rule_mkheaders_conf=$rule_dest$rule_itoolsdatadir/mkheaders.conf
# shellcheck disable=SC2016
assert_contains 'install-mkheaders records the deferred SDK header suffix' \
	"$(sed -n '1,3p' "$rule_mkheaders_conf")" \
	'SYSTEM_HEADER_DIR="${sysroot_headers_suffix}/include"'
# shellcheck disable=SC2034,SC2031,SC1090
resolved_install_header_dir=$(
	sysroot_headers_suffix=''
	. "$rule_mkheaders_conf"
	printf '%s' "$SYSTEM_HEADER_DIR"
)
assert_equal 'installed mkheaders metadata resolves the empty suffix to /include' \
	'/include' "$resolved_install_header_dir"

# This compiles and runs only fixincl.c's actual conditional preamble with a
# stub context; it does not compile the fixincl generator.
if ! command -v cc >/dev/null 2>&1; then
	fail 'C compiler is required for the isolated fixincl preamble probe'
fi
fixincl_preamble=$task_tmp/fixincl-preamble.c
{
	printf '%s\n' \
		'#include <stdio.h>' \
		'static const char z_std_preamble[] = "%s/%s\n";' \
		'int main(void)' \
		'{' \
		'  FILE *pf = stdout;' \
		'  const char *pz_machine = "riscv64-aros";' \
		'  const char *pz_curr_file = "stdio.h";' \
		'  const char *pz_input_dir = "/build-physical-prefix/sdk/include";'
	awk '
		/fprintf \(pf, z_std_preamble,/ { capturing = 1 }
		capturing { print }
		capturing && /pz_curr_file\);/ { found = 1; exit }
		END { if (!found) exit 1 }
	' "$tree/fixincludes/fixincl.c"
	printf '%s\n' '  return 0;' '}'
} > "$fixincl_preamble"
cc -o "$task_tmp/fixincl-preamble-classic" "$fixincl_preamble"
cc -DAROS_GCC_RELEASE_LAYOUT -o "$task_tmp/fixincl-preamble-release" "$fixincl_preamble"
classic_preamble=$("$task_tmp/fixincl-preamble-classic")
release_preamble=$("$task_tmp/fixincl-preamble-release")
assert_equal 'classic fixincl preamble retains its physical input path' \
	'/build-physical-prefix/sdk/include/stdio.h' "$classic_preamble"
assert_equal 'release fixincl preamble uses the canonical SDK include path' \
	'/aros-sdk/include/stdio.h' "$release_preamble"
assert_not_contains 'release fixincl preamble excludes the build path' \
	"$release_preamble" '/build-physical-prefix'

mkheaders_template=$task_tmp/mkheaders.in
cp "$tree/fixincludes/mkheaders.in" "$mkheaders_template"

replace_template_token() {
	template_file=$1
	template_token=$2
	template_value=$3
	template_value=$(printf '%s' "$template_value" | sed 's/[\\&|]/\\&/g')
	template_next=$template_file.next
	sed "s|@$template_token@|$template_value|g" "$template_file" > "$template_next"
	mv "$template_next" "$template_file"
}

replace_template_token "$mkheaders_template" target riscv64-aros
replace_template_token "$mkheaders_template" target_noncanonical riscv64-aros
replace_template_token "$mkheaders_template" gcc_version 16.2.0
replace_template_token "$mkheaders_template" prefix /build-physical-prefix
replace_template_token "$mkheaders_template" exec_prefix /build-physical-prefix
replace_template_token "$mkheaders_template" libdir /build-physical-prefix/lib
replace_template_token "$mkheaders_template" libexecdir /build-physical-prefix/libexec
replace_template_token "$mkheaders_template" SHELL /bin/sh
replace_template_token "$mkheaders_template" aros_release_layout yes
if grep -q '@[^@]*@' "$mkheaders_template"; then
	fail 'mkheaders template contains an unsubstituted configure parameter'
fi

mkheaders_script=$task_tmp/mkheaders
cp "$mkheaders_template" "$mkheaders_script"
chmod +x "$mkheaders_script"
relocated_prefix=$task_tmp/relocated-prefix
relocated_sdk=$task_tmp/relocated-sdk
missing_sdk=$task_tmp/missing-sdk
target=riscv64-aros
version=16.2.0
libsubdir=$relocated_prefix/lib/gcc/$target/$version
libexecsubdir=$relocated_prefix/libexec/gcc/$target/$version
itoolsdatadir=$libsubdir/install-tools
itoolsdir=$libexecsubdir/install-tools
incdir=$libsubdir/include-fixed
mkdir -p "$itoolsdatadir/include" "$itoolsdir" "$incdir"
printf '%s\n' '.;' > "$itoolsdatadir/fixinc_list"
printf '%s\n' 'macro probe' > "$itoolsdatadir/macro_list"
printf '%s\n' 'generic limits' > "$itoolsdatadir/gsyslimits.h"
printf '%s\n' 'installed limits' > "$itoolsdatadir/include/limits.h"
printf '%s\n' \
	'SYSTEM_HEADER_DIR="/include"' \
	'OTHER_FIXINCLUDES_DIRS=""' \
	'STMP_FIXINC="yes"' > "$itoolsdatadir/mkheaders.conf"
printf '%s\n' 'keep before validation' > "$incdir/keep.me"

mkdirs_record=$task_tmp/mkinstalldirs.log
fixinc_record=$task_tmp/fixinc.log
: > "$mkdirs_record"
: > "$fixinc_record"
# shellcheck disable=SC2016
printf '%s\n' \
	'#!/bin/sh' \
	'printf "%s\n" "$*" >> "$MKDIRS_RECORD"' \
	'if [ "$#" -gt 0 ]; then mkdir -p "$@"; fi' > "$itoolsdir/mkinstalldirs"
# shellcheck disable=SC2016
printf '%s\n' \
	'#!/bin/sh' \
	'printf "destination=<%s> sdk=<%s>\n" "$1" "$2" >> "$FIXINC_RECORD"' \
	'printf "%s\n" "fixed limits" > "$1/limits.h"' > "$itoolsdir/fixinc.sh"
chmod +x "$itoolsdir/mkinstalldirs" "$itoolsdir/fixinc.sh"

missing_sdk_output=$task_tmp/mkheaders-missing-sdk.out
if CONFIG_SHELL=/bin/sh SHELL=/bin/sh MKDIRS_RECORD="$mkdirs_record" \
	FIXINC_RECORD="$fixinc_record" "$mkheaders_script" "$relocated_prefix" \
	"$missing_sdk" > "$missing_sdk_output" 2>&1; then
	fail 'mkheaders accepted an SDK without an include directory'
fi
assert_contains 'mkheaders explains missing SDK rejection' \
	"$(sed -n '1,4p' "$missing_sdk_output")" 'SDK include directory is missing'
if [ ! -f "$incdir/keep.me" ]; then
	fail 'mkheaders removed installed headers before rejecting the missing SDK'
fi
if [ -s "$fixinc_record" ]; then
	fail 'mkheaders ran fixinc.sh after rejecting the missing SDK'
fi

mkdir -p "$relocated_sdk/include"
if ! CONFIG_SHELL=/bin/sh SHELL=/bin/sh MKDIRS_RECORD="$mkdirs_record" \
	FIXINC_RECORD="$fixinc_record" "$mkheaders_script" "$relocated_prefix" \
	"$relocated_sdk" > "$task_tmp/mkheaders-relocated.out" 2>&1; then
	cat "$task_tmp/mkheaders-relocated.out" >&2
	fail 'mkheaders did not use the explicit relocated prefix and SDK'
fi
fixinc_output=$(sed -n '1,4p' "$fixinc_record")
assert_contains 'fixinc.sh receives the actual relocated SDK include directory' \
	"$fixinc_output" "sdk=<$relocated_sdk/include>"
assert_contains 'mock mkinstalldirs created the relocated include-fixed directory' \
	"$(sed -n '1,8p' "$mkdirs_record")" "$incdir"
assert_not_contains 'relocated fixinc invocation omits the build-time prefix' \
	"$fixinc_output" '/build-physical-prefix'
assert_equal 'mkheaders preserves fixed output limits' \
	'fixed limits' "$(sed -n '1p' "$incdir/syslimits.h")"
assert_equal 'mkheaders installs its supplied limits.h file' \
	'installed limits' "$(sed -n '1p' "$incdir/limits.h")"

printf '%s\n' 'keep before root-prefix validation' > "$incdir/keep.me"
root_symlink=$task_tmp/root-symlink
ln -s / "$root_symlink"
for invalid_prefix in /. /.. // /// "$root_symlink"; do
	: > "$mkdirs_record"
	: > "$fixinc_record"
	if CONFIG_SHELL=/bin/sh SHELL=/bin/sh MKDIRS_RECORD="$mkdirs_record" \
		FIXINC_RECORD="$fixinc_record" "$mkheaders_script" "$invalid_prefix" \
		"$relocated_sdk" > "$task_tmp/mkheaders-root-prefix.out" 2>&1; then
		fail "mkheaders accepted prefix resolving to /: $invalid_prefix"
	fi
	assert_contains 'mkheaders identifies a prefix resolving to the filesystem root' \
		"$(sed -n '1,4p' "$task_tmp/mkheaders-root-prefix.out")" \
		'compiler prefix resolves to the filesystem root'
	if [ ! -f "$incdir/keep.me" ]; then
		fail "mkheaders deleted installed headers for root-equivalent prefix: $invalid_prefix"
	fi
	if [ -s "$mkdirs_record" ] || [ -s "$fixinc_record" ]; then
		fail "mkheaders ran maintenance helpers for root-equivalent prefix: $invalid_prefix"
	fi
done

printf '%s\n' 'GNU recorded-metadata regression probes passed.'
