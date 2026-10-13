#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
source_root=$(CDPATH='' cd "$test_dir/../../../.." && pwd)
make_bin=${MAKE:-make}
task_tmp=$(mktemp -d /tmp/aros-gnu-zstd-closure.XXXXXX)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
build_dir="$task_tmp/build"
mkdir -p "$build_dir" "$task_tmp/bin" "$task_tmp/gen" "$task_tmp/ports"

cd "$source_root"
python3 tools/genmf/genmf.py config/make.tmpl tools/crosstools/gnu/mmakefile.src "$build_dir/mmakefile"
cd "$build_dir"

make_mock() {
	mock_path="$task_tmp/bin/$1"
	# The mock shell expands these variables when it runs, not during generation.
	# shellcheck disable=SC2016
	{
		printf '%s\n' '#!/bin/sh'
		printf '%s\n' 'printf "command=%s\\n" "${0##*/}" >> "$MOCK_LOG"'
		printf '%s\n' 'for argument do printf "arg=%s\\n" "$argument" >> "$MOCK_LOG"; done'
		printf '%s\n' 'printf "%s\\n" end-command >> "$MOCK_LOG"'
		printf '%s\n' 'if [ "${MOCK_REAL_MAKE_ZSTD:-}" = 1 ] && [ "${1-}" = -C ] && [ "${3-}" = libzstd.a ]; then'
		printf '%s\n' '  "${REAL_MAKE_BIN:-make}" --no-print-directory --silent "$@"'
		printf '%s\n' '  exit $?'
		printf '%s\n' 'fi'
	} > "$mock_path"
	chmod +x "$mock_path"
}

make_mock mock-make
make_mock mock-host-ranlib
make_mock mock-fetch
make_mock mock-mkdir
make_mock mock-cp

run_generated() {
	target=$1
	shift
	"$make_bin" --no-print-directory --silent -f ./mmakefile "$target" \
		TOP="$test_dir/fixture" \
		SRCDIR="$source_root" \
		GENDIR="$task_tmp/gen" \
		CURDIR=tools/crosstools/gnu \
		AROS_TOOLCHAIN=gnu \
		CROSSTOOLSDIR="$task_tmp/candidate" \
		AROS_DEVELOPER="$task_tmp/developer" \
		HOSTDIR="$task_tmp/host" \
		HOSTGENDIR="$task_tmp/host-gen" \
		PORTSSOURCEDIR="$task_tmp/ports" \
		AROS_TARGET_CPU=riscv64 \
		AROS_HOST_CPU=aarch64 \
		AROS_HOST_ARCH=darwin \
		TARGET_GCC_VER=16.2.0 \
		TARGET_BINUTILS_VER=2.47 \
		ZSTD_VERSION=1.5.7 \
		HOST_DEF_CC='mock-host-cc -target mock-host' \
		HOST_AR='mock-host-ar cr' HOST_RANLIB="$task_tmp/bin/mock-host-ranlib" \
		AROS_TOOLCHAIN_REPRO_FLAGS='-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped' \
		IF=if TEST=test TOUCH=touch \
		"$@"
}

assert_contains() {
	label=$1
	value=$2
	needle=$3
	case "$value" in
		*"$needle"*) ;;
		*) printf '%s\n' "FAIL: $label; missing: $needle" '--- actual ---' "$value" >&2; exit 1 ;;
	esac
}

assert_log_arg() {
	label=$1
	log=$2
	argument=$3
	if ! grep -Fqx -- "arg=$argument" "$log"; then
		printf '%s\n' "FAIL: $label; missing argument: $argument" '--- mock log ---' >&2
		cat "$log" >&2
		exit 1
	fi
}

classic_log="$task_tmp/classic.log"
run_generated tools-crosstools-zstd-no \
	AROS_GNU_RELEASE_LAYOUT=no FETCH="$task_tmp/bin/mock-fetch" \
	MAKE="$task_tmp/bin/mock-make" MOCK_LOG="$classic_log"
