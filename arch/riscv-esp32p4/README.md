# AROS on the Espressif ESP32-P4 (riscv-esp32p4)

Native AROS platform port for the ESP32-P4 SoC. The target name follows the
AROS convention `$(AROS_TARGET_CPU)-$(AROS_TARGET_ARCH)`, so the platform is
configured with

    ./configure --target=esp32p4-riscv

and shares the CPU layer in `arch/riscv-all` with any other 32-bit RISC-V
platform. Note the two orders: an AROS target string is `<arch>-<cpu>`, built
in `configure.in` as `$target_os-$target_cpu`, while the directory it selects
is `<cpu>-<arch>`. Hence `--target=esp32p4-riscv` for `arch/riscv-esp32p4`,
the same way `--target=opensbi-riscv64` selects `arch/riscv64-opensbi`.

The SoC target now has a compile-time board selector. Use `P4_BOARD=d1001`
for the only implemented profile; it is also the current default. The profile
under [board/](board/) owns D1001 GPIOs, the port-expander signals, flash
geometry, partition input and touch-firmware paths. An unknown name fails the
build rather than silently producing a D1001 image. Keep a separate configured
build directory for each board: mmake does not invalidate existing objects
when only a make variable changes. This is a board-support foundation, not a
claim that another ESP32-P4 board boots yet. D1001-specific panel, touch,
PSRAM and SD-power sequences still need driver selection before a second board
can be added safely; see the board-portability track in the roadmap.

Current C1P caveat (2026-09-30): Fabian confirms noticeably faster initial
drawing, then reports sudden severe slowdown and a hang. The runtime console
now polls once per output byte and drops saturated debug output before Exec
starts; USB/UART extracted-source sanitizer tests pass. This removes a
source-proven blocking path, not a confirmed explanation of the hang.
First-empty tap completion improves drive-icon double-taps according to Fabian;
the 50-ms post-read idle gap has been restored after renewed graphics slowdown.
The THEME/PNG image fix (64 MiB, 167 entries) is now written to SD and completely
readback-verified before exclusive eject. Original write-range content is backed
up. The visible gate exposes missing png.library>=52: the datatype's dynamic
library dependencies were omitted. The corrected 171-entry image is now SD
written/readback-verified/ejected (2026-10-01). Following the synchronized boot,
Fabian confirms RAM Disk opening without an error message and one visible
toolbar symbol. The missing-library/requester regression passes this opening;
Fabian also reports screen drawing is fine so far on the same running device.
This is short-run feedback with no measured duration; complete toolbar behavior
and sustained performance remain unaccepted. The
earlier volume requester (reported `TENEME`, possibly `THEME`) and subsequent
missing png.library>=52 failure remain documented. Neither was evidence of
filesystem corruption or a passed stability gate. An attempted passive
console reopen reset the board and lost the slow state. No-listener runtime
and interactive regression gates remain open; see the latest roadmap evidence.

The ordered path from the current storage bring-up to a graphical Workbench,
including mandatory hardware gates and the persistent evidence format, is in
[ROADMAP.md](ROADMAP.md).  Work on this port must update that roadmap whenever
a step starts, changes, becomes blocked or is verified.

The currently flashed core was superseded by the console-only candidate:
201,504 bytes at `0x20000`, SHA-256
`1d77ae9251e702b00ac68163bdcf61944f34f5a1207a90afa8fc5a8ad1a645e6`.
Write-time and independent core verification pass. The v2 BSP now carries the
double-tap candidate (only p4touch.hidd replaced; see Currently flashed below).
Artifacts and the pre-write core backup are in build
`evidence/console-runtime-2026-09-30/`. Runtime logs are intentionally lossy;
missing messages are not proof of missing execution. Interactive acceptance
of the hang fix remains open.

## Status

Native command-role preparation (2026-10-05): the source profile now retains
source-local USER_CFLAGS, USER_INCLUDES and USER_CPPFLAGS in its ordered target
flags. Target compiler/AR/RANLIB roles refer explicitly to the admitted native
toolchain, OBJDIR derives from GENDIR/CURDIR, and the measured GCC quote lane
is explicit. EXECSMP retains the quoted empty Make value from geninc.cfg.in
for this non-SMP profile; an unquoted empty value selects the wrong execbase
branch. The 80-input export now also declares the sealed configure-owned
geninc.cfg template and its explicit non-SMP substitution. The optional,
source-commented INCEMPTYDIRS list is explicitly empty, so directory admission
does not rely on undefined Make expansion. The export now declares actual-host
Make values separately: Linux uses `AROS_HOST_ARCH=linux`, macOS ARM64 uses
`darwin`, matching the inventoried configure inputs. A missing declared host
fails rather than borrowing another host's values. Current export SHA256 is
`d78394c3e06261be9d447cedcdc61a5089c7eac12c6a572c14cb9a45400d38e2`.
The preceding empty-list export was
`57863b094afb4d4365911ac2cb50cb7e020a24e73aab51ec0c2b6e9b1fb03e62`.
The preceding template export was
`c19b28b3a895dd248d499c3b1de494bf233c54568d2caca0e4d21eb7f2af48ce`.
The preceding command-role export was
`2c3134653e7b953ecb56ae4ccfd875737dd2ef1a25e0b35154ebbbf38399b574`.
These are configuration declarations, not a passed complete build. The
previous integrated checkpoint passes 268 common and 704 transpiler unit
tests plus its configured integration tests. New complete archive bindings,
ordered header pipelines and local directory prerequisites are being tested;
that previous checkpoint does not qualify these additions. The complete
configured, serial engine run now passes 42 tests, including generated-header
runtime refusal and the direct archive consumer. Source-diagnostic ownership
now excludes inert Make variable bodies and retains physical continuation
provenance. The complete configured v7 run passes 268 common and 782
transpiler library tests plus all enabled integration tests; five explicit
source-dependent probes remain ignored in that run. Its actual-source audit
visits 1,069 endpoints with no missing endpoint or unproven Make provider.
With materialized Ports sources, three selected failures and 14 unowned
diagnostics still prevent strict graph qualification. Subsequent optional-owner,
header and lint corrections require a fresh integrated run. A later diagnostic
run on the preceding export visits 1,068 endpoints, with two selected failures,
12 unowned diagnostics, no missing endpoints and no unproven Make providers.
Disabled TIFF no longer blocks its explicitly optional edge. Header provenance,
actual-host configuration and paired compile-sidecar attribution are undergoing
fresh verification; this does not qualify the native build. Exact header
identity preserves case; conservative collision
checks do not substitute for producer proof. The current common-only run passes
300 enabled tests. All 20 isolated directory-resolver tests pass after the
pre-copy size guard; shared Make-evaluation guards still need integrated
verification. Earlier recursion limits alone did not bound flat expansion.
Independent review also found that diagnostic macro arguments must follow
GenMF's double-quote grammar and reject indirect Make parser effects in flag
arguments. The configured integrated v14 run verifies those corrections and
whitespace-preserving foreach checks. The fresh CLI build succeeds as 0.3.19;
it is not a P4 payload result. Transitive GenMF helper verification, inline
invocation refusal and surviving-include refusal are implemented. An independent
review found no blocking defect in the bounded ownership search. Active
`build_prog` still vetoes diagnostic attribution because its implicit objects
and generated dependency inputs are not sealed. This does not disable native
program generation or convert an uncertain diagnostic into a foreign one.
Strict common/transpiler Clippy v19 passes. The configured integrated v14 run
passes 1,328 tests with no failures and 19 explicit opt-ins ignored, including
300 common tests and 834 transpiler library tests.
The whitespace correction preserves GNU Make foreach argument bytes, and the
finite 300-pair predecessor case now checks its exact owner within unchanged
limits. The real header binder now reconciles a legacy SDK Adhoc projection
only at the same physical declaration with an exact generated prerequisite;
duplicate, foreign, mismatched and malformed claims still veto the transaction.
Independent review additionally required refusing backslash path aliases; that
hardening and its counterprobe pass the v17 library and v6 real-source runs.
Fresh cold/warm v5 audits use binary SHA256
`b794de8f37127afb9307a4baafc019e53c4ac6a1e4f5eb228196873ba64637a0`
and export d78394c3: both visit 1,070 endpoints with no missing endpoint or
unproven Make provider. Warm has no selected capability failure but retains
12 unowned diagnostics; cold adds one deferred Zstd-source failure. All five
explicit real-P4 source probes and the cold/warm Zstd probe pass separately.
Strict validation still refuses unowned rules. Dependency-only object overlays,
boot images, resource objects and host tools need complete source effects and
owner closure; alternate architecture names alone do not prove irrelevance.
No full graph or payload is qualified.
The subsequent archive correction preserves prerequisite completeness for
both automatic `$^` and explicit finite `from=` recipes. Additional archive
targets, unresolved aliases and ambient conditional assignments are refused;
proof work has a shared byte budget. The exact source-member expression must
remain stable through recipe execution. Independent review accepts this
bounded correction. Its v20 archive tests pass 22 cases, with one explicit
source opt-in ignored; workspace Clippy v26 passes with warnings denied.
The serial v21 all-targets run passes 1,341 tests in 39 suites, with no failures
and 19 opt-ins ignored. The earlier parallel v20 run failed the documented
fork-window advisory-lock test; its assertion and implementation are unchanged.
The rebuilt v7 audit exposed a guard regression: libopenurl's disjoint object
pattern was refused, leaving one unproven provider and two warm selected
failures. The correction proves pattern disjointness using full-path/basename
Make semantics and retains bare `%.a` rules in the scan. v23 passed 26 probes
but failed the explicit-member disjoint-pattern probe: the effect guard still
confused Make patterns with GenMF references. The next correction uses GenMF's
actual lexical grammar and refuses unbound physical/deferred/home roots and
relative archive aliases; v24 passes 29 probes. A further GenMF-before-comment
counterprobe led to raw-line checking, including inactive Make branches and
unrelated recipes; v25 passes all 29 probes with that correction. Workspace
Clippy v28 passes with warnings denied; standalone v12 builds. Actual v8 P4/Zstd
probes pass 5+1 cases. The current v9 diagnostic audits restore 1,070 reachable
endpoints, zero missing and zero unproven providers; warm has zero selected
failures and cold one deferred Zstd input. Both still refuse 12 unowned rules.
The v22 broad serial intermediate run was cancelled after a newer counterprobe
invalidated its source checkpoint; it is not a complete success. Four residual
rules augment existing compile producers: their normal and order-only
prerequisites need distinct typed bindings and emission. The remaining
macro/include closure is not solved by treating common or depfiles as inert.
The preceding complete serial v29 regression exits zero: 1,347 tests in 39
suites, no failures, 19 explicit opt-ins ignored. Source-bound P4 probes remain
separate evidence. GenMF reference-order, continuation, character-offset and
newline corrections now pass an independent 1,167-input reference corpus.
The subsequent serial v30 common/transpiler run passes 1,411 tests in 41
suites, with 20 explicit opt-ins ignored. The new
`native-metamake-policy-v1.json` declares the native P4 project, ordered
projected globals, exact host/target environment, empty selectors and
explicitly absent names. It does not claim generated Classic configure
snapshots or execute configure. Its integration is in progress; the
85-input export and imported-template/recipe snapshots require fresh
qualification. Selected dependency overlays still require producer binding;
custom Make parser mutations are explicitly refused. This does not yet close the
12 unowned diagnostics or qualify the complete P4 payload.
Current source-error policy: active MetaMake dependencies cannot be removed
by native optionality declarations. The disabled TIFF pkgconfig owner and
unresolved architecture-selector endpoints must remain visible with
their declaring recipe and a `fix upstream` hint. Classic missing-target
tolerance is not a per-edge optionality proof. These are source review gates,
not permission to activate TIFF or invent empty producers.
The
Make quote-semantics regression and independent GNU Make comparison pass;
the configured transpiler suite passes 674 tests before the next compile-rule
integration. The archive backend's full configured engine suite passes 41.
These bounded tests do not qualify the complete native graph. The
77-input audit and full-suite counts below retain their earlier contract
identity; qualification of the new command-role projection is in progress.

