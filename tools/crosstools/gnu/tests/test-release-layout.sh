#!/bin/sh
set -eu

test_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
source_root=$(CDPATH='' cd "$test_dir/../../../.." && pwd)
unset AROS_GCC_RELEASE_LAYOUT AROS_GCC_RELEASE_BUILD_ROOT AROS_GCC_RELEASE_SOURCE_ROOT
make_bin=${MAKE:-make}
task_tmp=$(mktemp -d /tmp/aros-gnu-release-layout.XXXXXX)
trap 'rm -rf "$task_tmp"' EXIT HUP INT TERM
mkdir -p "$task_tmp/gen"

cd "$source_root"
python3 tools/genmf/genmf.py config/make.tmpl tools/crosstools/gnu/mmakefile.src "$task_tmp/mmakefile"

run_generated() {
	target=$1
	shift
	"$make_bin" --no-print-directory --silent \
		-f "$task_tmp/mmakefile" \
		-f "$test_dir/generated-mmakefile-probe.mk" \
		"$target" \
		TOP="$test_dir/fixture" \
		SRCDIR="$source_root" \
		GENDIR="$task_tmp/gen" \
		CURDIR=tools/crosstools/gnu \
		AROS_TOOLCHAIN=gnu \
		CROSSTOOLSDIR=/tmp/gnu-candidate \
		AROS_DEVELOPER=/tmp/aros-developer \
		HOSTDIR=/tmp/aros-host \
		HOSTGENDIR=/tmp/aros-host-gen \
		AROS_TARGET_CPU=riscv64 \
		AROS_HOST_CPU=aarch64 \
		AROS_HOST_ARCH=darwin \
		TARGET_GCC_VER=16.2.0 \
		TARGET_BINUTILS_VER=2.47 \
		AROS_TOOLCHAIN_REPRO_FLAGS='-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped' \
		GCC_TARGET_CFLAGS=-mcmodel=medany \
		GCC_TARGET_CXXFLAGS=-mcmodel=medany \
		CFLAGS_FOR_TARGET=-Dambient-target-only "$@"
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

assert_not_contains() {
	label=$1
	value=$2
	needle=$3
	case "$value" in
		*"$needle"*) printf '%s\n' "FAIL: $label; unexpected: $needle" '--- actual ---' "$value" >&2; exit 1 ;;
		*) ;;
	esac
}