if [ -e "$classic_log" ]; then
	printf '%s\n' 'FAIL: classic zstd target invoked a mocked fetch or make command' >&2
	cat "$classic_log" >&2
	exit 1
fi

fetch_log="$task_tmp/fetch.log"
run_generated crosstools-zstd-fetch \
	AROS_GNU_RELEASE_LAYOUT=yes FETCH="$task_tmp/bin/mock-fetch" \
	ECHO=: MOCK_LOG="$fetch_log"
assert_log_arg 'release fetch pins the archive version' "$fetch_log" 'zstd-1.5.7'
assert_log_arg 'release fetch pins the archive suffix' "$fetch_log" 'tar.gz'
assert_log_arg 'release fetch pins the exact archive checksum' "$fetch_log" \
	'zstd-1.5.7.tar.gz=sha256:eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'

release_log="$task_tmp/release-build.log"
candidate="$task_tmp/release-candidate"
zstd_source="$task_tmp/host/Ports/host/zstd/zstd-1.5.7"
zstd_private_prefix="$task_tmp/host-gen/tools/crosstools/gnu/gnu-builddeps/zstd"
run_generated tools-crosstools-zstd-yes \
	AROS_GNU_RELEASE_LAYOUT=yes \
	CROSSTOOLSDIR="$candidate" \
	MAKE="$task_tmp/bin/mock-make" \
	MKDIR="$task_tmp/bin/mock-mkdir" CP="$task_tmp/bin/mock-cp" \
	MOCK_LOG="$release_log"