Latest native preparation checkpoint (2026-10-04): TIFF remains unchanged.
Board and compiler profiles are separately forwarded and bound to build-tree
identity; all 36 engine tests and an independent bounded review pass. Typed
cold-source identities remain outside compilation targets, and 91 CLI tests
cover selected/unselected fetching and cold library roots. Real selected P4
preparation now checks the complete declaration closure. It exposed an absent
CPU-specific startup alternative, now explicitly declared optional in the
port contract without changing the generic startup producer. The current
77-input contract digest is
`3ff2b97b53b1ab788c513f73965e9d66655578adaec87b1aef76bf3c9917c8a2`.
It now binds the unchanged `mmake.config.in` discovery policy. Literal
ignored directory names follow Classic MetaMake at every depth; unresolved
configure substitutions hide no additional directories.
The native configuration explicitly declares the otherwise absent default
`INCLUDE_FILES` empty. This permits a proven empty `%copy_includes` endpoint;
unknown, conditional or opaque lists still refuse admission. Empty-copy and
copy/transform aggregate counterprobes pass in both declaration orders and
reject unrelated owners. The actual-source diagnostic traversal is recorded
below; it is not build admission.
The authorized GL header recipe correction names its local
`workbench-libs-gl-includes-copy` producer and retains its global
`includes-copy` membership. Classic GenMF confirms both routes. The
local/global route regression, focused Clippy and independent source review pass.
The preceding 73-input verification remains evidence for its original contract.
The new export binds the unchanged STDC consumer, GenCTBL Makefile and C source,
and explicitly pins both raw Unicode 16.0.0 files from the existing compiler
input lock. An independently host-compiled source generator produces the same
6,180-byte default table with Unicode 16 and the retained Unicode 18 raw inputs
(SHA-256 `7906b7ca6d758463ee1788febca54b7e0a5d864f63a6c5bb070b5f1300830209`).
The generic sealed host-C file capability now passes the actual unchanged
GenCTBL engine harness and the complete 37-test engine suite (serial).
No shared STDC/GenCTBL recipe changes. This is not a successful selected graph
or P4 payload.
Native translation now represents the handwritten GL and POSIXC-LFA stub
producers through a shared manifest-backed Genmodule writer. Its output set and
bytes match the unmodified reference tool; missing outputs rerun the writer.
Selected owner collisions, source-parent symlinks and unsafe subdirectories
are rejected. The initial diagnostic-only graph audit traverses 1,066 known
endpoints and finds eight missing endpoints and five unproven Make providers.
Six missing endpoints are generated ABI-header edges for plain static archives;
the other groups concern concrete archive and copy producers. The 140 ownerless
diagnostics require ownership/selection analysis, not an assumption that all
foreign architecture recipes belong to P4. No GL, POSIXC or TIFF functionality
is disabled.
The latest 77-input audit visits 1,060 endpoints: one missing endpoint,
one unproven Make provider, three selected capability failures and fifteen
unowned diagnostics remain. The configured transpiler suite passes 652 tests
(one existing ignored); Clippy is clean. Source-declared SDK bin aliases and
literal manpage stubs now have concrete producers, architecture-aware file
binding and byte/incremental/refusal probes. A final configuration guard rejects
changed program output identities. Real competing archive providers remain
rejected. Normal/relative client-archive suppression and cross-kind ownership
fixtures pass; the complete engine suite passes 40 tests. The report is
diagnostic-only; it refuses native graph admission.
Archive preparation now preserves concrete source-owned endpoints, rejects
missing/ambiguous selected archives, and matches raw-link visibility to the
full resolver. Its 583 library tests, all-target Clippy and bounded independent
review pass. The earlier Unicode-generator refusal retains its original
evidence identity. The current audit publishes only a diagnostic report, never
a build graph or source inventory. ELF-to-flash-BIN conversion, Developer FAT16
production, kernel compile/link parity and successful-build receipts also
remain open. The fresh core/BSP/Developer build remains
open. No successful native payload or complete flash plan is claimed.