classic=$(run_generated generated-params)
mkdir -p "$task_tmp/child-make"
classic_child=$(run_generated generated-child-env "crosstools-gcc--pkgdir=$task_tmp/child-make")
assert_contains 'classic child make has no metadata mode' "$classic_child" 'metadata-layout='
assert_not_contains 'classic child make does not enable metadata mode' "$classic_child" 'metadata-layout=yes'
classic_gmp_env=$(printf '%s\n' "$classic" | sed -n 's/^gmp-configure-env=//p')
assert_contains 'classic mode' "$classic" 'mode=no'
assert_not_contains 'classic does not opt into recorded metadata mapping' "$classic" 'AROS_GCC_RELEASE_LAYOUT=yes'
assert_contains 'classic physical configure prefix' "$classic" 'gcc-prefix=/tmp/gnu-candidate'
assert_contains 'classic Binutils keeps physical slash-free bindir' "$classic" 'binutils-configure-args=--prefix=/tmp/gnu-candidate --target=riscv64-aros --bindir=/tmp/gnu-candidate --libdir=/tmp/gnu-candidate/lib --with-sysroot=/tmp/aros-developer'
assert_contains 'classic Binutils has no compile override' "$classic" 'binutils-build-args='
assert_not_contains 'classic Binutils cannot receive release compile bindir' "$classic" 'binutils-build-args=bindir='
assert_not_contains 'classic Binutils preserves optional host zstd detection' "$classic" '--with-zstd'
assert_not_contains 'classic Binutils has no explicit private zstd selection' "$classic" 'ZSTD_LIBS='
assert_contains 'classic Binutils stamp is unchanged' "$classic" 'binutils-install-stamp=/tmp/gnu-candidate/.installflag-binutils-2.47-riscv64'
assert_not_contains 'classic Binutils stamp has no release suffix' "$classic" '-release-zstd-'
classic_binutils_child=$(run_generated generated-binutils-build-args "crosstools-binutils--pkgdir=$task_tmp/child-make")
assert_contains 'classic Binutils child make has no compile bindir override' "$classic_binutils_child" 'build-bindir='
assert_not_contains 'classic Binutils child make cannot receive release compile bindir' "$classic_binutils_child" 'build-bindir=/aros-toolchain/'
assert_contains 'classic physical sysroot' "$classic" '--with-sysroot=/tmp/aros-developer'
assert_contains 'classic physical package dependency prefix' "$classic" '--with-isl=/tmp/gnu-candidate'
assert_contains 'classic install arguments preserve physical prefix' "$classic" 'gcc-install-args=prefix=/tmp/gnu-candidate exec_prefix=/tmp/gnu-candidate'
assert_contains 'classic dependency include paths remain unchanged' "$classic" '-I/tmp/gnu-candidate/include'
assert_contains 'classic dependency library paths remain unchanged' "$classic" '-L/tmp/gnu-candidate/lib'
assert_not_contains 'classic ignores release-only host maps' "$classic" 'CFLAGS="-ffile-prefix-map=/src=/mapped'
assert_contains 'classic GMP keeps the configured host C standard' "$classic_gmp_env" 'CC="/usr/bin/cc -std=gnu23 -I/tmp/gnu-candidate/include"'
assert_not_contains 'classic GMP has no release-only CFLAGS override' "$classic_gmp_env" ' CFLAGS="'
classic_args=$(printf '%s\n' "$classic" | sed -n 's/^gcc-configure-args=//p')
if printf '%s\n' "$classic_args" | grep -Eq '(^|[[:space:]])--disable-plugin([[:space:]]|$)'; then
	printf '%s\n' 'FAIL: classic mode unexpectedly changes the GCC plugin switch' >&2
	exit 1
fi

classic_install=$(run_generated generated-install-args)
assert_contains 'classic template-expanded install prefix' "$classic_install" 'install-prefix=/tmp/gnu-candidate'
assert_contains 'classic template-expanded install destination' "$classic_install" 'install-destdir='

# The producer entry point passes the validated configure release selector,
# not a second layout knob. Check the actual generated recipe with that same
# input so a manually forced layout test cannot conceal an unwired default.
automatic_release=$(run_generated generated-params AROS_TOOLCHAIN_RELEASE=1)
assert_contains 'producer release selects relocatable layout automatically' "$automatic_release" 'mode=yes'
assert_contains 'producer release selects neutral prefix automatically' "$automatic_release" 'gcc-prefix=/aros-toolchain'
assert_contains 'producer release separates host dependencies automatically' "$automatic_release" 'host-packages-use-crosstoolsdir=no'
assert_contains 'producer release stages installation automatically' "$automatic_release" 'DESTDIR=/tmp/gnu-candidate'
automatic_release_child=$(run_generated generated-child-env AROS_TOOLCHAIN_RELEASE=1 "crosstools-gcc--pkgdir=$task_tmp/child-make")
assert_contains 'automatic release metadata mode survives nested make' "$automatic_release_child" 'metadata-layout=yes'
automatic_rv32=$(run_generated generated-params AROS_TOOLCHAIN_RELEASE=1 AROS_TARGET_CPU=riscv \
	GCC_TARGET_CFLAGS='-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany' \
	GCC_TARGET_CXXFLAGS='-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany')
assert_contains 'RV32 release automatically uses neutral prefix' "$automatic_rv32" 'gcc-prefix=/aros-toolchain'
assert_contains 'RV32 release keeps the source-owned target triple' "$automatic_rv32" '--target=riscv-aros'
assert_contains 'RV32 release retains its explicit ISA and ABI' "$automatic_rv32" '-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32f -mcmodel=medany'
assert_contains 'RV32 release uses a build-only SDK' "$automatic_rv32" '--with-build-sysroot=/tmp/aros-developer'
assert_not_contains 'RV32 release never embeds the SDK as its default sysroot' "$automatic_rv32" '--with-sysroot=/tmp/aros-developer'
explicit_classic=$(run_generated generated-params AROS_TOOLCHAIN_RELEASE=0)
assert_contains 'explicit ordinary selector retains classic layout' "$explicit_classic" 'mode=no'
if run_generated generated-params AROS_TOOLCHAIN_RELEASE=1 AROS_GNU_RELEASE_LAYOUT=no >/dev/null 2>&1; then
	printf '%s\n' 'FAIL: producer release accepted the nonrelocatable classic layout' >&2
	exit 1
