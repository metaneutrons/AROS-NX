# GNU release layout selection

Compiler profiles describe toolchain artifacts; source presets select
source and board build rules. They are separate namespaces and must not be
inferred from a compiler filename. The first GNU release profile is RV32 for
ESP32-P4. Existing LLVM source presets remain unchanged; no RV64 GNU consumer
preset is introduced by these release-recipe corrections.

The GCC 16.2 AROS patch does not invoke the removed
`GLIBCXX_CHECK_COMPLEX_MATH_SUPPORT` or `GLIBCXX_CHECK_WCHAR_T_SUPPORT` macros.
Neither has a definition in this GCC release; literal calls in generated
configure scripts only emit command-not-found errors. Current common
`GLIBCXX_CHECK_C99_TR1` and `GLIBCXX_ENABLE_WCHAR_T` checks already run for AROS
cross builds. The AROS-specific header checks and function declarations are
retained. Removing obsolete invocations is not a target runtime qualification.

Run `sh tools/crosstools/gnu/tests/test-libstdcxx-configure.sh
/path/to/gcc-16.2.0.tar.xz` against the pristine locked archive. The regression
applies the two affected patch sections without fuzz, executes the generated
AROS case with header probes stubbed, and rejects both obsolete shell tokens.

In GNU recipes, `AROS_GNU_RELEASE_LAYOUT` defaults to `yes` exactly when
`AROS_TOOLCHAIN_RELEASE=1`, and to `no` otherwise. The producer's release
selector therefore enables the relocatable layout automatically; an explicit
`AROS_GNU_RELEASE_LAYOUT=no` with selector `1` is rejected. Explicit `yes`
remains available to other GNU builds. The `no` path retains the historical
physical-prefix configure and install arguments, including the legacy
`GCC_EXTRA_OPTS` replacement behavior.

In release mode, Binutils, GCC, and GDB configure in the neutral namespace `/aros-toolchain`, with `--bindir=/aros-toolchain` and `--libdir=/aros-toolchain/lib`. Their installs override those variables with `/` and `/lib` and stage with `DESTDIR=$(CROSSTOOLSDIR)`, keeping the candidate payload flat. A root configure prefix alone produces GCC's `STANDARD_BINDIR_PREFIX=//`, which prevents executable-prefix relocation; the canonical namespace avoids that mismatch.

Binutils alone receives compile-only make argument `bindir=/aros-toolchain/`. Its libiberty script-directory relocation compares split directory components including trailing separators. Without that slash, a flat installed `ld` searches outside the candidate for `<target>/lib/ldscripts`; relocatable links used by the feature probe fail even though the scripts are present. Passing the slash only to configure is ineffective: Autoconf removes trailing directory separators. The generic `build_make_opts` template parameter applies to the compile/up-to-date commands, never install or uninstall. Binutils installation therefore retains `bindir=/`; GCC, GDB and classic builds retain their previous arguments. The actual-libiberty regression tests RV32 and RV64 script lookup under a relocated path containing spaces; it is a path-resolution proof, not a complete compiler build.

Binutils and GCC receive `--with-sysroot=yes`, which gives them a relocatable default at `${exec_prefix}/<target>/sys-root`. GCC additionally receives `--with-build-sysroot=$(AROS_DEVELOPER)` so target libraries can use generated headers during the build; that build-only path is not its default sysroot. The release install creates a marker-only `<target>/sys-root/README` because GCC adopts a relocated default sysroot only when that path exists. This is not an SDK: no target headers or runtime libraries are bundled there. Consumers must provide their target SDK explicitly.

The mode stages GMP, ISL, MPFR, and MPC under the host-generated GNU build-dependency directory, outside the candidate package. Host GNU packages no longer receive `CROSSTOOLSDIR`'s include/library paths. MPFR and MPC select their GMP/MPFR dependencies by that private prefix; ISL uses its supported `--with-gmp=system --with-gmp-prefix=...` options, and its `PKG_CONFIG_PATH`/`PKG_CONFIG_LIBDIR` are constrained to the private staging tree. GCC is pointed at the same build-only dependency directory. In release mode, custom `GCC_EXTRA_OPTS` extend the normal target and private-dependency options before enforced layout options are appended.