Native aros-tools integration (2026-10-04) is in development in an isolated
worktree of `763a536f26a5e94176c821d48cedbcbff03b4189`. The root
`aros-targets.toml` declares the D1001 GNU/RV32 selectors and public ABI;
its explicit `toolchain_profile` selects the separate local RV32 compiler
candidate. This allows compatible board presets to share a compiler identity
without renaming or rewriting that compiler's measured manifest.
`native-build-v1.json` binds the core, package and media facts to their source
inputs. These declarations are not native-build or flash qualification.
The export explicitly selects the source-supported `ldscript-xip.lds`; the
raw Make default `ldscript.lds` remains unchanged. Its `riscv32-xip-v1` policy
records `.sramtext` and the source check's half-open flash/SRAM address ranges.
The native engine uses an owned verifier for this linear named-reference
lint; the contract names no executable command and does not authorize running
the source script. This is not a whole-program control-flow residency proof.
They do not enable SMP, change drivers, or qualify newer parallel port work.
Fresh compiler attempts exposed source-fetch invocation and Unicode source
publication defects in the tools path; both have local corrections, and a
third cold RV32 compiler attempt stopped before compilation: this source lacks
the restricted compiler-release target and its broad fallback reaches ACPICA.
Fabian authorized the two isolated source build-rule corrections on 2026-10-04.
The FDT include-copy rule now explicitly produces `linklibs-fdt-includes`
and lists the actual `fdt.h`. The opt-in `crosstools-release` selects the
existing GNU compiler-only rule and explicit build/install goals for `libgcc`,
not the generic GCC root that also requires broad SDK headers and runtimes.
Configure guards and source-rule probes pass; a fresh compiler build remains
unqualified. Fabian then authorized local P4 architecture fixes. The Exec
recipe now supplies `kernel-exec-includes-esp32p4-riscv` through the existing
kernel-header producer; this rule is part of the current source export.
Native selection passes that edge. Generic directory preparation now has real
CMake targets and rejection tests. Source-owned host-C header generation now
builds the actual MUI generator in a cold private fixture and matches its output
and SDK mirror to direct reference execution, including incremental rebuilds.
Five handwritten GENMODULE layouts generate exactly 54 declared headers with
the real source tool; no-op and missing-byproduct repair probes pass. Literal
header copies preserve their single source-declared destination. These are
generic tools capabilities, not ESP32/MUI switches in Rust. MUI libraries and
GUI selection remain source-owned; required SDK headers are not disabled.
Explicit empty virtual MetaMake targets are retained, but cannot hide an
unmodelled nonvirtual Make provider. Actual P4 selection now passes the zstd
and PNG producer edges. The closed whole-line header interpreter matches
cached libpng output against GNU sed and preserves literal multiline data
through real Ninja commands. The source contract declares empty
`P4_C2_INTUITION_GATE`, `P4_C3_GRAPHICAL_BOOT` and `P4_C4_TOUCH_HIDD` defaults,
matching the reference's unselected optional gates. Local Make assignments
retain precedence; unknown writes cannot be hidden by defaults. Both CMake
transpiler passes and replay records forward validated native identity.
Both FreeType products now have source-derived producers: the complete option
header and executable cross-config script match independent GNU sed output
from the exact cached templates, including script mode `744`. CMake consumer
fixtures pass with the generic producer and the older fallback, without a
duplicate output rule. The finite SDK file-copy producer now preserves actual
Codesets FD bytes through a private CMake/Ninja fixture, with rebuild and
missing-output repair checks. Selected translation passes Codesets and the
source-owned Resource ABI endpoint. The full Resource header/libdefs/FD
fixture matches direct reference-generator bytes, including config-owned
card/cardres names, the actual flashdisk device and base/override defaults.
No-op and missing-header repair pass. Runtime-only module macros do not
acquire public ABI aliases. Generic finite static-pattern copies now stage
all 81 actual network headers byte-for-byte into their sole source-declared
SDK root. Directory lists retain declaration-time Make state; root overrides,
unmodelled commands and unsafe paths are refused. The selected graph now
passes the m68k and i386 named host-header aggregates. Their four declared
SDK/GEN outputs match direct reference generator execution; m68k has no
invented GEN mirror. Incremental builds reject even future-dated tool/header
symlinks and changed sealed recipes without rewriting valid headers.
437 library, 54 CLI and the preceding 20 embedded-engine tests pass, as does all-target
transpiler Clippy. Fabian separately approved the local Icon prerequisite
correction to `workbench-libs-cgfx-includes` and removal of the two orphaned
Vega aggregate entries. Fabian subsequently approved the same Cybergraphics
prerequisite correction in Colorwheel and Coolimages, and removal of the
unowned `workbench-libs-mesa-includes` aggregate entry. A subsequent aggregate
audit confirmed absent LCMS2/PCI sources and the incorrectly named Codesets
include producer; the separately approved three removals and one rename are
also local. The actual Mesa/GL header producers remain unchanged. These are
host-only checks, not a complete SDK build.
The current 69-input export has SHA256
`15fe94bbfc18b672ffbb3c2446f7f17374918745895437d4d7acae85978ddb2d`;
earlier receipts keep their original digests. Native packaging rejects missing members and capacity overflow
before publication; exact-boundary and retained-output counterprobes pass.
The native core now consumes registered source-derived KOBJ intermediate links
rather than raw object mirrors. Fresh P4 core qualification is still open. Exact full,
runtime-only, ABI-only and simple macro forms are now retained separately.
The standalone KOBJ localization CLI matches an independently reproduced GNU
recipe byte-for-byte on a real 2,096-byte RV32 collected object. Its 71 ordinary
Collector tests, explicit real-tool probe and all-target Clippy pass. This
does not establish complete source flag/archive parity. The CMake KOBJ helper
now matches a separately assembled real RV32 partial link and GNU localization
recipe byte-for-byte (2,632 bytes). Source-derived compilation groups preserve
Make's architecture-object order, fetched logical stems, deferred quote paths,
transitive include usage and `-noposixc`; full/simple fixtures and twelve
refusal probes pass. Simple modules exclude CXX/ASM from KOBJ source resolution;
disabled Objective-C remains declared metadata, not a missing KOBJ input.
Declaration-scoped extra objects, libraries and linker flags now retain ordered
values and explicit uncertainty in the generated CMake metadata. Its consumer
refuses unresolved inputs, including conditional values frozen by `:=`.
The declaration metadata also retains the actual macro `uselibs` list, global
KOBJ flags/script and effective function-instrumentation selector. An absent
selector is resolved from `TARGET_FUNCINSTR`, not assumed to be `no`. The bridge
refuses missing configuration proof and enabled instrumentation until its
compile contract is bound. The latest scoped-input suite passes 22 focused
tests; 462 library, 54 native CLI and 24 embedded-engine tests pass. A separate
real RV32 fixture verifies source groups, collection/localization, core linking
and identical no-op/rebuild outputs; it is not a complete P4 build. A source-owned
native KOBJ configuration projection now preserves the four global source defaults,
including an undefined `KOBJ_LDFLAGS`, and excludes generated global overrides.
Core recipes and local options are hash-bound. Its bounded reader/schema/process
integration passes 482 library, 55 native CLI, 23 contract and 24 engine tests,
plus all-target Clippy. All eight scoped fields of the six actual P4 core
members resolve without uncertainty. Independent review and counterprobes
confirm that unknown Make flavor and wildcard traversal errors cannot select
guessed includes. Generic header-only projection now retains Gallium's source
config independently of the rejected Mesa runtime capability. Three projection
regressions, 482 library, 55 native CLI and 25 engine tests pass; no runtime,
archive or KOBJ owner is fabricated. A fresh source-built reference generator
also produces Gallium's actual headers, FD and libdefs through the new helper;
all 23 private/published regular outputs match independent direct execution.
An unchanged Ninja invocation performs no work; a missing SDK header is
repaired, preserving all 23 output hashes.
The actual generator remains responsible for validating config syntax and
output facts. Fabian separately approved
removing EGL's include aggregate because its source module is commented out.
Fabian separately approved correcting the Workbench input-device header name
to the actual `kernel-input-includes` producer; the recipe is now inventoried.
Its separate real-generator fixture produces 23 regular header/FD/libdefs
outputs byte-identical to direct reference generation; its next Ninja run
performs no work. This fixture does not compile Input or qualify the full SDK.
Fabian separately approved changing the undefined ROM ACPICA header reference
to the actual `kernel-acpica-sharedlib-includes` producer; that aggregate is
now inventoried. Generic handwritten SFDC header support derives mode, target,
input hashes and SDK outputs from the source. The privately built host compiler
and three actual CIA headers match original host-tool/direct generation bytes;
no CIA source change was needed. No-op, missing-output repair, source/tool
mutation, unsafe paths and exit-zero-without-output checks pass. The current
contract explicitly declares the ten compiler selector edges as optional and
binds their recipe plus MetaMake reference implementation. Native admission
proves each edge in that exact virtual source declaration. Nine absent
alternatives are recorded and omitted; the existing architecture endpoint is
retained. Literal missing edges, unknown selectors, rejected real producers and
overlapping unmarked source declarations remain fatal. No empty target is
invented. Recipe proof and its digest now use the same raw-byte snapshot,
including legacy text encodings; independent review and 60 CLI tests pass.
The generic `linklibs-autoinit-autofile` copy capability matches the actual
295-byte GCC specs input through CMake/Ninja, including no-op and missing-output
repair. The same recipe snapshot determines its proof and measured identity.
Uninstantiated GNU Make `define` bodies cannot invent a live file list or flags;
opaque definitions and frozen aliases cannot reuse an earlier known value.
Source-owned standalone SDK-object compilation/staging is connected to the
native graph for the real `linklibs-startup` provider. All eight actual source
objects retain their C/C++ lanes and positional flags. Its bounded host
C/C++ fixture passes 16 configure refusals and seven runtime symlink refusals.
The native engine's source-role binding and generated-header propagation pass
the five-mode program-link fixture; a duplicate legacy C++ edge that caused a
cycle was removed only from the native path. Three real link consumers reject
missing C/C++/detach startup roles. Generic `%rule_makedirs` support advances
full selected translation to `libinit-libentry`. That compile-only frontier
now has a generic native object producer and a source-owned normal GNU compile
policy, including repeated ISA/flag lanes and the current Developer sysroot.
Four actual P4 source probes pass on the preceding 63-input export. Its
literal integration passes 544 library, three projection, 64 CLI and one
GNU Make reference tests; all 30 embedded-engine tests pass. Full selection
advances through libinit to absent ROM architecture additions. The current
export binds six unchanged ROM recipes and marks only their exact optional
selector edges. Concrete producers still need ordinary capability proof;
existing or rejected architecture endpoints are not omitted. Current graph
replay passes the former `linklibs-stdc.static` gap by binding only generated
uselib edges to the actual `linklibs-stdc-static` archive. Handwritten edges,
known endpoints and ambiguous providers remain mandatory. Their selector
provenance is retained through native selection. The final archive correction
passes six focused tests and independent review. Source-value integration
passes 562 library tests, nine scanner tests and 67 CLI process tests; all
33 embedded-engine tests pass. The actual ABI text output matches independent
GNU sed, without a constant substituted in the tools. The current 71-input
export additionally binds the unchanged ABI recipe and input; all 71
altered-input refusals and four actual source probes pass. Full translation
now stops at `workbench-libs-tiff-pkgconfig`: its required owner declaration
is commented out in the source, although its recipe exists. Permission to
activate the two declaration lines is pending; the dependency is not omitted.
Distinct local-only compiler metadata and CLI admission pass focused tests.
The real GCC 16.2.0/binutils 2.47.20260726 compiler also passes isolated CLI
install/path/verify admission on a byte-identical retained copy. Changed tools
and coexisting release metadata are rejected before execution; the historical
prefix and source inputs remain unchanged. A real embedded-engine CMake
consumer also admits that prefix with source-derived RV32 options, all runtime
archives contained in it and no release identity; this is not a payload build.
The extended SDK-text capability passes 573 library and 71 CLI tests. Its
first-match semantics match GNU sed, including mixed line endings; source-bound
CMake/Ninja rejects recipe drift without replacing the last valid product.
All 34 embedded-engine tests pass. Build-tree stamps reject local
identity changes and local/release mixing while retaining released stamp bytes.
Native media receipt integration remains open.
Local native pre-build stamps now bind the whole actual dirty source tree,
compiler, regular engine files, executables and configure inputs separately
from release provenance. A dated snapshot includes 28,436 entries and about
1.03 GB of regular files. CLI probes reject source/configuration drift, linked
engine inputs and symlink-directed cleanup without touching external sentinels.
Recursive submodule commit selection and discovered executable bytes are also
bound. Configured DTB and the three consumed KOBJ members are measured even
inside the selected generated namespace; in-build mutation counterprobes pass.
These checks do not establish a successful native build or media receipt.
The commented TIFF pkg-config owner remains unchanged. A classic MetaMake
fixture skips this missing metadata child, executes the real TIFF linklib
sibling and exits successfully. The native contract now names that exact
disabled owner and hashes its unchanged recipe; the translator verifies the
comment from the same parsed byte snapshot. All 76 CLI selection probes and
574 transpiler library tests pass. The subsequent ZSTD investigation found
lost deferred-source metadata, not a disabled library. The corrected tools
retain all four wildcard groups and prepare inventories without publishing
a partial graph. A real cold P4 preparation identifies 15 fetch owners;
the isolated warm ZSTD parse verifies its 30 sources and client-archive endpoint.
All 78 native CLI selection tests pass. Full core/BSP/Developer compilation
and media qualification remain open; no source-built payload has been borrowed.
No release identity is invented. Full kernel/BSP/Developer qualification
remains open. The earlier SDK integration
passes 529 library, three projection and 62 CLI process tests. An independent
GNU Make comparison also confirms implicit lane selection when an unrequested
aggregate names the intermediate alongside an unsupported header. A separate
literal compile-only capability passes real C/C++ fixtures; it does not borrow
unknown generated Make flags or stage its output into the Developer library.
The export binds the unchanged startup recipe, eight source files and local
header, and explicitly selects inactive `STARTUP_DEBUG`. No startup source is
modified. A complete P4 graph/build and flash plan are still unqualified.
Actual core/BSP qualification
remains open. These bounded
checks are not native core/BSP/Developer acceptance; missing
source edges are never silently discarded or replaced with empty aliases.
Fresh isolated ESP-IDF bootloader production passed with a hash-locked Python
environment and verified vendor tools. The unsigned image's chip, silicon
interval, capacity, XOR checksum and appended SHA-256 passed independent Rust
inspection. Public `aros image prepare` now also passes a fresh offline
process test on macOS ARM64, producing the external bootloader and source CSV
partition table. This is not a native core/BSP/Developer build or complete
flash workflow.
Rust-owned vendor runtime preparation now independently creates a fresh
offline CPython environment from all 61 exact wheel hashes. Installed package
versions, private import origins, local pip report URLs/hashes and unchanged
host-runtime content pass; changed wheel bytes are refused before an output
root is reserved. This local-byte receipt is not distribution provenance or
the complete ESP-IDF producer. The internal Rust IDF adapter now also copies
the four inventoried project inputs, checks exact dependencies/constraints and
builds a fresh 22,544-byte bootloader with measured vendor tools. Parent and
nested CMake selections and unchanged source/runtime inputs are checked;
component-manager fetching and submodule updates are disabled. Independent
image checks pass. A private executable alias PATH excludes vendor/wheel
directory shadowing; all 16 source-input mutations are refused before build
reservation. Fresh fetch receipts now bind the exact IDF archive, ordered
source patch and live tree. IDF builds a byte-identical private source copy;
only its regular Git index may refresh, while original inputs stay unchanged.
The receipt records both tree/index digests. GCC/CMake/Ninja now also require
archive-tree receipts matching the locked IDF host manifest; the selected
executables cannot escape those prepared trees. Final owned-copy execution
with the pinned-snapshot guard passed; fresh production using all three
archive-bound tool trees also passes image verification and unchanged-input
checks. The public preparation receipt binds these inputs and explicitly says
`complete_flash_plan:false`. Native core/BSP/Developer and complete media/CLI
gates remain open; no device or flash operation was performed.
The source CSV also resolves directly in Rust and matches every record in the
fresh partition binary, including its MD5 checksum and erased padding. The
native Rust encoder now reproduces that 3,072-byte table byte-for-byte from
the bound source CSV; its separate binary decoder and corruption checks pass.
It is not yet exposed as a complete public P4 media workflow. A
source-bound generation adapter checks the selected contract's exact digest
and all inventoried inputs before and after encoding, and rejects even in-tree
CSV symlinks. The inventoried `media-geometry-v1.json` now supplies numeric
layout facts to a separate adapter, not board defaults compiled into Rust.
The current export binds 24 source inputs, including the approved Icon/Vega
corrections, P4 Exec include edge, corrected compiler/FDT build rules and the
existing bootloader recipe;
its partition roles resolve from the CSV and must match the exported exact
core/BSP type and subtype, not merely an app/custom type category.
Geometry combines board declarations
with pinned ESP-IDF format facts: chip ID 18, bootloader placement and a 4 KiB
planning sector. The source export is not vendor provenance or a live NOR-device
measurement. Its consistency binding and verified external containers do not
qualify native core/BSP/Developer producer receipts. The
generic read-only planner passes fixture tests for separate payload and erase
spans: the BSP prefix ends at 0xc00000 and the 4 MiB Developer volume occupies
0xc00000..0x1000000 inside the same `arosbsp` partition. This is not a composed
native flash plan; complete producer receipts still need integration.
Fresh native core/BSP/Developer builds and independently verified complete
flash artifacts remain required. No device was accessed for this integration.
The retained RV32 reference compiler now passes a compiler-only isolation
probe: a copied prefix builds C/C++/assembly and a raw libgcc partial link
against a new empty sysroot. Actual include-search paths remain inside the
copy; access to old AROS SDK headers and a wrong float ABI are rejected.
This does not create a producer manifest, a native Developer SDK or a P4
kernel/BSP result. The native CLI still needs a qualified compiler input and
the separately approved source-closure corrections.

Nothing in the table below is claimed to work until it has been observed
working on hardware. `stub` means the file exists and compiles; `works`
means the behaviour was verified on a board and how it was verified is
recorded in the milestone notes.  Documentation is part of the definition of
done: a status change must update this table, the matching roadmap phase and
its evidence entry in the same change.