fi

classic_custom=$(run_generated generated-params GCC_EXTRA_OPTS=--enable-user-custom)
classic_custom_args=$(printf '%s\n' "$classic_custom" | sed -n 's/^gcc-configure-args=//p')
assert_contains 'classic custom GCC options preserved' "$classic_custom" '--enable-user-custom'
assert_not_contains 'classic custom GCC options preserve replacement semantics' "$classic_custom_args" '--with-sysroot=/tmp/aros-developer'

release=$(run_generated generated-params AROS_GNU_RELEASE_LAYOUT=yes)
release_config=$(printf '%s\n' "$release" | sed -n 's/^gcc-configure-args=//p')
release_env=$(printf '%s\n' "$release" | sed -n 's/^gcc-configure-env=//p')
release_gmp_env=$(printf '%s\n' "$release" | sed -n 's/^gmp-configure-env=//p')
release_child=$(run_generated generated-child-env AROS_GNU_RELEASE_LAYOUT=yes "crosstools-gcc--pkgdir=$task_tmp/child-make")
assert_contains 'release metadata mode survives the make boundary' "$release_child" 'metadata-layout=yes'
assert_contains 'release build root survives the make boundary' "$release_child" "metadata-build-root=$test_dir/fixture"
assert_contains 'release source root survives the make boundary' "$release_child" "metadata-source-root=$source_root"
assert_contains 'release recorded metadata opt-in reaches configure' "$release_env" 'AROS_GCC_RELEASE_LAYOUT=yes'
assert_contains 'release recorded metadata gets the actual build root' "$release_env" "AROS_GCC_RELEASE_BUILD_ROOT=\"$test_dir/fixture\""
assert_contains 'release recorded metadata gets the actual source root' "$release_env" "AROS_GCC_RELEASE_SOURCE_ROOT=\"$source_root\""
assert_contains 'release canonical neutral compiler configure prefix' "$release" 'gcc-prefix=/aros-toolchain'
assert_contains 'release canonical compiler configure namespace' "$release_config" '--prefix=/aros-toolchain --bindir=/aros-toolchain --libdir=/aros-toolchain/lib'
assert_contains 'release relocatable compiler default sysroot' "$release_config" '--with-sysroot=yes'
if ! printf '%s\n' "$release_config" | grep -Eq '(^|[[:space:]])--disable-plugin([[:space:]]|$)'; then
	printf '%s\n' 'FAIL: release mode does not disable GCC compiler plugins explicitly' >&2
	exit 1
