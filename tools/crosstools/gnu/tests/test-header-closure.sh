#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
source_root=$(CDPATH='' cd "$test_dir/../../../.." && pwd)
task_tmp=$(mktemp -d /tmp/aros-gnu-header-closure.XXXXXX)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM

cd "$source_root"
python3 tools/genmf/genmf.py config/make.tmpl tools/crosstools/gnu/mmakefile.src "$task_tmp/gnu-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl compiler/include/mmakefile.src "$task_tmp/compiler-include-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl compiler/crt/stdc/include/mmakefile.src "$task_tmp/stdc-include-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl compiler/crt/posixc/include/mmakefile.src "$task_tmp/posixc-include-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl tools/crosstools/gnu/tests/fixture/host-includes.mm.src "$task_tmp/host-includes-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl workbench/libs/muimaster/staticlib/mmakefile.src "$task_tmp/mui-static-mmakefile"
python3 tools/genmf/genmf.py config/make.tmpl workbench/libs/codesets/mmakefile.src "$task_tmp/codesets-mmakefile"

assert_line() {
	label=$1
	file=$2
	line=$3
	if ! grep -Fqx -- "$line" "$file"; then
		printf '%s\n' "FAIL: $label; missing GenMF line: $line" "--- $file ---" >&2
		exit 1
	fi
}

assert_absent_line() {
	label=$1
	file=$2
	line=$3
	if grep -Fqx -- "$line" "$file"; then
		printf '%s\n' "FAIL: $label; unexpected GenMF line: $line" "--- $file ---" >&2
		exit 1
	fi
}

assert_line 'GNU compiler build uses the release selector' "$task_tmp/gnu-mmakefile" \
	"#MM tools-crosstools-gcc : sdk-includes-\$(AROS_TOOLCHAIN_RELEASE)"
assert_line 'GNU compiler package uses the release selector' "$task_tmp/gnu-mmakefile" \
	"#MM crosstools-gcc : sdk-includes-\$(AROS_TOOLCHAIN_RELEASE)"
assert_absent_line 'GNU compiler build no longer names the global aggregate' "$task_tmp/gnu-mmakefile" \
	'#MM tools-crosstools-gcc : includes-copy'
assert_absent_line 'GNU compiler package no longer names the global aggregate' "$task_tmp/gnu-mmakefile" \
	'#MM crosstools-gcc : includes-copy'
assert_line 'GNU compiler host build uses the selected SDK closure' "$task_tmp/gnu-mmakefile" \
	"#MM- crosstools-gcc--host : setup sdk-includes-\$(AROS_TOOLCHAIN_RELEASE) crosstools-gcc--quick"
assert_absent_line 'GNU compiler host build no longer uses the global aggregate' "$task_tmp/gnu-mmakefile" \
	'#MM- crosstools-gcc--host : setup includes crosstools-gcc--quick'

assert_line 'direct generic host build retains its default aggregate' "$task_tmp/host-includes-mmakefile" \
	'#MM- fixture-direct-host : setup includes fixture-direct-quick'
assert_line 'generic fetch wrapper retains its default aggregate' "$task_tmp/host-includes-mmakefile" \
	'#MM- fixture-fetch--host : setup includes fixture-fetch--quick'
assert_line 'generic fetch wrapper forwards a host override' "$task_tmp/host-includes-mmakefile" \
	"#MM- fixture-fetch-override--host : setup sdk-includes-\$(AROS_TOOLCHAIN_RELEASE) fixture-fetch-override--quick"
assert_line 'target prerequisite remains unchanged despite host override' "$task_tmp/host-includes-mmakefile" \
	'#MM- fixture-target-override-target : setup includes core-linklibs fixture-target-override-quick'
assert_line 'direct target can declare its source-owned closure' "$task_tmp/host-includes-mmakefile" \
	'#MM- fixture-direct-target-closure-target : setup fixture-headers fixture-linklibs fixture-direct-target-closure-quick'
assert_line 'fetch wrapper forwards target closure unchanged' "$task_tmp/host-includes-mmakefile" \
	'#MM- fixture-fetch-target-closure--target : setup fixture-headers fixture-linklibs fixture-fetch-target-closure--quick'

assert_line 'GNU libatomic selects SDK headers and declared target libraries' "$task_tmp/gnu-mmakefile" \
	"#MM- tools-crosstools-gcc-libatomic-target : setup sdk-includes-\$(AROS_TOOLCHAIN_RELEASE) gnu-libatomic-linklibs-\$(AROS_TOOLCHAIN_RELEASE) tools-crosstools-gcc-libatomic-quick"