| Area | State | Notes |
| :--- | :--- | :--- |
| graphics performance | hardware partial | C1P tiled rotation, CPU360 and framebuffer v2 coalescing reduce the measured first full update from 2.776 s synchronous/CPU90 to 0.296 s queued/CPU360. Initial visual speedup confirmed, then slowdown/hang reported. Console-only nonblocking fix is sanitizer-tested, flashed and independently verified; causality and fresh visual/touch/no-reader acceptance remain open. See C1P and dated evidence. |
| configure target | done | `--target=esp32p4-riscv`, configure completes |
| board selection | hardware partial | `P4_BOARD=d1001` selects shared C/Make wiring and rejects unknown boards; the new core reports `board d1001` on hardware, but the retained PSRAM no-answer state prevents the graphical/BSP compatibility gate, and the full aggregate build remains open |
| crosstools | done | binutils 2.47 and gcc 16.2.0 for riscv-aros, link libraries built |
| rv32 CPU layer | done | M-mode CSR names, FLEN-aware FPU context, cache clears, backtrace, single-precision fenv, ABI-aware stub frames |
| kernel arch layer | compiles | `gmake kernel-kernel-esp32p4-riscv` builds all platform objects |
| exec arch layer | compiles | `gmake kernel-exec-esp32p4-riscv` builds all seven |
| trap entry | compiles | frame offsets asserted against the struct |
| link script | written | test-linked, both SRAM windows asserted |
| bring-up report | works | hart, misa, ids, section extents, then a repeating heartbeat |
| kickstart link | done | 132160 bytes of the 167 KB window, one segment at 0x4FF00000 |
| flashable image | done | `gmake kernel-esp32p4-riscv`; diagnostics-off image 138608 bytes, F0 hardware-diagnostic core 148848 bytes with hash in the roadmap |
| runs on hardware | **yes** | patched ESP-IDF second stage maps the XIP image from ota_0; see below |
| watchdogs | done | timer groups and LP off, super watchdog self-feeding |
| CLIC interrupts | done | a raised line reaches the trap handler |
| SYSTIMER tick | done | 100 Hz sustained, 1201 ticks over 12 heartbeats, 16000000 counts per second |
| context switch | done | tasks are entered in M-mode and their syscalls dispatch |
| preemptive multitasking | done | two equal-priority tasks share the CPU within a few hundred counts of each other over 11 heartbeats |
| memory list | done | 626384 bytes across two internal regions, plus PSRAM as a third region; every byte of SRAM is accounted for.  The 393216-byte data-only bank is the port's lower-priority `MEMF_CHIP` compatibility pool, so classic `AllocRaster()` users such as `Text()` work without consuming code-capable SRAM |
| kernel.resource | done | initialises, KernelBase built, context size 283 |
| exec.library | runs | SysBase, both InitCode passes, AvailMem and AllocMem answer |
| serial debug console | not started | UART0 |
| code from flash | done | .text and .rodata mapped from the app partition, 117744 bytes of SRAM returned |
| PSRAM bring-up | hardware partial | The D1001 LDO2/1,800-mV path recovers 32 MB without a Vellum preflash: XPD changed from off to on, chip identification succeeded on the first attempt and all 32 MB verified; a second USB reset passed. The old Vellum preflash was a workaround, not a power contract. With inherited 90-MHz CPU and 20-MHz PSRAM, the graphical artifact reached Wanderer in UART but showed Fabian only a light-blue field and flickering edge strip. Changing only PSRAM to 200 MHz restored clean calibration and a spontaneously reported visible desktop. That report was not a synchronized visual gate; an earlier 360/200-MHz C1 strip is not explained by this comparison. The global-reset candidate, matched full BSP build, controlled visual gate and physical-cold-boot gate remain open; see D0 in [ROADMAP.md](ROADMAP.md). |
| CPU clock | B1 hardware verified | the port configured none and inherited 90 MHz from the bootloader until 2026-08-23.  `P4_CPU_MHZ=360` moves the four root dividers to CPU /1, MEM /2, APB /2, measured back as 360 MHz from `mcycle`.  Sequential PSRAM reads go 20 to 60 MB/s and internal SRAM 25 to 101 |
| BSP package from flash | C2 and C3 hardware verified | the accepted C2 package has 30 members.  The accepted C3 package conditionally adds the normal console, RAM/CON handlers, `misc.resource` and `gadtools.library`: 34 members, 3,275,492 bytes, SHA-256 `7fd83f5c64d38928f7928f24b351461e6d28e3dad98909e052080edd7de0e8b`, all accepted by `boot/audit-package.py`, while preserving exact C2 reproduction |
| timer.device | done | a 500 ms timerequest on the VBLANK unit returns after exactly 50 ticks |
| SD/MMC block device | A1 hardware verified | read-only native DesignWare-MMC/IDMAC path.  Every read goes through the IDMAC, as in ESP-IDF.  One run passes 59 card-referenced cells, 1,000 repetitions and the invalid-request rejection cases; separate runs pass the three injected fault modes with CMD12/CMD13 recovery and the heartbeat.  Two gate points are met differently and documented in the roadmap |
| Partition discovery | A2 hardware verified | bounded MBR/GPT/EBR reading: range and overflow guards in the common funnel, GPT header and entry-array bounds, an EBR visited set and depth limit.  The card reports exactly its one partition; eleven malformed tables from `ramtest.device` are all refused within 4 to 36 sector reads |
| Test image | A3 hardware verified | reproducible FAT32 image built on the host without root or external tools, one MBR partition at LBA 2048.  Checked by the host's own parser, by `fsck_msdos` and against its manifest, byte-identical on rebuild, and read correctly on the board at the values predicted from the image.  The generator also writes the volume label as a root directory entry, which is where AROS's FAT handler reads it; without it the volume was named from its serial.  See [image/README.md](image/README.md) |
| Code loaded from SD | A5 hardware verified | a command and a library, in neither the kickstart nor the flash package, loaded from FAT by DOS/LoadSeg and lddemon and run from two different callers, with every address outside the resident ranges.  Four refusal cases fail with the reason named and nothing leaked.  See [proof/](proof/) |
| DOS/FAT boot | A4 hardware verified | boots from the card to a Shell prompt on the emergency console, which accepts typed input.  dosboot replaces the whole-disk node with the partition node, `AROS.boot` is accepted, `SYS:` is assigned from the volume, `Info()` reports `ID_WRITE_PROTECTED` and eight DOS mutations are refused with error 214 leaving the medium bit-identical.  A card without `AROS.boot` unmounts cleanly and reaches the same prompt |
| Flash development volume | hardware verified | a FAT16 volume in the last 4 MB of `arosbsp` at flash `0xc00000`, written with `esptool` instead of by a card handoff.  `flashdisk.device` serves it read-only from the kickstart and registers a boot node at priority -10, so it mounts as `FLASHDISK0P0:` on every boot while a present card still wins.  A file read off it is byte-identical to the same file on the card, and with `bootdevice=FLASHDISK0P0` the system boots out of flash and loads the A5 proof library and command from it.  With no card in the board the flash volume is booted automatically, because `sdcard.device` then registers no boot node at all |
| I2C and panel power | B2 hardware verified | an I2C master for both P4 controllers in `kernel/i2c_hw.c`, OOP-free and shaped for the `WriteRead` method of AROS's `hidd.i2c` class, following the `arch/riscv64-opensbi/hidd/dwi2c` precedent.  I2C1 answers at 0x18, 0x20, 0x40, 0x51 and 0x6a.  The panel supply and reset pulse run twice and return to safe with a deliberately seeded unrelated bit provably unmoved; the backlight is never enabled.  The expander survives a CPU reset and, because the board has a battery, has no reachable cold state |
| MIPI-DSI PHY and command path | B3 hardware verified | the PHY supply, clocks and PLL lock; all three lanes reach stop state, the full JD9365 sequence transmits, and post-handover DCS power mode confirms the panel is on and awake.  The working B5 profile uses 1500 Mbit/s lanes |
| Internal test pattern | B4 superseded | the isolated host generator stayed dark, but the now-working B5 framebuffer path proves the complete bridge, host, PHY and panel path and is the useful display gate |
| Native PSRAM framebuffer | B5 hardware partial | a complete, clean `800 x 1280` RGB565 grid scans from PSRAM with `P4_PANEL_VMUL=1`, 40 MHz pixels, 1500 Mbit/s lanes and no payload error.  A hardware-observed colour card verifies native little-endian RGB565 and the primary/pair-colour mapping; the corrected exactly tiled checker is perfect and moving dirty rectangles leave no visible artefacts.  A raw-source ruler measures a constant cyclic row displacement of exactly 525 pixels.  The explicitly named +525 write-mapping **workaround** makes asymmetric quadrants, four differently sized corner marks and isolated one-pixel lines geometrically correct on the D1001, but does not fix or define the native scanout mapping.  The sustained gate passes 1,800 seconds of concurrent 71 MB/s scanout, read-only 128-sector SD reads and cache-forced 1 MB PSRAM write/read passes with 1,800 clean status samples and zero failures.  The measured 71 MB/s matches the 69.3 MB/s imposed by frame size and pixel timing; it rejects a full-frame CPU transform and permits the workaround to be fused temporarily with B6 dirty-rectangle rotation.  B5R separately requires the same raw linear framebuffer to scan correctly with compensation disabled and removal of that mapping.  Ten consecutive controlled EN warm resets of the unchanged +525 artifact each recovered PSRAM in one attempt, completed 2,146 frames with zero faults and stopped safely.  A real cold boot cannot be initiated in software on this battery-backed board; a separately named forced-peripheral-reset development gate may unblock B6, but remains distinct from the still-required physical cold-boot evidence.  The optional post-video DCS read that can independently pin `GEN_RD_CMD_BUSY` is excluded from acceptance runs |
| VSYNC handoff and landscape rotation | B6 hardware verified | the ESP32-P4 v1 bridge has no VSYNC interrupt, so the proven GDMA one-frame completion is the ownership boundary.  Two complete native PSRAM surfaces are reserved outside Exec and expose logical `1280 x 800` content through the reference's 90-degree-clockwise CPU transform.  The immutable gate completed 2,006 frames and 19 exact source switches without DMA, bridge or host faults; direct observation confirmed correctly placed complete alternating images with no visible tearing.  The producer gate then completed 60 bounded inactive-surface updates, 60 requested frame-boundary handoffs and exact rotated row-range writebacks with zero rejects or transport faults; direct observation confirmed exactly one clean moving rectangle with no stale pixels, split frame or tearing.  The +525 row mapping remains explicitly named `P4_B6_ROW_PHASE_WORKAROUND` and removable through B5R |
| MIPI-DSI framebuffer HIDD | C1 hardware verified | the shared `fbgfx` family registers exactly one logical `1280 x 800` RGB565 boot mode over a versioned kernel framebuffer-operations contract.  Three mode-bound displayable bitmap allocations, `Show`, four coloured fills, both diagonals, six real `Text()` rows and a full update pass on the D1001; direct observation confirms correct landscape orientation, colours, centred geometry and readable white text.  The final gate counts 1686 text pixels and reports 14 frame-boundary swaps, zero DMA faults, zero rejected updates and no pending surface before the normal Shell boot, with SD still read-only.  The missing-text defect was not a HIDD workaround: the port now preserves its internal data-only SRAM as a real `MEMF_CHIP` compatibility pool for `AllocRaster()`.  The +525 physical-row mapping remains an explicitly named workaround below the HIDD and B5R remains its removal gate.  The display contract is frozen in [display/DISPLAY-CONTRACT.md](display/DISPLAY-CONTRACT.md) |
| Layers / Intuition screen | C2 hardware verified | a 30-member package contains Layers, Keymap, Intuition and the complete generic input skeleton required by `input.device`, with no fabricated input events.  The D1001 loads all members, installs Intuition's display callback at priority 15 before fbgfx insertion at 9, creates the monitor, then a priority-8 resident opens a custom `1280 x 800` Screen and two overlapping titled simple-refresh Windows before dosboot at -50.  A priority-5 worker runs after multitasking, waits for real `LAYERREFRESH` damage from asynchronous depth changes, consumes both `IDCMP_REFRESHWINDOW` messages, redraws through `BeginRefresh()`/`EndRefresh()` and finally scrolls the front window.  Direct observation confirms readable text, correct overlap and clipping, the intended scroll strip, four corner marks and a stable red pointer; the grey unrefreshed-area failure of the first candidate is absent.  The final unchanged package passes the complete marker sequence, read-only SD discovery and Shell on 20/20 EN-reset boots with one normalized marker hash.  This reset series is not an unplugged battery cold-boot claim |
| Normal read-only Wanderer boot | C3 hardware verified | Normal boot mounts the prepared SD as read-only `SYS:`, executes Startup-Sequence, installs fbgfx and loads Wanderer, its Zune classes, preferences and volume icons.  The first real `ENV:` multi-directory read exposed and led to a generic DOS FileHandle routing fix.  Wanderer correctly detaches and remains alive; the corrected sequence closes only the initial CLI with the standard `EndCLI` pattern.  The exact 64-MB, 89-entry image has SHA-256 `7012189035197c3ccfcee84c95a53bd39fc7b5d4d43c9ee26a6962f370859758`.  A first-byte 60-second D1001 capture has no trap, Alert, panic, Guru or fallback prompt.  Direct observation confirms a persistent, correctly oriented Wanderer desktop with readable title, visible drive icons and all four logical edges complete.  With the SD removed, two first-byte captures reproducibly report `GPIO45 high: no card present`, boot the read-only `FLASHDISK0P0` fallback and reach a visible graphical recovery Shell without a trap, Alert, panic or Guru.  The normal artifact passes 20/20 controlled EN-reset boots with one normalized gate tuple; this is not a physical battery-cold-start claim.  It also retained the complete, artefact-free desktop for a directly observed 1,800-second UART soak with no fatal marker.  That normal run is intentionally combined with B5's accepted 1,800 seconds of active read-only SD traffic under concurrent scanout; it does not misattribute periodic reads to the idle Wanderer desktop.  Two initial integrity attempts failed honestly because macOS inserted only `.fseventsd`; all 89 intended entries still matched and those failures remain in the evidence log.  The corrected host helper holds the raw card exclusively through write, synchronization and eject.  Hardware-locked first host insertions around exactly one normal D1001 boot then produced a prepared image, pre-run readback and post-run readback that are byte-identical at the image SHA above.  This closes the complete GB0 gate |
| touch HIDD | C4 hardware partial | the D1001 answers at I2C0 address `0x40` with Silead ID `0x50910000`; GPIO16, reset on PCA9535 output 12 and the initially empty RAM-firmware state are measured.  No firmware blob is committed because redistribution authority is not established; a validated converter accepts a user-supplied vendor header or binary outside the repo.  The maintained absolute `mouse.hidd` path, persistent task-context I2C0 polling, direct-X/mirrored-Y normalization, scheduler idle correction and bounded press/release policy pass synchronized motion, three-tap and centre/four-edge observation.  The complete coherent 44-byte frame supports up to ten contacts while retaining the closest continuing contact as the visible pointer; its FIFO-segmented long read and actual independently moving two-contact frames are D1001-verified with zero errors.  Both contacts report ID zero, so positions rather than IDs deliberately drive continuity.  Two contacts promote the active press to Button2 and latch it through sequential finger release; synchronized UART and direct Intuition observation verify the menu/right-button response, absence of a spurious left re-press and clean final release.  Version 4 removes firmware from the normal core and loads the validated 34,848-byte D1001 record image in HIDD task context from `DEVS:Firmware/silead/gsl3670-d1001.fw`, with a development-only `FLASHDISK0P0:Firmware/...` fallback.  D1001 captures verify both properties: missing firmware leaves only touch disabled and does not block Wanderer, while the external fallback loads all 4,356 records, reaches status `0x5a5a5a5a` and polls without errors.  The bounded reload after repeated I2C errors is implemented but still lacks fault-injection hardware evidence, as does the full 1,000-cycle C4 soak |
| second core / SMP | transitional Giant: ordinary tasks on both harts, 30-minute stress passed (headless) | `P4_GIANT=1` makes Forbid()/Disable() system wide with two fair SRAM locks and brings hart1 into Exec from a COLDSTART resident; `P4_GIANT_MIGRATE=1` lets new tasks run on either hart. Headless D1001 evidence: self-test 10/10 (concurrency, exclusion, signals, semaphores, messages, AllocMem, FPU, RemTask eviction, cache-off park) and a 30-minute stress of four migrating workers next to the desktop with 15.4 M rounds, 0 errors and hart1 running Wanderer, input.device and the touch poller. A named workaround, not the E3 design. Visual/touch acceptance pending. See [SMP.md](SMP.md) and Track E. |