fi
assert_contains 'release build-only Developer sysroot' "$release_config" '--with-build-sysroot=/tmp/aros-developer'
assert_not_contains 'Developer path not compiled as default sysroot' "$release_config" '--with-sysroot=/tmp/aros-developer'
assert_contains 'release private GMP prefix' "$release_config" '--with-gmp=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release private MPFR prefix' "$release_config" '--with-mpfr=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release private MPC prefix' "$release_config" '--with-mpc=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release private ISL prefix' "$release_config" '--with-isl=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release producer host remap survives full template environment' "$release_env" 'CFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped"'
assert_contains 'release producer host CXX remap survives full template environment' "$release_env" 'CXXFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped"'
assert_contains 'release target repro flags and ISA remain separate' "$release_env" 'CFLAGS_FOR_TARGET="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped -mcmodel=medany"'
assert_contains 'release target CXX repro flags and ISA remain separate' "$release_env" 'CXXFLAGS_FOR_TARGET="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped -mcmodel=medany"'
assert_not_contains 'ambient target CFLAGS do not enter host or target repro flags' "$release_env" 'ambient-target-only'
assert_not_contains 'release GCC configure environment omits candidate include path' "$release_env" '-I/tmp/gnu-candidate/include'
assert_not_contains 'release GCC configure environment omits candidate library path' "$release_env" '-L/tmp/gnu-candidate/lib'
assert_contains 'release GMP retains the host C23 compiler setting' "$release_gmp_env" 'CC="/usr/bin/cc -std=gnu23"'
assert_contains 'release GMP CFLAGS end with the local GNU C11 override' "$release_gmp_env" 'CFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped -std=gnu11"'
assert_contains 'release GMP keeps CXXFLAGS to producer remaps' "$release_gmp_env" 'CXXFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped"'
assert_not_contains 'release GMP standard override does not reach CXXFLAGS' "$release_gmp_env" 'CXXFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped -std=gnu11"'
assert_not_contains 'release GCC does not receive the GMP C standard override' "$release_env" '-std=gnu11'
release_isl_env=$(printf '%s\n' "$release" | sed -n 's/^isl-configure-env=//p')
assert_not_contains 'release ISL does not receive the GMP C standard override' "$release_isl_env" '-std=gnu11'
assert_contains 'release Binutils configure keeps canonical slash-free bindir' "$release" 'binutils-configure-args=--prefix=/aros-toolchain --target=riscv64-aros --bindir=/aros-toolchain --libdir=/aros-toolchain/lib --with-sysroot=yes'
assert_contains 'release Binutils preserves script relocation at compile time' "$release" 'binutils-build-args=bindir=/aros-toolchain/'
release_binutils_child=$(run_generated generated-binutils-build-args AROS_GNU_RELEASE_LAYOUT=yes "crosstools-binutils--pkgdir=$task_tmp/child-make")
assert_contains 'release Binutils child make receives directory-form bindir' "$release_binutils_child" 'build-bindir=/aros-toolchain/'
assert_contains 'release Binutils explicitly requires zstd support' "$release" '--with-zstd'
assert_contains 'release Binutils stamp invalidates old host dependency selection' "$release" 'binutils-install-stamp=/tmp/gnu-candidate/.installflag-binutils-2.47-riscv64-release-zstd-1.5.7'
assert_contains 'release Binutils configure uses private zstd headers' "$release" 'ZSTD_CFLAGS="-I/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps/zstd/include"'
assert_contains 'release Binutils configure uses private library flags, not a nested archive' "$release" 'ZSTD_LIBS="-L/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps/zstd/lib -lzstd"'
assert_contains 'private zstd headers survive nested configure make boundary' "$release_binutils_child" 'build-zstd-cflags=-I/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps/zstd/include'
assert_contains 'private static zstd selection survives nested configure make boundary' "$release_binutils_child" 'build-zstd-libs=-L/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps/zstd/lib -lzstd'
assert_contains 'release Binutils compile has no candidate install destination' "$release_binutils_child" 'build-destdir='
release_binutils_install=$(run_generated generated-binutils-install-args AROS_GNU_RELEASE_LAYOUT=yes)
assert_contains 'release Binutils installation remains flat' "$release_binutils_install" 'install-bindir=/'
assert_not_contains 'release Binutils installation cannot inherit compile bindir' "$release_binutils_install" 'install-bindir=/aros-toolchain/'
assert_contains 'release Binutils installation retains candidate staging' "$release_binutils_install" 'install-destdir=/tmp/gnu-candidate'
assert_contains 'release Binutils configure environment omits candidate paths' "$release" 'binutils-configure-env=CFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped" CXXFLAGS="-ffile-prefix-map=/src=/mapped -fdebug-prefix-map=/build=/mapped"'
assert_contains 'release GDB uses canonical neutral configure namespace' "$release" 'gdb-configure-args=--prefix=/aros-toolchain --target=riscv64-aros --bindir=/aros-toolchain --libdir=/aros-toolchain/lib'
assert_contains 'release Binutils install has neutral prefix and candidate DESTDIR' "$release" 'binutils-install-args=prefix=/tmp/gnu-candidate exec_prefix=/tmp/gnu-candidate prefix=/ exec_prefix=/ bindir=/ libdir=/lib DESTDIR=/tmp/gnu-candidate'
assert_contains 'release GDB install has neutral prefix and candidate DESTDIR' "$release" 'gdb-install-args=prefix=/tmp/gnu-candidate exec_prefix=/tmp/gnu-candidate prefix=/ exec_prefix=/ bindir=/ libdir=/lib DESTDIR=/tmp/gnu-candidate'
assert_contains 'release host GNU packages disable candidate include injection' "$release" 'host-packages-use-crosstoolsdir=no'
assert_not_contains 'release dependency environment omits candidate include path' "$release" '-I/tmp/gnu-candidate/include'
assert_not_contains 'release dependency environment omits candidate library path' "$release" '-L/tmp/gnu-candidate/lib'
assert_contains 'release GMP configure has no candidate prefix' "$release" 'gmp-configure-args=--prefix=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release ISL uses supported GMP prefix interface' "$release" '--with-gmp=system --with-gmp-prefix=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release MPFR selects private GMP' "$release" '--with-gmp=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release MPC selects private GMP and MPFR' "$release" '--with-gmp=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps --with-mpfr=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release PKG_CONFIG lookup is isolated to build dependencies' "$release" 'PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps/lib/pkgconfig'
assert_contains 'release GCC install receives DESTDIR candidate' "$release" 'gcc-install-args=prefix=/tmp/gnu-candidate exec_prefix=/tmp/gnu-candidate prefix=/ exec_prefix=/ bindir=/ libdir=/lib DESTDIR=/tmp/gnu-candidate'