The GCC MetaMake targets and GCC host-build prerequisite select `sdk-includes-$(AROS_TOOLCHAIN_RELEASE)`. Classic selector `0` retains the full upstream SDK closure. Release selector `1` uses compiler and architecture headers, the existing standard C and POSIX header-copy producers, and the vendored Boost and ACPICA header subsets. GCC's target-runtime configuration needs the standard headers for its preprocessor sanity checks; these copy targets do not build libc. The selector avoids the global `includes-copy` aggregate, which reaches optional Ports such as Expat and would add unrelated archives to the locked release source set. The generic host-build prerequisite remains `includes` unless a caller supplies the optional `hostincludes` parameter. The focused header-closure test checks these macro expansions and the two header-copy producers in GenMF output.

Release mode also passes GCC's singular `--disable-plugin` option. The existing plural `--disable-plugins` controls a different generic option and does not disable GCC's compiler-plugin support or its development payload. Classic mode retains its previous options. The expansion probe checks the exact singular token, not a substring of the plural spelling.

When `AROS_TOOLCHAIN_REPRO_FLAGS` is set, its maps enter host `CFLAGS`/`CXXFLAGS` and target `CFLAGS_FOR_TARGET`/`CXXFLAGS_FOR_TARGET` separately; ambient target make variables are not copied into those host flags. Explicitly empty producer maps remain explicit empty environment values.

GMP alone receives `-std=gnu11` at the end of its host `CFLAGS` in release mode. GMP 6.3.0's compiler reliability probe contains an empty-parameter function definition and later calls that function with arguments; the host's C23 mode rejects that legacy GNU C construct. The local `CFLAGS` override lets the probe use GNU C11 semantics while preserving the producer remaps. It does not change the host compiler setting, C++ flags, other host-package flags, or GCC target ISA flags. Classic mode does not add the override.

Release mode also supplies `AROS_GCC_RELEASE_LAYOUT=yes` and the actual build/source roots to the GCC source patch, both during the top-level configure and as GCC make variables exported to its later subdirectory configure processes. The patch maps only the recorded configure-argument copy into `/aros-build` and `/aros-source`; the arguments used to configure or build GCC remain unchanged. Generated fixed-header comments use `/aros-sdk/include`, while fixincludes still reads the real build SDK. Installed `mkheaders.conf` selects SDK-relative headers instead of the producer's Developer tree. Header maintenance requires an explicit SDK and supports an explicit relocated compiler prefix; it does not remove the target SDK requirement.

The native GNU producer replaces both legacy C `collect-aros` destinations with the Rust collector before recording the collector receipt or publishing a completed candidate. That collector resolves sibling tools from its manifests and receives the SDK from the caller. The separate classic C-collector/SDK-install path is unchanged.

Release host packages use a separate `release-layout-v1/sdk-<selector>` object directory;
classic paths remain unchanged. GCC and GDB also use layout-versioned install
markers, and GCC additionally keys its marker by SDK selector. A classic marker
or selector-0 GCC state cannot skip a selector-1 release installation. Libatomic's
actual configure/install state additionally separates SDK selectors `0` and `1`.
The generated-recipe regression executes these guards against seeded classic
state; it is not a full compiler build. Do not alternate install layouts in one
candidate payload: qualification still requires a fresh complete native build.

This is source-generation and recipe work, not strict-prefix closure qualification. The native producer's `AROS_TOOLCHAIN_RELEASE=1` selector enables this mode automatically; the producer must still provide repro maps. `test-release-layout.sh` runs GenMF and GNU make over the actual generated recipes and host configure environment, but it does not build GNU packages or scan a candidate. `test-header-closure.sh` checks macro expansion only; it does not prove the complete native MetaMake closure or a successful GNU build. A fresh complete native build, strict package scan, package read-back and compatibility tests remain required after the metadata correction. A failed earlier build or a partial installed GCC is not a completed release candidate.

From the source root, run the header-closure and parameter probes with:

```sh
sh tools/crosstools/gnu/tests/test-header-closure.sh
sh tools/crosstools/gnu/tests/test-release-layout.sh
```