E2 is hardware verified within its two-hart primitive scope. The exact
219,776-byte P4_E2_PRIMITIVES candidate passes five ordinary headless captures
with both reset-separated epochs, including SRAM/PSRAM publication/refusal,
measured atomic contention, software IPI/remote fence.i and cache rendezvous.
The complete normal core range is restored and independently verified;
normal headless PSRAM/Wanderer/touch boot passes. No new visual/touch or
long stability acceptance is claimed. Normal Exec remains single-hart.
E3's parallel Luna CPU-local/runtime, synchronization and build/ABI audits
are complete. The initial opt-in CPU-local foundation now passes five
headless hardware runs and all E2 regressions; it is not Exec SMP. The
private SRAM lock backend is now hardware qualified. Opt-in entry-time FPU
isolation passes three complete headless runs with real preempted hart0 tasks,
plus five core04 repeats with hardened diagnostic transport. The normal
core was restored and independently verified before the startup campaign.
Startup core03 now passes three headless metadata-binding reports followed by
Wanderer/touch-heartbeat boot; this binds hart0 only, not the SMP scheduler.
Next come CPU-local runtime and matched ABI integration. Direct task-field atomics and semaphore queues
also require redesign. The private cached-PSRAM software-atomic service now
passes five captures/ten epochs; direct PSRAM AMO/LRSC remains prohibited.
Generic build output is the freshly rebuilt normal core (201,504 bytes, SHA256
1d77ae9251e702b00ac68163bdcf61944f34f5a1207a90afa8fc5a8ad1a645e6),
byte-identical to the saved baseline prefix with every E3 test flag off.
Candidate01 was rejected before flashing; candidate02 boots but its truncated
metadata report does not pass the gate. PSRAM candidate02 was rejected for an
E2 guard failure; corrected PSRAM candidate03 passed its private gate. The
full saved262,144-byte normal baseline range is restored and digest-verified;
invalidate kernel objects before changing opt-in flags. See Track E evidence.

Current D1001 flash state (2026-09-30): the 200-MHz-PSRAM core remains at
`0x20000`. The exact prior 4-MiB GSL3670 firmware development volume was
restored at `0xc00000`; a headless first-byte boot loaded its 4,356 records,
read ready status `0x5a5a5a5a`, polled without errors and started Wanderer.
An earlier dead-touch report was made while the deliberately firmware-free
comparison volume was flashed. A later report on the restored combination
also described a stationary pointer. A synchronized USB-reset repeat on
2026-09-30 restored visible pointer tracking: UART recorded 107 contact frames,
81 mouse events, matching framebuffer updates and clean final release, with
zero I2C errors. The pre-reset failure was not captured and its cause remains
open. Two-finger gestures and the full C4 soak still need their separate gates;
the private firmware remains outside the repository.

The 2026-09-30 tap-mode candidate changes the default mouse policy: one-finger
movement moves only the pointer; a short stationary tap clicks on lift; a
stationary 400-ms hold starts a left drag. Movement beyond 8 logical pixels
cancels tap/hold recognition for that contact. Two fingers retain the latched
right-button gesture. Two empty reports debounce release; I2C errors and
worker shutdown release any held button without synthesizing a click.
The pure policy passes 251 host checks under ASan/UBSan, and the D1001 BSP
contains only the replacement `p4touch.hidd`. A first-byte headless boot
confirms tap mode, firmware readiness and 200 error-free idle polls, with
Wanderer launched. Fabian subsequently confirmed movement, tap and hold-drag
in a synchronized test. The menu bar appears, but the dropdown does not open;
menu selection and full C4 acceptance remain open.
Fabian reports that the pointer stays below the menu strip. A diagnostic-only
`P4_TOUCH_EDGE_TRACE=1` build now records single-contact raw extrema and sampled
raw/logical positions for a consented perimeter measurement. It does not
change or automatically learn calibration; normal builds leave tracing off.
The completed trace contains 263 single-contact frames and zero I2C errors;
its extrema are X=16..1638 and Y=15..874. The next candidate maps those
inclusive D1001 board-profile bounds to 0..1279 and mirrored 799..0, saturating
outliers. It changes both primary-contact selection and event coordinates
consistently. All 3,863 host checks pass, including endpoints, monotonicity,
invalid inputs and the gesture regression. These empirical bounds are not a
controller-wide contract.
The candidate is flashed with writer-side hash verification. A first-byte
headless boot confirms those calibration bounds and 200 error-free idle polls.
After fresh readiness, Fabian confirms four-edge reachability and menu
dropdown opening. The synchronized trace records Button2 release at logical
Y=5, 182 contact frames, 174 events and zero I2C errors, with clean button state
at 1,200 polls. Menu-item execution, full soak and intermittent startup
reliability remain separate open gates; C4 remains hardware partial.
To select legacy immediate-left-press behavior, put `direct` (optionally
followed by LF) in `p4touch.mode` beside the firmware file actually loaded,
then reboot. Missing/invalid files select tap mode. This startup-only preference
uses the already mounted firmware volume instead of racing early ENV assigns.
The current read-only flash fallback requires preparing that preference in the
development volume image; it cannot be saved by writing the running flash disk.

After Fabian reported an empty battery and initially no desktop, opening the
USB console coincided with `CHIP_USB_UART_RESET`. A later controlled USB-reset
capture booted `SDCARD0P0` to Wanderer with clean 200-MHz PSRAM, 256 accepted
graphics swaps and external touch firmware loaded; Fabian then saw the
desktop. The undisturbed post-discharge start was not captured, so neither
automatic cold-boot completion nor a physical touch response is established
by this repeat. See the 2026-09-30 D0 evidence entry in
[ROADMAP.md](ROADMAP.md).

## The board this is being brought up on

Read off the hardware with esptool 5.3, not taken from a datasheet.

| | |
| :--- | :--- |
| Chip | ESP32-P4 revision **v1.3**, dual core plus LP core, 400 MHz |
| Crystal | 40 MHz |
| Console | USB-Serial/JTAG, Espressif 303a:1001, enumerates without a bridge chip |
| Flash | 32 MB, Winbond (manufacturer 0xef, device 0x4019) |
| Flash layout | ESP-IDF table at 0x8000, nvs 0x9000, nvs_key 0xf000, otadata 0x10000, phy_init 0x12000, ota_0 0x20000 (8 MB), `arosbsp` 0x820000 (8064 KB), FAT storage 0x1020000 (15.9 MB) |
| Currently flashed | Patched ESP-IDF v6.0.1 second stage unchanged. Experimental two-hart core for the pending visual test: transitional Giant with default migration and self-test (`P4_GIANT=1 P4_GIANT_MIGRATE=1 P4_GIANT_TEST=1`, otherwise the normal flags), 224,640 bytes at `0x20000`, SHA-256 `318995370c5a6a5b69a2680620458021164a4c9e847e90bb2576f451e6c411f2`, esptool write-verified, headless boot only. The normal single-hart core for restore is 202,032 bytes, SHA-256 `37dba09d4de56e35d267aefc62e8520bd3590b0a298bb21f4773659cea19fd5d`. Double-tap/50-ms-idle candidate 35-member BSP: 3,314,560 bytes at `0x820000`, SHA-256 `eadb8723feb7bf1af92debf48daca033f626ab22bb6308ef3125f3abf93a9714`, unchanged. Firmware volume at `0xc00000` last verified SHA-256 `ed5dca8356362e6065d56922220a2108442dd19db718db7b905833c20818e138`. Backups/logs: build `evidence/giant-2026-10-02/`. Do not pair a v2 core with a v1 BSP. Bootloader, partitions, firmware volume and SD unchanged. No visual acceptance of either core yet. |

Revision v1.3 means the pre-v3 memory layout applies. The hardware facts above
were read before the first write. The bring-up now deliberately replaces only
ota_0 and the former ota_1 rollback slot in the current table; early direct-ROM
experiments also replaced the original second stage and board-data sectors.
The FAT `storage` partition has not been touched.

## Silicon revisions

Supporting both revision families costs less than the 190 differing register
headers in ESP-IDF suggest, because the peripheral base addresses are not
among the differences: `register/hw_ver1/soc/reg_base.h` and its hw_ver3
counterpart differ by exactly one added define, `DR_REG_LP_TRNG_BASE`. What
differs is inside peripherals, and only for the ones a driver touches.

What does differ structurally is where the L2 cache is carved out of the
768 KB of internal SRAM. From `components/esp_system/ld/esp32p4/memory.ld.in`:

| | before v3 | v3 and later |
| :--- | :--- | :--- |
| L2 cache | top of SRAM | bottom of SRAM, from 0x4FF00000 |
| Code and data | 0x4FF00000 to 0x4FF2CBD0, 179 KB | 0x4FF00000 + L2 size to 0x4FFAEFC0 |
| Data only | 0x4FF40000, 512 KB minus L2 size | - |
| Second stage and ROM working set | 0x4FF2CBD0 to 0x4FF40000, 77 KB | none |

The L2 cache is 128, 256 or 512 KB, chosen at startup. On this board with
256 KB, which is what the resident firmware selects, that leaves 179 KB of
executable SRAM plus 256 KB of data SRAM; choosing 128 KB instead would give
384 KB of data SRAM, and for a system that is not yet scanning a framebuffer
out of PSRAM that is the better trade.

That last row is not memory the ROM keeps from us, which is what it looked
like from the app linker script alone. Reading the second stage bootloader out
of the flash backup settles it: it loads into three segments spanning
0x4FF29ED0 to 0x4FF354C4, and the ROM's own variables sit at the top of the
window from about 0x4FF3FFC8 up (`rom_spiflash_legacy_data`, `ets_ops_table_ptr`,
`uart_acm_dev` and the rest, in `components/esp_rom/esp32p4/ld/esp32p4.rom.ld`).
The gap between the two is where the ROM loader's stack must live, which is
also why a working second stage stops at 0x4FF354C4 instead of filling
upwards. An image the ROM loads itself has all of it free, because then there
is no second stage.

The RAM inventory can therefore be decided at runtime, and cheaply: exec
takes any number of disjoint memory regions, so the revision is read and the
matching region table is added. The link address cannot be. The overlap of
the executable windows across both families is 0x4FF20000 to 0x4FF2CBD0,
about 51 KB, which will not hold kernel.resource, exec.library and
task.resource together.

So this port builds one image per revision family, which is also how ESP-IDF
treats the question: the image header carries min_chip_rev_full and
max_chip_rev_full, and the firmware on this board declares v1.00 to v1.99.
The v1 numbers are the ones being used, since that is the silicon in hand;
the v3 numbers belong in the same header next to them, marked as untested
until someone runs them.

## Hardware facts this port is built on

Taken from ESP-IDF v6.0 (`components/soc/esp32p4`) unless noted. Anything
marked *unverified* still needs a look at the ESP32-P4 TRM.