release_install=$(run_generated generated-install-args AROS_GNU_RELEASE_LAYOUT=yes)
assert_contains 'release actual make install prefix is neutral' "$release_install" 'install-prefix=/'
assert_contains 'release actual make install bindir is neutral' "$release_install" 'install-bindir=/'
assert_contains 'release actual make install libdir is neutral' "$release_install" 'install-libdir=/lib'
assert_contains 'release actual make install destination is candidate' "$release_install" 'install-destdir=/tmp/gnu-candidate'

release_custom=$(run_generated generated-params AROS_GNU_RELEASE_LAYOUT=yes GCC_EXTRA_OPTS=--enable-user-custom)
release_custom_args=$(printf '%s\n' "$release_custom" | sed -n 's/^gcc-configure-args=//p')
assert_contains 'release custom options remain additive' "$release_custom_args" '--enable-user-custom'
assert_contains 'release custom options retain target triple/defaults' "$release_custom_args" '--target=riscv64-aros'
assert_contains 'release custom options retain private GMP' "$release_custom_args" '--with-gmp=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release custom options retain private ISL' "$release_custom_args" '--with-isl=/tmp/aros-host-gen/tools/crosstools/gnu/gnu-builddeps'
assert_contains 'release custom options retain build-only sysroot' "$release_custom_args" '--with-build-sysroot=/tmp/aros-developer'

release_empty_flags=$(run_generated generated-params AROS_GNU_RELEASE_LAYOUT=yes AROS_TOOLCHAIN_REPRO_FLAGS= GCC_TARGET_CFLAGS= GCC_TARGET_CXXFLAGS=)
release_empty_env=$(printf '%s\n' "$release_empty_flags" | sed -n 's/^gcc-configure-env=//p')
release_empty_gmp_env=$(printf '%s\n' "$release_empty_flags" | sed -n 's/^gmp-configure-env=//p')
assert_contains 'empty producer remap still emits explicit host CFLAGS' "$release_empty_env" 'CFLAGS=""'
assert_contains 'empty producer remap still emits explicit target CFLAGS' "$release_empty_env" 'CFLAGS_FOR_TARGET=""'
assert_not_contains 'ambient target flags are excluded from empty target repro flags' "$release_empty_env" 'CFLAGS_FOR_TARGET="-Dambient-target-only'
assert_contains 'empty GMP producer remap retains the GNU C11 override' "$release_empty_gmp_env" 'CFLAGS="-std=gnu11"'
assert_contains 'empty GMP producer remap remains explicit in CXXFLAGS' "$release_empty_gmp_env" 'CXXFLAGS=""'