The recorded-metadata regression additionally requires an extracted GCC 16.2.0
source tree before the recorded-metadata patch is applied. Pass its path explicitly:

```sh
sh tools/crosstools/gnu/tests/test-recorded-metadata.sh "$GCC_SOURCE_DIR"
```

It applies the metadata patch without fuzz, executes the actual configure
argument-recording block and install rule, compiles an isolated fixed-header
preamble probe, and exercises SDK validation through a mocked header-maintenance
fixture. It is not a complete compiler build or an SDK compatibility test.

After a host Binutils build has produced its static libiberty library, run:

```sh
sh tools/crosstools/gnu/tests/test-script-relocation.sh "$BINUTILS_BUILD_DIR/libiberty/libiberty.a"
```

## Private host zstd

The separate GNU libatomic target uses `sdk-includes-$(AROS_TOOLCHAIN_RELEASE)`
and a named library selector. Classic selector `0` retains `core-linklibs` and
the CPU-wide aggregate. Release selector `1` retains
`tools-crosstools-autolibs`, needed by GCC's AROS default library specification
and libatomic's pthread configure probes, without the raw `includes` aggregate
or CPU-wide linklibs. The generic Autotools template exposes default-preserving
`targetincludes` and `targetlinklibs` parameters; unrelated callers still get
`includes core-linklibs`. MetaMake ignores Make conditionals when collecting
edges, so selection is by target name, not a conditional around `#MM` lines.
Generated-header tests verify these declarations. A fresh full native build
must still establish successful libatomic configure/link behavior and the
complete source/runtime closure; the earlier Expat failure is not success.

The MUI static library is an actual GCC default-link prerequisite. Its explicit
header edge also selects `sdk-includes-$(AROS_TOOLCHAIN_RELEASE)`, retaining
both its generated `proto/muimaster.h` producer and the local `libraries/mui.h`
producer. Classic selector `0` retains the global header graph. Release selector
`1` must not re-enter `includes` through this library: that aggregate reaches
unrelated Expat fetches even when libatomic's own prerequisites are bounded.
The header regression checks the generated MUI edge and both required headers.
This corrects a source dependency; it is not a completed native release proof.

Codesets remains a real GCC default-link library, unlike Expat. Its linklib
source includes `proto/codesets.h` from the Codesets archive, so the source rule
explicitly depends on `workbench-libs-codesets-includes`. That named producer
fetches the existing locked archive and copies its own headers without entering
the global Ports aggregate. Both classic and release builds retain this real
dependency instead of relying on another target to populate the source first.

Release Binutils requires zstd 1.5.7. Its exact archive checksum is declared in
the source recipe and must also be included in the producer's offline source
lock. A host PIC static library and headers are staged under
`gnu-builddeps/zstd`, outside the compiler payload; the BSD license is copied
to `share/licenses/zstd/LICENSE` inside the payload. No shared zstd library is
built. A contaminated private directory with a shared-library linker name is
rejected. Classic builds retain the existing optional host autodetection.

The configured `HOST_AR` is a complete command including its archive operation
(normally `cr`), not just an executable. The zstd build suppresses its own
`ARFLAGS` to avoid appending a second operation, then uses `HOST_RANLIB` to
index the resulting static archive before installation. This preserves the
configured host tools without splitting or replacing their command strings.

The normal Binutils wrapper builds this dependency before its configure/build
quick target. Both configure and compile make receive explicit `ZSTD_CFLAGS`
and `ZSTD_LIBS`: BFD configures later inside make. `-L<private>/lib -lzstd`
selects the static library; passing an absolute archive here causes Libtool to
nest an archive inside `libbfd.a` on macOS. The release install stamp includes
the zstd version so an older/classic installed Binutils cannot satisfy it.

The parameter tests verify actual generated make arguments. The dedicated
zstd recipe test checks generated command contracts, not a full compiler
release. Independent macOS RV64 Binutils diagnostics confirmed zstd debug
compression/decompression and system-only Mach-O dependencies. Fresh native
builds, all-host runtime closure, packaging, SBOM/license inventory and
compatibility remain release gates; a same-host link alone is insufficient.