| Property | Value | Source |
| :--- | :--- | :--- |
| ISA | RV32IMAFC, `zicsr zifencei zaamo zalrsc` | `components/soc/project_include.cmake` |
| ABI | `ilp32f`, single precision floats only, FLEN=32 | same |
| ISA string accepted | yes | `riscv32-esp-elf-gcc 15.2.0` compiles with it and reports `__riscv_flen 32`, `__riscv_atomic 1`, `__riscv_float_abi_single 1` |
| Privilege modes | M-mode; no S-mode, no Sv32 MMU | *unverified* |
| Interrupt controller | CLIC, not PLIC | `soc_caps.h: SOC_INT_CLIC_SUPPORTED` |
| HP cores | 2 | `soc_caps.h: SOC_CPU_CORES_NUM` |
| FPU | present, `EXT_ILL` CSR errata on FLW/FSW | `soc_caps.h: SOC_CPU_HAS_FPU*` |
| Internal SRAM | `0x4FF00000`-`0x4FFC0000`, 768 KB | `soc.h: SOC_DRAM_LOW/HIGH` |
| PSRAM window | `0x48000000`-`0x4C000000`, 64 MB window | `soc.h: SOC_EXTRAM_LOW/HIGH` |
| LP SRAM | `0x50108000`-`0x50110000`, 32 KB | `soc.h: SOC_RTC_DRAM_LOW/HIGH` |
| Register layout revisions | `hw_ver1` and `hw_ver3` differ | two `register/hw_ver*/soc/reg_base.h` trees |

Two consequences worth stating early. First, the D extension is absent, so
the `ilp32d` default in `configure.in` for 32-bit RISC-V does not hold here
and `struct FpuContext` in `arch/riscv-all/include/aros/cpucontext.h`, which
declares `UQUAD f[32]` for FLEN=64, is wrong for this platform. Second,
768 KB of internal SRAM is not enough to reach a Workbench, so PSRAM
bring-up gates every milestone above the exec layer.

## Relationship to the existing RISC-V code

- `arch/riscv-all` is the shared 32-bit CPU layer and is used as is where
  possible. It currently lacks the S-mode/M-mode-independent pieces that
  `arch/riscv64-all` has (`cli`, `sti`, `issuper`, `backtracefromframe`,
  `cachecleare`, `cacheclearu`, `processor/`), and its `asm/cpu.h` defines
  only the `sstatus`/`sie` names. M-mode equivalents have to be added.
- `arch/riscv64-opensbi` is the reference implementation to port down from.
  It is the only complete and recently maintained RISC-V platform in the
  tree (traps, CLIC's PLIC counterpart, Sv39 MMU, SBI timer, ELF loader,
  lazy FPU switching, per-hart data).
- `arch/riscv64-opensbi` takes a lot of what it knows from the flattened
  device tree the firmware hands it: `kernel_fdt.c` parses it, and
  `arch/riscv64-all/processor/processor_init.c` derives the whole hart
  description from `/cpus`. The ESP32-P4 boots with no device tree at all.
  Supplying a small built-in DTB describing the harts, RAM, UART0, the CLIC
  and the timer would let those paths be reused rather than reimplemented,
  and `arch/riscv-all/linklibs/fdt` is already there to read it. Worth
  deciding before the kernel starts growing platform-specific variants of
  code that already exists.
- `arch/riscv-native` and its `sifive_u` board are 2023 scaffolding that was
  never built: no configure target exists for them, `kernel/intr.c` ends in a
  truncated inline-assembly handler with the call commented out, and
  `cpu_Init()` has an empty body. This port does not build on that code.

## The reTerminal D1001

There is a complete, working board definition to refer to rather than guess
from: the owner's `vellum-d1001` firmware, at `/Volumes/Dev/Source/Vellum/firmware`,
runs on this exact board. `components-lcd/d1001_board/include/d1001_board.h`
carries the pin map and `components-lcd/lcd_jd9365/` the panel driver.

Worth reading before writing any driver here, because several things the
earlier attempt assumed are wrong:

| | earlier attempt | the working firmware |
| :--- | :--- | :--- |
| Panel | "8 inch 1280x800 MIPI-DSI" | JD9365, natively 800x1280 portrait, 2 lanes at 1000 Mbps, DPI clock 40 MHz.  The porches are not listed here on purpose: two sources give different sets and only one of them ever drove this panel.  See [display/DISPLAY-CONTRACT.md](display/DISPLAY-CONTRACT.md) |
| DSI PHY | not addressed | needs the internal LDO on channel 3 at 2500 mV |
| Backlight | GPIO 26 | GPIO 14, with LCD_PWR_EN and LCD_BL_EN on an I2C port expander |
| Panel reset | not addressed | port expander bit, not a GPIO |
| Button | GPIO 35 | GPIO 3 |
| Port expander | not addressed | PCA9535, holding power, panel power, panel reset, backlight enable, battery read and charge enable, amplifier enable |
| ESP32-C6 radio | UART at 460800 with a hand-written EZSP/Spinel stack | SDIO (CLK 11, CMD 6, D0-D3 on 7-10), reset on GPIO 13, ESP-Hosted protocol |
| I2C buses | one assumed | I2C0 on SCL 38 / SDA 37, I2C1 on SCL 21 / SDA 20 |
| Console | UART0 | native USB-Serial/JTAG |

Other pins from the same header: battery ADC 18, charge state 15, VSYS power
good 4, USB insertion 17, LEDs on 22 (red), 36 (green) and 23 (blue), touch
interrupt 16.

## Where the image goes

One image, two places it can be written, and no rebuild between them.

    0x2000    the second stage bootloader offset, where the first stage ROM
              loader picks it up directly
    0x20000   the ota_0 app partition, loaded by whichever second stage
              bootloader the device already carries

The first is the one this port is built around. An app partition brings three
mechanisms with it that all assume an ESP-IDF application is sitting there:
the partition table, the OTA data with its state field, and anti-rollback. An
image that is not such an application cannot hold up its end - it has no
`esp_ota_mark_app_valid_cancel_rollback()` to call - so the bootloader does
the right thing for the firmware it was built with and reverts. Observed
exactly that on this board: written to ota_0, the next boot rolled back to the
other slot, and the firmware there repaired ota_0 over the network.

Everything is linked into one run of SRAM from 0x4FF00000, nothing into the
high window at 0x4FF40000, for two reasons. The high window only exists as
RAM while the L2 cache is smaller than 512 KB, and on this silicon the cache
size is set by software that has not run yet when the ROM loader starts an
image - so linking a boot stack there is a bet on a setting we do not yet
control. And the high window and PSRAM are better given to exec as heap at
runtime than spent on link-time sections.

There is a second ceiling, and it is on the image rather than on the
addresses: the first stage ROM loader stops loading a second stage somewhere
between 120544 bytes, which it takes, and 142064 bytes, which it silently
refuses - the flash is written and verified and not one byte comes out. 128 KB
sits in that window and matches how the loader works, mapping flash in 64 KB
pages, but the exact number has not been pinned down. Adding debug.library to
the kickstart was enough to cross it.

The room came from the unwind tables. .eh_frame was 25164 bytes of a 120544
byte image, a fifth of it, and nothing reads it: the backtrace walks the frame
pointer chain (UnwindFrame in arch/riscv-all/exec/alert_cpu.c) and there are no
C++ exceptions in a kickstart. Discarding it in the link script leaves 95376
bytes, and debug.library then fits at 114576 with about 14 KB still to spare.

Which is worth stating as a measurement rather than a rule of thumb, because it
inverts the obvious conclusion. debug.library looked like the thing that did
not fit, and it was not; the tables nobody asked for were twice its weight.

The link script's ceiling is the lower of the two bounds, 0x4FF29ED0, which
costs 12032 bytes of headroom and is what makes one image valid on both
paths. Overflowing it is a linker error naming the script, not an image that
loads over whatever put it there; that assertion has been made to fire on
purpose to check it works.

## Booting from an app partition

The ROM loads a second stage from 0x2000 and that is what this port did
first, but the ROM stops taking an image somewhere between 120544 and
142064 bytes, and that limit decided three separate design questions
before it was worth removing. So there is a second path, and it is now the
one the board runs:

    0x2000     ESP-IDF second stage bootloader (22544 bytes)
    0x8000     partition table
    0x10000    otadata, pointing at slot 0
    0x20000    ota_0, this image
    0x820000   arosbsp, custom type 0x40

The table started as the board's own. Pointing otadata at slot 0 before
repurposing the second slot is one command:

    otatool.py --port <port> --partition-table-file <table> \
               --partition-table-offset 0x8000 switch_ota_partition --slot 0

M6 replaces the former second OTA/rollback slot in that table. The source is
`bootloader/partition-table.csv`; `esp32p4-partition-table` runs ESP-IDF's
own `gen_esp32part.py` in both directions and leaves these checked artifacts
under `bin/esp32p4-riscv/gen/rom/boot`:

    esp32p4-partition-table.bin
    esp32p4-partition-table.decoded.csv

The changed entry has custom type 0x40, subtype 0, label `arosbsp`, offset
0x820000 and size 0x7e0000. It therefore ends at the 16 MB cache-mapping
limit, while the FAT `storage` partition remains at 0x1020000. Building the
table never touches the board. Its separate flash target requires both an
explicit character-device path and an acknowledgement that `ota_1` is being
replaced:

    gmake esp32p4-flash-partition-table \
        ESPTOOL="uvx esptool" \
        ESP32P4_PORT=/dev/cu.usbmodem101 \
        ESP32P4_PARTITION_CONFIRM=replace-ota_1-with-arosbsp

That target validates the binary again immediately before writing only the
partition-table sector at 0x8000. It leaves the chip in download mode rather
than booting an unobserved new layout. It is not a dependency of the
bootloader, kernel image or any default build.

The package itself uses AROS's existing PKG container rather than a new disk
format. The current package contains fourteen members: `sdcard.device`,
`utility.library`, `partition.library`, `expansion.library`, the A2 corpus
fixture `ramtest.device`, and, added for A4, `dos.library`,
`bootloader.resource`, `FileSystem.resource`, `lddemon.resource`,
`shell.resource`, `shellcommands.resource`, `dosboot.resource`, the `fat`
handler and the `econsole` handler. Together that is 1,163,992 bytes, 14.1 %
of the 0x7e0000-byte partition.

    gmake kernel-package-esp32p4-riscv FAT_DEBUG=1 DOS_DEBUG=1 DOSBOOT_DEBUG=1

The three debug flags are what make a boot readable. `dosboot.resource`'s
COLDSTART init never returns, so nothing printed after `krnStartExec()` in
`kernel_startup.c` is reachable once it is present, and the modules' own
narration is the only account of what the boot did. Note that mmake does not
invalidate objects when a mmakefile changes, so switching one of these flags
on needs the affected objects deleted by hand.

`boot/audit-package.py` checks the built container. It reads the set of
accepted relocation types out of `kernel/kernel_elf.c` itself, so it cannot
drift away from the loader, and it fails if any member is not a little endian
ELF32 relocatable RISC-V object or carries a relocation the loader does not
implement.

It produces `AROS/boot/esp32p4/aros-bsp.pkg`. At boot the kernel validates the
container and its ELF32/RISC-V members, copies the exact declared package size
through a temporary flash-MMU mapping, relocates the modules into PSRAM, syncs
the generated code and gives the resident scanner a second address range. The
package and loaded sections remain below the PSRAM MemHeader so Exec cannot
allocate over them.

The initial hardware run on the revision 1.3 D1001 loaded one 40,988-byte
package, placed `utility.library` at `0x4800a020`, found its RomTag at
`0x4800bc14`, reserved 48,900 bytes and exposed the remaining external memory
from `0x4800bf40`. Both InitCode passes completed and
`OpenLibrary("utility.library", 0)` returned version 50. The 100 Hz timer then
continued for the full capture, so this is an end-to-end hardware result, not
only a loader or build test.

The expanded read-only SD diagnostic package initially had three members and
was 186,416 bytes. Its no-card hardware boot reserved 230,764 bytes of PSRAM,
found both residents in `sdcard.device` as well as the utility and expansion
residents, failed opening unit 0 cleanly, and kept the 100 Hz heartbeat alive.
The current four-member package is 290,904 bytes (SHA-256
`cc18bc81c701b376294c4a39e024048adcee58a6e50595b831439dcce23885cc`). A
hardware boot loaded all four members, including `partition.library` at
`0x480504b0`, and reserved 361,308 bytes of PSRAM.

The first-stage backend accepts exactly one 512-byte sector per request; it
rejects CMD18 and larger requests before touching the controller until
multi-block error recovery is proven on hardware. Card identification and the
public-device reads are verified with a SanDisk `SN128` SDXC card (CID and CSD
CRC7 both valid, manufactured September 2021): the decoded capacity is exactly
249,737,216 sectors or 127,865,454,592 bytes. Ten single-sector reads covered
LBAs 0, 1, 2, 2048, 8192, 32768 and 65536, the final 33rd and final sectors
through `NSCMD_TD_READ64`, and a repeat of LBA 0. Every request replaced its
nonzero sentinel buffer, LBA 2048 contained real nonzero data, both LBA-0
hashes matched, and the timer/IRQ heartbeat continued after the test.