for forbidden_sysroot in '--with-sysroot=/forbidden' '--with-build-sysroot=/forbidden'; do
	if run_generated generated-params AROS_GNU_RELEASE_LAYOUT=yes "GCC_EXTRA_OPTS=$forbidden_sysroot" >/dev/null 2>&1; then
		printf '%s\n' "FAIL: release mode accepted GCC_EXTRA_OPTS=$forbidden_sysroot" >&2
		exit 1
	fi
done

for invalid_layout in '' 'yes no'; do
	if run_generated generated-params "AROS_GNU_RELEASE_LAYOUT=$invalid_layout" >/dev/null 2>&1; then
		printf '%s\n' "FAIL: release mode accepted invalid value '$invalid_layout'" >&2
		exit 1
	fi
done

# Execute only the actual installed-GCC postlude, not a compiler build. The
# existing install flag skips the build/install commands; no helpers are copied.
for layout in no yes; do
	candidate="$task_tmp/candidate-$layout"
	mkdir -p "$candidate"
	touch "$candidate/.installflag-gcc-16.2.0-riscv64"
	run_generated tools-crosstools-gcc \
		AROS_GNU_RELEASE_LAYOUT="$layout" CROSSTOOLSDIR="$candidate" \
		GCC_VERSION=16.2.0 GCC_PATH=/unused TOOLDIR="$task_tmp/unused" \
		IF=if TEST=test SED=: MKDIR='mkdir -p' CP=cp
	if [ "$layout" = yes ]; then
		cmp "$source_root/tools/crosstools/gnu/README.sys-root" "$candidate/riscv64-aros/sys-root/README"
	else
		if [ -e "$candidate/riscv64-aros/sys-root" ]; then
			printf '%s\n' 'FAIL: classic mode creates a release-only sysroot anchor' >&2
			exit 1
		fi
	fi
done

# The custom all-gcc command must export the same variables as generic builds.
# Use the actual generated recipe with a harmless child makefile, not a copied
# command assembled by this test.
child_gcc="$task_tmp/actual-child/tools/crosstools/gnu/gcc"
mkdir -p "$child_gcc"
# The dollar signs below must survive into the child make recipe.
# shellcheck disable=SC2016
{
	printf '%s\n' '.PHONY: all-gcc install-gcc' 'all-gcc:'
	printf '\t@printf "actual-metadata-layout=%%s\\n" "$$AROS_GCC_RELEASE_LAYOUT"\n'
	printf '\t@printf "actual-metadata-build-root=%%s\\n" "$$AROS_GCC_RELEASE_BUILD_ROOT"\n'
	printf '\t@printf "actual-metadata-source-root=%%s\\n" "$$AROS_GCC_RELEASE_SOURCE_ROOT"\n'
	printf '%s\n' 'install-gcc:'
	printf '\t@:\n'
} > "$child_gcc/Makefile"
for layout in no yes; do
	candidate="$task_tmp/actual-candidate-$layout"
	mkdir -p "$candidate"
	actual_child=$(run_generated tools-crosstools-gcc \
		AROS_GNU_RELEASE_LAYOUT="$layout" CROSSTOOLSDIR="$candidate" \
		HOSTGENDIR="$task_tmp/actual-child" GCC_VERSION=16.2.0 GCC_PATH=/unused \
		TOOLDIR="$task_tmp/unused" IF=if TEST=test SED=: TOUCH=touch \
		MKDIR='mkdir -p' CP=cp)
	if [ "$layout" = yes ]; then
		assert_contains 'actual all-gcc exports metadata mode' "$actual_child" 'actual-metadata-layout=yes'
		assert_contains 'actual all-gcc exports build root' "$actual_child" "actual-metadata-build-root=$test_dir/fixture"
		assert_contains 'actual all-gcc exports source root' "$actual_child" "actual-metadata-source-root=$source_root"
	else
		assert_not_contains 'classic actual all-gcc does not enable metadata mapping' "$actual_child" 'actual-metadata-layout=yes'
	fi
done

printf '%s\n' 'GNU release-layout GenMF and make expansion probes passed.'