build_call=$(awk '
	/^command=mock-make$/ { block = $0; active = 1; found = 0; next }
	active { block = block "\n" $0; if ($0 == "arg=libzstd.a") found = 1 }
	active && $0 == "end-command" { if (found) { print block; exit }; active = 0 }
' "$release_log")
assert_contains 'release build invokes the host compiler' "$build_call" 'arg=CC=mock-host-cc -target mock-host'
assert_contains 'release build invokes the supplied host archiver' "$build_call" 'arg=AR=mock-host-ar'
assert_contains 'release build preserves the host archiver operation' "$build_call" 'arg=AR=mock-host-ar cr'
assert_contains 'release build suppresses zstd ARFLAGS' "$build_call" 'arg=ARFLAGS='
assert_contains 'release build uses reproducibility flags and PIC optimization' "$build_call" \
	'arg=CFLAGS=-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped -O2 -fPIC'

ranlib_call=$(awk '
	/^command=mock-host-ranlib$/ { block = $0; active = 1; next }
	active { block = block "\n" $0 }
	active && $0 == "end-command" { print block; exit }
' "$release_log")
expected_ranlib_call=$(printf '%s\n' \
	'command=mock-host-ranlib' \
	"arg=$zstd_source/lib/libzstd.a" \
	'end-command')
if [ "$ranlib_call" != "$expected_ranlib_call" ]; then
	printf '%s\n' 'FAIL: release must index the built zstd archive with the host ranlib' \
		'--- actual ranlib call ---' "$ranlib_call" >&2
	exit 1
fi

install_call=$(awk '
	/^command=mock-make$/ { block = $0; active = 1; found = 0; next }
	active { block = block "\n" $0; if ($0 == "arg=install-static") found = 1 }
	active && $0 == "end-command" { if (found) { print block; exit }; active = 0 }
' "$release_log")
assert_contains 'release installs the static library' "$install_call" 'arg=install-static'
assert_contains 'release installs the public headers' "$install_call" 'arg=install-includes'
assert_contains 'release installs beneath its private prefix' "$install_call" "arg=PREFIX=$zstd_private_prefix"
assert_contains 'release keeps its library directory private' "$install_call" "arg=LIBDIR=$zstd_private_prefix/lib"
assert_contains 'release keeps its include directory private' "$install_call" "arg=INCLUDEDIR=$zstd_private_prefix/include"
expected_install_call=$(printf '%s\n' \
	'command=mock-make' \
	'arg=-C' \
	"arg=$zstd_source/lib" \
	'arg=install-static' \
	'arg=install-includes' \
	"arg=PREFIX=$zstd_private_prefix" \
	"arg=LIBDIR=$zstd_private_prefix/lib" \
	"arg=INCLUDEDIR=$zstd_private_prefix/include" \
	'end-command')
if [ "$install_call" != "$expected_install_call" ]; then
	printf '%s\n' 'FAIL: release zstd install must request only the static archive and headers' \
		'--- actual install call ---' "$install_call" >&2
	exit 1
fi

build_line=$(grep -nF 'arg=libzstd.a' "$release_log" | cut -d: -f1)
ranlib_line=$(grep -nF 'command=mock-host-ranlib' "$release_log" | cut -d: -f1)
install_line=$(grep -nF 'arg=install-static' "$release_log" | cut -d: -f1)
if [ "$build_line" -ge "$ranlib_line" ] || [ "$ranlib_line" -ge "$install_line" ]; then
	printf '%s\n' 'FAIL: release must build, index, then install the zstd archive' \
		'--- mock make log ---' >&2
	cat "$release_log" >&2
	exit 1
fi

# Exercise the archive command with a small upstream-shaped Makefile. Its
# target-specific ARFLAGS assignment reproduces zstd's lib/Makefile behavior.
native_fixture_cc=${ZSTD_CLOSURE_CC:-${HOST_DEF_CC:-${CC:-cc}}}
native_fixture_ranlib=${ZSTD_CLOSURE_RANLIB:-${HOST_RANLIB:-${RANLIB:-ranlib}}}
if [ -n "${ZSTD_CLOSURE_HOST_AR:-}" ]; then
	native_fixture_ar=$ZSTD_CLOSURE_HOST_AR
elif [ -n "${HOST_AR:-}" ]; then
	native_fixture_ar=$HOST_AR
else
	native_fixture_ar=${ZSTD_CLOSURE_AR:-${AR:-ar}}
	native_fixture_ar="$native_fixture_ar cr"
fi

write_native_fixture() {
	native_fixture_lib=$1
	mkdir -p "$native_fixture_lib"
	printf '%s\n' \
		'int zstd_fixture_value(void) { return 42; }' \
		> "$native_fixture_lib/zstd.c"
	printf '%s\n' \
		'extern int zstd_fixture_value(void);' \
		'int main(void) { return zstd_fixture_value() == 42 ? 0 : 1; }' \
		> "$native_fixture_lib/consumer.c"
	# The fixture Makefile must retain GNU make's variable references literally.
	# shellcheck disable=SC2016
	{
		printf '%s\n' \
			'libzstd.a: ARFLAGS = rcs' \
			'libzstd.a: zstd.o'
		printf '\t%s\n' '$(AR) $(ARFLAGS) $@ $^'
		printf '%s\n' 'zstd.o: zstd.c'
		printf '\t%s\n' '$(CC) $(CFLAGS) -c -o $@ $<'
	} > "$native_fixture_lib/Makefile"
}

native_fixture_dir="$task_tmp/native-zstd-fixture"
duplicate_lib="$native_fixture_dir/duplicate/lib"
write_native_fixture "$duplicate_lib"
if "$make_bin" --no-print-directory --silent -C "$duplicate_lib" libzstd.a \
		CC="$native_fixture_cc" AR="$native_fixture_ar" \
		> "$task_tmp/duplicate-archive.stdout" 2> "$task_tmp/duplicate-archive.stderr"; then
	printf '%s\n' 'FAIL: native archiver accepted HOST_AR cr together with zstd ARFLAGS rcs' >&2
	exit 1
fi
if [ ! -s "$duplicate_lib/zstd.o" ] || [ -e "$duplicate_lib/libzstd.a" ]; then
	printf '%s\n' 'FAIL: duplicate-operation fixture did not reach and fail at archive creation' >&2
	cat "$task_tmp/duplicate-archive.stderr" >&2
	exit 1
fi

# Run the generated release recipe against the native fixture. This proves its
# empty ARFLAGS override and explicit HOST_RANLIB produce a linkable archive.
write_native_fixture "$zstd_source/lib"
native_release_log="$task_tmp/native-release-build.log"
native_candidate="$task_tmp/native-release-candidate"
MOCK_REAL_MAKE_ZSTD=1 REAL_MAKE_BIN="$make_bin" run_generated tools-crosstools-zstd-yes \
	AROS_GNU_RELEASE_LAYOUT=yes CROSSTOOLSDIR="$native_candidate" \
	MAKE="$task_tmp/bin/mock-make" \
	MKDIR="$task_tmp/bin/mock-mkdir" CP="$task_tmp/bin/mock-cp" \
	HOST_DEF_CC="$native_fixture_cc" HOST_AR="$native_fixture_ar" \
	HOST_RANLIB="$native_fixture_ranlib" MOCK_LOG="$native_release_log"
# shellcheck disable=SC2086
$native_fixture_cc "$zstd_source/lib/consumer.c" "$zstd_source/lib/libzstd.a" \
	-o "$native_fixture_dir/consumer"
"$native_fixture_dir/consumer"

assert_log_arg 'release creates the candidate license directory' "$release_log" \
	"$candidate/share/licenses/zstd"
assert_log_arg 'release copies the upstream license to the candidate' "$release_log" \
	"$zstd_source/LICENSE"
assert_log_arg 'release uses the candidate license destination' "$release_log" \
	"$candidate/share/licenses/zstd/LICENSE"

# A private path containing a shared linker name must fail instead of silently
# selecting it when Binutils receives -lzstd. No candidate license is installed.
mkdir -p "$zstd_private_prefix/lib"
: > "$zstd_private_prefix/lib/libzstd.dylib"
contaminated_log="$task_tmp/contaminated.log"
if run_generated tools-crosstools-zstd-yes \
	AROS_GNU_RELEASE_LAYOUT=yes CROSSTOOLSDIR="$candidate" \
	MAKE="$task_tmp/bin/mock-make" MKDIR="$task_tmp/bin/mock-mkdir" \
	CP="$task_tmp/bin/mock-cp" MOCK_LOG="$contaminated_log" \
	> "$task_tmp/contaminated.stdout" 2> "$task_tmp/contaminated.stderr"; then
	printf '%s\n' 'FAIL: a private shared zstd linker name was accepted' >&2
	exit 1
fi
grep -Fq 'Release zstd directory contains a shared library' "$task_tmp/contaminated.stderr"
if grep -Fq 'command=mock-cp' "$contaminated_log"; then
	printf '%s\n' 'FAIL: contaminated zstd installation reached the candidate license step' >&2
	exit 1
fi
rm "$zstd_private_prefix/lib/libzstd.dylib"

# This invokes the generated Binutils recipe with a mock recursive make. It
# verifies the recipe's dispatch order, but does not run Binutils configure.
order_log="$task_tmp/binutils-order.log"
order_candidate="$task_tmp/order-candidate"
mkdir -p "$order_candidate"
run_generated tools-crosstools-binutils \
	AROS_GNU_RELEASE_LAYOUT=yes \
	CROSSTOOLSDIR="$order_candidate" \
	MAKE="$task_tmp/bin/mock-make" RM=: \
	MOCK_LOG="$order_log"
zstd_line=$(grep -nF 'arg=tools-crosstools-zstd-yes' "$order_log" | cut -d: -f1)
fetch_line=$(grep -nF 'arg=crosstools-binutils--fetch' "$order_log" | cut -d: -f1)
configure_line=$(grep -nF 'arg=crosstools-binutils--build_and_install-quick' "$order_log" | cut -d: -f1)
if [ "$zstd_line" -ge "$fetch_line" ] || [ "$fetch_line" -ge "$configure_line" ]; then
	printf '%s\n' 'FAIL: generated Binutils recipe does not build zstd before fetching and quick-configuring Binutils' \
		'--- mock make log ---' >&2
	cat "$order_log" >&2
	exit 1
fi

printf '%s\n' 'GNU zstd GenMF probes passed; generated recipe built and linked a native fixture archive.'