That last sentence needs a correction, added on 2026-08-22 after the card was
read in a host reader. The two LBA-0 hashes match each other, and both are
wrong. `0x4d7705c5` is the FNV-1a-32 of 512 zero bytes, while LBA 0 on this
card holds a valid MBR whose hash is `0xdebe99c1`. What the sentinel test
proved is coverage, that the buffer was overwritten, not that it was
overwritten with the right sector. The card's own hashes are recorded in
[sdcard/test-card-reference.md](sdcard/test-card-reference.md).

The next hardware run opened `partition.library` version 3 and then
`OpenRootPartition("sdcard.device", 0)`. The library selected
`NSCMD_TD_READ64` (`0x0000c000`) from the device's public command list. A
single `ReadPartitionDataQ()` of LBA 0 returned 0 with `io_Actual == 512`,
replaced the sentinel buffer and produced the same `0x4d7705c5` hash as the
direct device test. No partition-table scan or media write was issued, and
the timer/IRQ heartbeat remained stable for the full capture.

Two things had to be built for this, and neither is optional.

**The image needs an application descriptor.** An IDF bootloader reads the
first bytes of segment 0 as an esp_app_desc_t and checks the efuse block
revision range it finds there, without looking at the magic word first. An
image with no descriptor is therefore not treated as "no constraint" - the
bootloader reads whatever code is there and rejects the image:

    E boot_comm: Image requires efuse blk rev >= v509.47, but chip is v0.3

appdesc.c supplies one, and the link script puts it first. It costs the
ROM path nothing, because that loader takes the entry address from the
image header rather than assuming the segment begins with code.

**The bootloader needs four patches**, in
bootloader/esp-idf-6.0.1-standalone-app.diff. No ESP-IDF source is kept in
this tree: `make esp32p4-bootloader` fetches it at the pinned version, the
patch is applied to the fetched copy, and the bootloader is built there.
That target is not part of any default build, because it wants a host
toolchain AROS does not otherwise ask for - cmake, ninja, riscv32-esp-elf
and a python carrying ESP-IDF's requirements.

What it fetches is the release asset, `esp-idf-v6.0.1.zip`, and that is
1.7 GB. The git tag archive is a tenth of the size and was tried first, but
it carries no submodules, and IDF's cmake configures every component in the
tree whether the build needs it or not: mbedtls alone registers 36 include
directories that then do not exist, and satisfying them with placeholders
answers only until the next one. The release zip is what Espressif
publishes for people without git, and it is complete. It lands in the usual
`bin/Sources` cache, so it is fetched once.

The bootloader it produces identifies itself as `v6.0.1-dirty`, which is
the patch showing up in the version string. That is accurate and worth
leaving alone. The licence the fetched
sources carry, and what it asks of anyone redistributing the result, is in
the LEGAL file at the top of the tree; the patch doubles as the statement
of changes that licence requires, which is why it explains each hunk. All four exist for the same
reason: an IDF application always has its .text and .rodata mapped from
flash, and this image has neither - one segment, all of it in SRAM. The
loader assumes those segments exist in four places, and each assumption
only becomes visible once the previous one is out of the way. Whoever
tries this against another IDF version should expect the list to differ
rather than assume it is complete.

The bootloader also has to be built for pre-v3 silicon, and IDF treats
pre-v3 and v3+ as mutually exclusive: CONFIG_ESP32P4_SELECTS_REV_LESS_V3
opens the gate before CONFIG_ESP32P4_REV_MIN_100 can be chosen at all.
Without it the bootloader declares a minimum of v3.1 and esptool refuses
to flash it, which is the right answer to the wrong build.

### Code from flash

The four patches exist only because this image is shaped unlike an
application: one segment, all of it in SRAM. Shaped the other way it needs
none of them, so there is a second link script, ldscript-xip.lds, that
leaves .text and .rodata in flash for the bootloader to map:

    segment 0: paddr=00020020 vaddr=40000020 size=1cbd4h (117716) map
    segment 1: paddr=0003cbfc vaddr=4ff00000 size=00018h (    24) load

One mapped segment and a 24-byte load segment, which is the whole of
.data. It runs: exec comes up, the residents are found at their flash
addresses, and the 100 Hz tick keeps time from mapped code.

    internal SRAM available   519936 -> 637680 bytes
    AvailMem(MEMF_ANY)        426352 -> 538256 bytes

117744 bytes returned, which is exactly what moved out. The number is not
the point though: on this path the size of AROS's code stops being bounded
by internal SRAM at all, and a megabyte of Intuition and graphics would
not have to fit in 523 KB because it would not be there.

Almost nothing had to change for it. A romtag at 0x400053bc works like one
at 0x4ff053bc, and the module structures, the resident scan and the LVO
tables never notice. Two things did:

  - The address must be 0x40000020 exactly. The flash window is
    0x40000000-0x44000000 and instruction and data mappings share it; the
    mapped segment has to be first in the image, and its virtual address
    must sit at the same offset inside a 64 KB MMU page as its physical
    address. ota_0 is page aligned and the headers take 0x20 bytes. Give
    the address to the output section explicitly, or an input section with
    a stronger alignment pushes it to 0x40000040, esptool pads the file
    with the SRAM segment to fix the alignment, that segment becomes
    segment 0, and the bootloader looks for the application descriptor
    inside it.

  - "Where the modules are" and "which RAM the image occupies" stop being
    the same span. They were the same symbol pair for as long as the image
    was contiguous, so the romtag scan ran from __text_start to
    __kernel_end - which here is 200 MB of mostly unmapped address space.
    It found every module and then faulted in the emptiness beyond, with
    no trap output, because the trap handler is in flash too. Both scripts
    now define __romtags_start/_end and __kernel_lowest/_highest
    themselves.

What this path cannot do is boot from 0x2000: the first stage ROM loader
is not asked to set up these mappings. So both scripts stay, ldscript.lds
is the default, and P4_LDSCRIPT=ldscript-xip.lds selects the other.

And one thing it will need before M5: code that runs with the cache
disabled has to live in SRAM. Nothing does today, which is why this works
at all, but the MSPI bring-up will, and that is what IDF's IRAM_ATTR
exists for.

## Milestones

Each milestone names what has to be true before it counts as done. No
milestone is marked done on the strength of the code reading correctly.

**M0 - build chain.** Done. `configure --target=esp32p4-riscv` succeeds and
`gmake crosstools` produces binutils 2.47 and gcc 16.2.0 for
`rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f`, plus the AROS link
libraries. Four things had to be fixed on the way, none of them in this
platform: two host-side (see above) and two in the shared rv32 layer, where
`fenv.h` rejected the single precision ABI outright and the library stub
frames in `genmodule.h` were hardcoded to ILP32D.

**M1 - CPU layer.** Done. `gmake kernel-kernel-esp32p4-riscv` and
`gmake kernel-exec-esp32p4-riscv` both build. Note they cannot share one
gmake invocation under `-j`: genmf regenerates the same mmakefile from both
and the loser of the race finds the temporary file already renamed.

**M2 - first output.** M-mode startup, `mtvec` trap entry, UART0 debug
console, kernel.resource and exec.library reaching `KrnBug()`.
Done when: the board prints an AROS banner over USB-serial.

**M3 - flash image.** Own image at the ROM bootloader offset, or a
documented ESP-IDF bootloader dependency with its licence resolved.
Done when: the image flashes with esptool and reaches M2's banner from a
cold boot.

**M4 - multitasking.** Done. Two tasks at equal priority share the CPU
within a few hundred counts of each other over eleven heartbeats, and a
500 ms timerequest on timer.device's VBLANK unit returns after exactly 50
ticks, twenty-four times running.

`Delay()` itself is dos.library's, which this kickstart has not got; the
timerequest is what Delay() does internally, and it is measured in ticks
because the tick is the thing being trusted. Both tests are behind
`-DP4_TASK_TEST` and off by default.

Two things had to be fixed to get here, neither of them in this platform.
A prepared context named SPP where mret reads MPP, so every task returned
to User mode and faulted on its first instruction fetch. And
task.resource's expunge path restored exec's AddTask and RemTask vectors
it had never taken, nulling them whenever its init failed - which in a
kickstart without utility.library is every boot.

The measurement that first came back said 56 ticks rather than 50, every
time. That was not the timer: the waiting task sat at the same priority
as a CPU-bound one, so it became ready on time and ran a quantum later.
Raising it above the spinner gives exactly 50, which is worth recording
because a systematic six-tick offset is the kind of thing that gets
explained away as timer inaccuracy.

**M5 - memory.** Done. 32 MB of AP hex PSRAM at 20 MHz, mapped at
0x48000000 and in exec's memory list; `AvailMem(MEMF_ANY)` reports
34,177,392 bytes where it reported 634,128, and a 30 MB allocation survives
a write-read cycle across its whole length.

One route was tried and abandoned, and the reasons are worth keeping so
nobody spends the same afternoon on it. Bringing the chip up from the
ESP-IDF bootloader, where ESP-IDF's own code and headers are, looks
obvious and is not: `esp_psram` is written for the application
environment, and moving it into a bootloader means rebuilding that
environment there, one obstacle at a time.

  1. `CONFIG_SPIRAM=y` makes `esp_hw_support` include a PSRAM header whose
     include path is only added outside a non-OS build. That one is a real
     inconsistency in ESP-IDF and a one-line fix.
  2. `esp_psram/system_layer/esp_psram.c` needs FreeRTOS, and pulls in
     `esp_mm`, which needs it too.
  3. `device/esp_psram_impl_ap_hex.c`, the chip level file, needs no
     FreeRTOS at all - so taking that file alone into a component of our
     own gets past 2.
  4. Its configuration symbols come from `esp_psram`'s Kconfig, which is
     only read when that component is in the build. Statable by hand.
  5. A component whose symbols nobody references is compiled and then
     dropped at link time. The hook is a weak symbol, the build reports
     success, the image does not change size, and the bootloader skips
     the hook. `WHOLE_ARCHIVE` fixes it.
  6. And then the link wants five functions a bootloader build does not
     compile: `mspi_timing_psram_tuning`, `periph_rcc_enter`,
     `periph_rcc_exit`, `periph_rtc_mpll_acquire`,
     `periph_rtc_mpll_freq_set`.

Six is where it stopped, and the port writes its own instead. What that
turned out to be, in the order it has to happen:

    MPLL up at 400 MHz              over the internal configuration bus
    module clocks on, controllers reset
    clock source MPLL, divider 20   the divider is the whole speed question
    pin drive strength 2 on all twenty pins
    DQS enabled, CS setup/hold/delay 4/4/3
    split transactions, page size 2048
    DLL on, both controllers
    mode registers MR0/MR4/MR8      latency, burst length, sixteen bit bus
    identify: vendor 0x0d, density 7
    a word written and read back
    the AXI path: read and write commands, address and dummy lengths,
      octal command and address with hex data, double rate, AXI enable
    the translation table: physical page n to virtual page n
    a memory header, without MEMF_FAST, at priority -20

Two controllers share the bus, MSPI2 for the memory-mapped path and MSPI3
for the mode registers, which is why some of it is done twice.

Four things about this were wrong in the first attempt, and all four are
the kind of mistake that produces silence rather than an error:

  - **The clock source.** Taking the bus clock off XTAL to avoid bringing
    the MPLL up rests on the idea that 20 MHz needs no PLL. ESP-IDF's own
    20 MHz configuration runs the MPLL at 400 MHz and divides by twenty;
    the divider sits behind the PLL. What a low bus clock saves is the read
    timing calibration, and nothing else. This is a correction to what an
    earlier version of this file claimed.
  - **An address.** The clock source select field is in PERI_CLK_CTRL00,
    not CTRL01. It went unnoticed because XTAL is encoded as zero and zero
    is the reset value, so the bus ran off XTAL either way.
  - **Pin drive strength.** Reset selects zero, the weakest of four, and at
    that setting the chip does not answer at all. Leaving it alone was
    justified as a signal integrity question with margin to spare at 20
    MHz, which confuses two things: integrity is about the shape of an edge
    that arrives, this is about whether the driver moves the line far
    enough to be seen.
  - **The AXI path.** A correct translation table is not enough. Until
    MSPI2 is told what a read and a write look like and allowed to answer
    AXI requests, a store to the mapped window is a bus error.