assert_line 'Classic libatomic retains the core libraries' "$task_tmp/gnu-mmakefile" \
	'#MM- gnu-libatomic-linklibs-0 : core-linklibs'
assert_line 'Classic libatomic retains its CPU library aggregate' "$task_tmp/gnu-mmakefile" \
	"#MM- gnu-libatomic-cpu-linklibs-0 : linklibs-\$(AROS_TARGET_CPU)"
assert_line 'Release libatomic retains the actual GNU default link libraries' "$task_tmp/gnu-mmakefile" \
	'#MM- gnu-libatomic-linklibs-1 : tools-crosstools-autolibs'
assert_absent_line 'GNU libatomic cannot request the raw global header aggregate' "$task_tmp/gnu-mmakefile" \
	'#MM- tools-crosstools-gcc-libatomic-target : setup includes core-linklibs tools-crosstools-gcc-libatomic-quick'

assert_line 'MUI static library uses the selected SDK closure' "$task_tmp/mui-static-mmakefile" \
	"#MM linklibs-mui : sdk-includes-\$(AROS_TOOLCHAIN_RELEASE) \\"
assert_line 'MUI static library retains its own required generated headers' "$task_tmp/mui-static-mmakefile" \
	'#MM    workbench-libs-muimaster-includes includes-libraries-mui'
assert_absent_line 'MUI static library cannot reintroduce the raw global header aggregate' "$task_tmp/mui-static-mmakefile" \
	'#MM linklibs-mui : includes'

assert_line 'Codesets linklib explicitly requires its archive-owned headers' "$task_tmp/codesets-mmakefile" \
	'#MM linklibs-codesets : workbench-libs-codesets-includes'
assert_line 'Codesets headers retain the exact archive fetch prerequisite' "$task_tmp/codesets-mmakefile" \
	"#MM workbench-libs-codesets-includes : \\"
assert_line 'Codesets header closure fetches its own source' "$task_tmp/codesets-mmakefile" \
	"#MM     workbench-libs-codesets-fetch \\"

# Aggregate membership is the reverse edge, not a prerequisite. Inspect only
# the selected producer's declaration, not its membership in ports-includes.
codesets_header_closure=$(awk '
	/^#MM workbench-libs-codesets-includes :/ { active = 1 }
	active {
		print
		if ($0 !~ /\\$/) exit
	}
' "$task_tmp/codesets-mmakefile")
test -n "$codesets_header_closure"
if printf '%s\n' "$codesets_header_closure" | grep -Eq '(^|[[:space:]])(includes|includes-copy|ports-includes)([[:space:]\\]|$)'; then
	printf '%s\n' 'FAIL: Codesets headers reintroduce a global header aggregate' >&2
	exit 1
fi

assert_line 'classic selector retains the upstream SDK closure' "$task_tmp/compiler-include-mmakefile" \
	'#MM- sdk-includes-0 : includes includes-copy'

release_closure=$(awk '
	/^#MM- sdk-includes-1 :/ { active = 1 }
	active {
		print
		if ($0 !~ /\\$/) exit
	}
' "$task_tmp/compiler-include-mmakefile")
for required in \
	'compiler-includes' \
	'compiler-stdc-includes-extra' \
	'linklibs-posixc-includes-files' \
	"includes-copy-\$(ARCH)-\$(CPU)" \
	"includes-copy-\$(FAMILY)-\$(CPU)" \
	"includes-copy-\$(CPU)" \
	'compiler-boost-sdk-subset' \
	'kernel-acpica-sdk-subset'; do
	case "$release_closure" in
		*"$required"*) ;;
		*) printf '%s\n' "FAIL: release header closure omits $required" '--- actual closure ---' "$release_closure" >&2; exit 1 ;;
	esac
done
if printf '%s\n' "$release_closure" | grep -Eq '(^|[[:space:]])includes-copy([[:space:]\\]|$)|(^|[[:space:]])includes([[:space:]\\]|$)'; then
	printf '%s\n' 'FAIL: release header closure names a global includes target' '--- actual closure ---' "$release_closure" >&2
	exit 1
fi

assert_line 'C header producer exists without compiling libc' "$task_tmp/stdc-include-mmakefile" \
	"compiler-stdc-includes-extra : \$(BD_INCL_FILES)"
assert_line 'Posix header producer exists without compiling libc' "$task_tmp/posixc-include-mmakefile" \
	"linklibs-posixc-includes-files : \$(BD_INCL_FILES)"

printf '%s\n' 'GNU release header-closure GenMF probes passed.'