The method is worth more than any of the four. An ESP-IDF application with
PSRAM enabled, built from the same fetched sources and flashed to the same
app partition, establishes that the hardware works and gives a reference
to compare against. Then, in order: 688 registers across nine peripheral
blocks diffed between the two; the MPLL's configuration read back off the
internal bus, which is the only way to check a write there; and finally
this port's own transaction code compiled into a working ESP-IDF session,
which is what settled it. That last step also caught a mistake made while
looking: the ROM's `cs_en_mask` is `1 << 1`, its header's wording ("0 for
cs0, 1 for cs1") invites the other reading, and following it turns working
reads into a floating bus.

Two smaller things fall out of it. The bring-up runs with interrupts
disabled, because a sequence that can be interrupted between a controller
write and the transaction that depends on it is not a sequence. And the
mode registers are written as absolute values rather than
read-modify-write: before the chip is configured a read returns a floating
bus, and writing 0xff back sets every reserved bit along with a
partial-array-refresh setting nobody asked for. ESP-IDF read-modify-writes
because it can trust the read.

**The clock, which is where this is not finished.** 20 MHz against the
200 MHz the part is rated for is a tenth of the bandwidth and ten times
the latency. What stands between them is the per-board read timing
calibration - `mspi_timing_psram_tuning` and its delay-line and phase
sweep - plus the read and write latencies in MR0 and MR4 and the two dummy
lengths in the AXI path, which are the values that depend on the clock.
The MPLL is already at 400 MHz, so nothing about the PLL has to change:
200 MHz is a divider of two, three latency constants and the calibration.
Note that 80 MHz does not escape the calibration either; ESP-IDF's tables
require it at 80, 200 and 250 MHz, and only 20 MHz is exempt.

Where 20 MHz will not do is the display. The native 800x1280 RGB565 frame is
2,048,000 bytes. A 40 MHz RGB565 pixel stream can consume 80 MB/s, exactly the
theoretical x16-DTR limit of PSRAM at this clock and with no margin for CPU or
cache traffic; a full active-area redraw at a real 60 Hz would be 122.88 MB/s.
The existing timing sources also disagree and neither produces 60 Hz at a
40 MHz pixel clock. The timing and measured refresh therefore have their own
gate in the roadmap rather than being assumed here. As a heap for 32 MB that
would otherwise not exist, 20 MHz remains useful.

**M6 - storage and package loading.** In progress. The flash half is done:
the custom BSP partition, PKG build, hardened ELF32 loader, PSRAM relocation
and second resident scan load `sdcard.device`, `utility.library`,
`partition.library` and `expansion.library` on hardware. A native polling
DesignWare-MMC adapter now registers the D1001 slot through the generic
`sdcard.device`; it is intentionally read-only. Its no-card path, SDXC
identification, geometry/NSD APIs, low-LBA CMD17 reads, high-LBA TD64 reads
and one 512-byte read through `partition.library`'s public root API are
verified on hardware. Generic CMD12/CMD13 cleanup is proven to return the
card to TRAN. The long-standing CMD18 progression fault is resolved. Reading
the test card in a host reader on 2026-08-22 showed that every returned sector
consisted of 128 copies of its own first 32-bit word, because a CPU read of
the data FIFO does not pop on this controller and because
`CTRL.use_internal_dma` was defined on bit 26 instead of 25. With the bit
corrected and every read routed through the IDMAC, as ESP-IDF does, all 24
cells of the card-referenced matrix match. Historical note on the earlier
attempts: a four-by-4-KiB receive-only
IDMAC ring matching the local ESP-IDF transaction shape also times out at 32
and 128 blocks before `DATA_OVER`. A complete ESP-IDF v5.4.2 descriptor/event
state-model trial likewise leaves a full receive FIFO with RXDR pending after
16 KiB, so it was rejected and the manual CMD12 baseline restored. Buffered
P4 controller telemetry cannot distinguish the remaining card/CIU fault. A1
is complete: 59 card-referenced cells over 1, 2, 32 and 128 sectors at seven
addresses, 1,000 bounded repetitions, three injected fault modes each
recovering through CMD12/CMD13, the invalid-request rejection cases and a live
100 Hz heartbeat. The physical CMD and DAT0/D0 wire trace was never needed: it
had been chosen to explain a selectivity that the card's own content showed
does not exist.
A4 has begun. The package now also carries `dos.library`, the four resources
around it, the shell pair and the `fat` handler; twelve members load and
relocate into PSRAM, the resident order on the board matches what the `.conf`
files declare, and `FileSystem.resource` carries the three FAT DosTypes
including the 0x46415402 the A3 image's partition reports. Three defects had
to be fixed to get there, all outside this directory: `arch/riscv-all/crt`
saved the callee-saved floating point registers with D-extension opcodes this
target does not have, the `rom/dos/catalogs` submodule was not initialised so
`dos.library` could not compile, and this platform never assigned kernel.
resource's `BootMsg`, so `KrnGetBootInfo()` returned nothing and no boot
argument could reach `bootloader.resource`. With that last one fixed, a
`P4_CMDLINE`/`P4_HEADLESS_BOOT` build option supplies the
`econsole nomonitors nocomposition` command line the headless route needs.
A4 is done. The board boots from the MicroSD card: dosboot replaces the
whole-disk node with the partition node, `dos.library` starts the FAT handler,
FAT mounts the volume and reports it write protected, `AROS.boot` is accepted
on its `cpu riscv` line, `SYS:` is assigned from that volume, and the Shell
reaches a prompt on the emergency console that can be typed into. `Info()`
reports `ID_WRITE_PROTECTED` and eight representative DOS mutations are
refused with `ERROR_DISK_WRITE_PROTECTED`, with the file bit-identical
afterwards. A medium without `AROS.boot` unmounts cleanly and falls back to
the same prompt.

Five defects in shared code had to be fixed to get there, and none of them
was specific to this port beyond being first to hit it. `CacheClearE()` on
32-bit RISC-V clears nothing and each platform is expected to replace it;
this one had not, so `CreateSegList()`'s trampoline was jumped to before the
instruction side had re-fetched it. `shellcommands_init.c` guarded the same
flush for resident commands behind `__AROS_USE_FULLJMP`, which means
something else - that the library jump table holds instructions - so every
resident shell command was unreachable here. `sdcard.device` never
initialised its disc-change interrupt list, which only a filesystem
exercises. The SD backend's 512-byte bounce buffer could not take FAT's
32-sector reads into an `AllocMem` buffer that is only 32-byte aligned. And
`KrnMayGetChar()` had no implementation, which made the console output-only
and, because econsole polls it at DOS's handler priority 10, starved
everything below that priority.

A5 closes it. A command and a library, neither in the kickstart nor in the
flash package, are loaded off the card by `dos.library`'s own ELF loader and
run: the command from a kickstart resident and again typed at the Shell
prompt, the library through lddemon. Every code and rodata address lies
outside the kickstart and the package, judged by the kickstart itself since
only it knows where those are, and the library answers a call that mixes a
value the caller passes in with one its own initialisation wrote, which
neither side can fake. Four refusal cases - missing, truncated, wrong machine
and an unimplemented relocation - each fail with the loader naming its own
reason and freeing everything it had allocated.
Done when: modules outside the kickstart also start from MicroSD. Done.
The complete order, hardening work and test matrix are in
[ROADMAP.md, Track A](ROADMAP.md#track-a-storage-and-normal-boot).

**M7 - display and input.** MIPI-DSI framebuffer HIDD, touch HIDD, Intuition.
Done when: a Workbench screen appears and the pointer follows a touch.
The graphical boot and touch are separate gates in
[ROADMAP.md, Tracks B and C](ROADMAP.md#track-b-panel-hardware-and-scanout).

## First boot

Written to 0x2000 and started by the first stage ROM loader:

    ESP-ROM:esp32p4-eco2-20240710
    rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)
    SPI mode:DIO, clock div:1
    load:0x4ff00000,len:0x1b248
    entry 0x4ff00000

    AROS/esp32p4-riscv
    [kernel] hart   0
    [kernel] misa   0x40901125  rv32acfimux
    [kernel] vendor 0x00000612  arch 0x80000003  impl 0x00000001
    [kernel] text   0x4ff00000 - 0x4ff10044  65604 bytes
    [kernel] rodata 0x4ff10044 - 0x4ff1b230  45548 bytes
    [kernel] data   0x4ff1b234 - 0x4ff1b248  20 bytes
    [kernel] bss    0x4ff1b250 - 0x4ff1f680  17456 bytes
    [kernel] sram   0x4ff00000 - 0x4ffc0000
    [kernel] psram  0x48000000 - 0x4c000000  32 MB, mapped and verified
    [kernel] no memory list, no KernelBase, no exec yet - stopping here.

Three things that were assumptions before and are facts now.

`misa` reads 0x40901125: A, C, F, I, M, U and X, with MXL saying 32-bit. **No
D**, so the single precision ABI is confirmed by the silicon rather than
inferred from ESP-IDF's build files, and the two rv32 fixes it needed were a
precondition rather than a precaution. **No S, but U is there**: there is no
supervisor mode, as assumed, but user mode exists and will be available when
tasks want separating.

The ROM loader accepts an image at the bootloader offset and loads it to
0x4FF00000, so the reasoning about the low window being free while it runs
holds.

And `rst:0x7 (HP_SYS_HP_WDT_RESET)`: the ROM leaves a watchdog armed, so a
kernel that stops in wfi is restarted about once a second. Convenient for now,
because a single banner cannot reliably be caught over USB-Serial/JTAG at all
- the peripheral only takes data once a host has attached and is reading, and
krnP4PutC drops bytes when it has not - but it has to be fed or disabled in
platform_init before anything runs for longer than that.

## Verification

There is no emulator. Espressif's QEMU build in `~/.espressif/tools/qemu-riscv32`
supports the machine `esp32c3` only, and upstream QEMU has no ESP32-P4
model. Every milestone from M2 on is verified on a Seeed Studio reTerminal
D1001 over its USB-C serial console.

## Building

    ./configure --target=esp32p4-riscv --enable-ccache
    gmake crosstools

Out of tree, from a build directory of its own. Three things about this host
are worth writing down, because none of them is guessable:

  - **`gmake`, not `make`.** The system make is GNU Make 3.81, which is old
    enough to matter; Homebrew's is 4.4.1.
  - **`PYTHONPATH` for the mako check.** `configure.in:677` requires the
    Python mako module, though nothing in the tree imports it. It does not
    have to be installed: the copy the Google Cloud SDK vendors satisfies
    the check via
    `PYTHONPATH=/opt/homebrew/share/google-cloud-sdk/lib/third_party`.
    Note that Homebrew's `mako` formula is an unrelated JavaScript bundler.
  - **A worktree of its own.** The build reads SRCDIR live, so anything that
    changes the branch under a running build breaks it. If the main checkout
    is also being worked in, put this branch in `git worktree add`.

## Host prerequisites on macOS

Beyond what `configure` finds by default, this host needed `gawk`, netpbm
(for `pngtopnm` and `ppmtoilbm`) and the Python `mako` module. `mako` is
checked for in `configure.in:677` but nothing in this tree imports it, so
it is a configure-time gate only.

Two things in the build system had to be fixed before it would get as far as
a compiler, both of them upstream problems rather than anything to do with
this target, and both reproducible wherever the host compiler is clang:

  - The host binutils names were built from the compiler's command line
    rather than its name, giving `llvm-argcc`, `llvm-ranlibgcc` and
    `llvm-ldgcc`. configure reported success anyway and `HOST_AR` reached
    `host.cfg` as a bare `cr`.
  - `CXXCPP` was set to the C preprocessor complete with `-std=gnu23`, which
    gmp's `AC_PROG_CXXCPP` sanity check rejects.

## What was taken from the earlier attempt

The `feat/riscv32-esp32p4` branch was a freestanding bare-metal sketch that
defined its own `ULONG`/`APTR`/`struct Resident`, was built by a shell script
of 26 hand-written gcc calls rather than by mmake, and was never registered
in `configure`. Its structure is not reusable. What carries over is the
research: the SRAM and PSRAM addresses, the D1001 peripheral inventory
(GSL3670 touch, PCF8563 RTC, ES8311 audio, SC2356 camera, ESP32-C6 radio),
the dual-bank OTA flash layout, and the 6LoWPAN engine in
`radio/sixlowpan.c`, which is the one part written as real code and which
can be revisited once a SANA-II driver has something to sit on.
