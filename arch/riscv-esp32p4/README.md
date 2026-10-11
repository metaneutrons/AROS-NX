# AROS on the Espressif ESP32-P4 (riscv-esp32p4)

Generic kernel+BSP A/B support (2026-10-11): all supported P4 profiles
(`d1001`, `jc1060p470c-v1`, `jc1060p470c-v2`) accept `P4_FLASH_LAYOUT=ab`.
The default remains `legacy`; use a separate build tree for each board/layout.
The common layout reserves 256 KiB for boot infrastructure, two512-KiB kernel
slots, two6-MiB BSP slots and independent2816-KiB DEVELOP. The table at0x20000
leaves120KiB for the bootloader at0x2000. D1001 retains its separate upper-flash
storage; no upper-flash MMU support is implied.

The runtime resolves the actual running XIP slot and its matching BSP from
the live table, rejecting ambiguous/overlapping/out-of-bounds layouts. The
patched bootloader validates each candidate pair before selection. Explicit
target `kernel-package-esp32p4-ab` produces `aros-bsp-ab.bin`: a128-byte envelope
binds the complete core and raw PKG with SHA-256. Hashes detect mismatch and
corruption, not publisher authenticity. Use this artifact for A/B, never the
raw legacy PKG. Envelope overhead counts against the6-MiB BSP capacity.
DEVELOP images use exact bytes and an inner FAT16 partition at LBA1; legacy
images retain LBA2048. Size checks fail for missing or oversized artifacts.

The isolated JC-v2 A/B core and paired package builds passed; other profiles
have host checks only. Bootloader API compilation is not full linking.
Host checks and native compile checks are not hardware qualification. Native
flash updates, successful-boot confirmation, power-loss rollback and migration
remain open; the standalone A/B table-write target refuses activation. Installed
firmware and frozen WLAN artifacts remain unchanged. See roadmap M4-AB and
the [ESP32Board plan](../../docs/plans/esp32-board-tool.md).

Explicit WLAN test exception (2026-10-11): Fabian requested an association
test after the unknown installed C6 password-logging risk was explained,
postponing the C6 update. `P4_WIFI_ASSOCIATION_TEST_RISK_ACCEPTED=1` now permits
diagnostic association with the persistent owner without setting any privacy/
recovery verification assertion. It defaults0, contains no credentials and
does not enable OTA. This is accepted test risk, not production qualification;
hardware association/DHCP/traffic remains untested. Release assertions stay0.

Matched diagnostic deployment artifacts are now prepared (2026-10-11): the
actual installed BSP was read twice identically, and only its WLAN resource
was replaced in a 42-member audited candidate. The rebuilt ABI11 system image
passes all 332 manifest entries and host filesystem verification. The SD image
is now written and fully readback verified after two identical backups of the
overwritten 64-MiB prefix; Fabian returned the card to JC1060. BSP deployment
and independent readback now pass. The first WLAN-only replacement failed the
shared-host admission gate: bundled SDMMC ABI7 versus required ABI8. The corrected
package refreshes the host and sdcard together; other40 candidate payloads are
unchanged. Its warm boot passes host admission and SDIO setup but INIT still
fails stage13/error6. A coordinated fresh C6 cold start is the next diagnostic,
not an assumed fix. No credentials are embedded and C6 firmware is unchanged.
The unchanged baseline returned from USB reset, but its old WLAN INIT failed
at stage13/error6; neither run is an association pass. See roadmap N2T.

C6 bootstrap sequencing (2026-10-11): the
[first-install runbook](../../docs/plans/esp32-c6-first-install.md) separates
independent CN5 ROM recovery/app-only installation from the later AROS OTA
updater. Undeployed discovery cannot qualify its own installation. Actual
connector/adapter, installed security/layout and usable restore remain open;
CN5 photograph requested. No device operation or release change occurred.
UPDATE-C6 remains unavailable because public dispatch and device admission
are incomplete, not because bootstrap must wait for the finished OTA tool.

Read-only image-to-target diagnostics (2026-10-11): `COMPARE-C6` now shares
one CLI/MUI backend. It opens/inspects an owned application once, queries the
negotiated C6 registry, rechecks the held bytes and produces explicit rejected,
unknown and risk masks, then disposes the source. Invalid files do not query
the radio. The query leaves WLAN offline; GUI close uses discovery cancellation.
CLI returns FAIL on rejected conditions, otherwise WARN, never update approval.
The image parser now retains separate eFuse-block revision bounds and MMU-page
exponent. Discovery-v2 lacks those corresponding device/loader facts;
installed policy and registry provenance always remain unknown. Full physical
secure-version count is a risk screen, not the installed anti-rollback floor;
the upper wafer-revision screen conservatively assumes no disable override.
The new GUI action can display the historical partition observation, not a
live write target. No update dispatch or release flag is enabled. Host tests
and a fresh native build134864B (SHA256
5c65baa928ae339115a0ac77cd60bd268e78fac6c4c6f026a781a504b5c2a7ab)
are recorded in N2M; no hardware operation occurred.

Discovery target facts (2026-10-11): the undeployed extension now negotiates
version2, with a canonical40-byte header and1064-byte maximum. It adds detected
C6 image chip ID, MXX revision and a full physical SECURE_VERSION burned-bit
count with independent known bits. Unknown values are zero, not presumed safe.
The IDF app-config-sized secure-version helper is unsuitable for this fact;
the collector uses a status-checked full-field count instead. Installed
bootloader policy, image compatibility and independent restore remain unproved.
Private ABI11 requires matched resource/device/tool rebuilds; standalone builds
are not repackaged or deployed. A fresh complete C6 version2 build now succeeds:
app1110928B SHA2563583e341ea108a087a924c4320833f3cad3c3be436070c1ebf5d80f14292b2f4.
The old version1 candidate is preserved. Candidate table bytes match it, not
the installed device; the new bootloader differs and is not flash-qualified.
Real image/hash preflight and fresh RPC/privacy helper tests pass. Separate
static stack reports give3360B for nested RPC/collector/parser before outer IDF
frames in the5120B worker; runtime stack margin and UART privacy remain open.
UPDATE-C6 and all three release assertions remain off. No hardware action.

C6 inspected-source ownership (2026-10-11): CHECK-C6 now uses a shared
tool-private loader that opens once, retains bounded verified bytes, closes
before publication and frees on refusal. A future admitted update can recheck
those same held bytes against the original digest and inspected metadata,
without reopening the path. The current read-only CHECK disposes them after
inspection; recheck has no production update caller yet. Caller-supplied SHA256
is not publisher authentication. Authentic manifest, live target/layout and
device recovery proof producers remain absent; no proof boolean is fabricated.
The fresh RV32-SMP tool build is129432B; actual loader/check/image/hash host
tests pass26 cases/374 checks each O0/O2 ASan/UBSan, including primary repeat
and independent review. DOS/Exec are modeled; no native file interaction or
recovery is tested. UPDATE-C6 and all three release
assertions remain off; no hardware, serial, flash, SD or credential action.
Detailed evidence and limits are in roadmap N2M.
Historical compatibility audit: discovery version1 lacked chip revision and eFuse
secure_version; its capacity/slot inventory is not full image compatibility.
Installed bootloader policy and first-install recovery remain unknown. Actual
CN5 adapter/cable availability was requested, not assumed or exercised.

Bootstrap update release separation (2026-10-11): installing the C6 privacy
fix must not require that fix already installed. OTA hook registration and
the three native OTA admission boundaries now use a resource-private update
gate, not `association_enabled`. Its separate
`P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED` assertion defaults0 and requires the
persistent owner, independently secured C6 restore/full-transfer qualification
and host privacy/recovery release. Each job still requires source inspection,
trusted origin, target/layout compatibility and recovery proofs. This is not a
CLI override or runtime proof. Credential association still requires the two
original C6/host privacy assertions; none was enabled. ABI10/structure layout
is unchanged. All three assertions remain0, UPDATE-C6 unavailable; no hardware,
flash or credential action occurred. Native resource/device/tool builds and
standalone boot-module audits pass. Extracted OTA admissions/maintenance pass
9416 checks each O0/O2 sanitized; the 16-configuration hook matrix passes480
checks, with privacy-coupling and missing-call counterprobes rejected. Primary
repeats and independent review cover the fully unassociated bootstrap state
and both runtime association-enable values. These synthetic helper tests do
not qualify recovery, a full transfer, native SMP or positive release flags.
Tests/builds and limits are in N2M's dated evidence; full WLAN and updater
acceptance remains open.

OTA requester drainage (2026-10-11): ESP32Board now links a shared metadata
client and a private native prepared-job adapter. Submission is once-only;
caller cancellation/CTRL-C is sticky, not task termination. Invalid or
foreign observations remain pending; only a correlated DONE result followed
by successful discard permits requester exit. Metadata contention retries no
transport, and an owner stall deliberately keeps the requester alive. A drained
WAIT_REBOOT/commit-unknown/incomplete result is not installation or recovery.
This adapter is not invoked by CLI/GUI operation dispatch: authenticated source,
live target/layout, independent restore and a qualified full-image budget still
must be supplied. UPDATE-C6 remains disabled and all three release assertions0.
Nine actual-client scenarios/448 checks pass at O0/O2, with and without
ASan/UBSan, including a primary repeat. Native ESP32Board builds126464B;
independent review closed terminal-validation and fixture-baseline gaps.
Hooks are mocked in host tests; abstract progress values are not native queue
or SDIO evidence. No hardware or credential action accompanied this change;
artifact hashes and limitations are recorded in roadmap N2M's dated entry.

Initial OTA bridge (2026-10-11, gate policy superseded above): ABI10 appends
typed owned-job submit/poll/cancel/discard hooks. Originally every hook required
both credential privacy/recovery assertions, which prevented privacy-fix
bootstrap updates. The separated default-off policy above replaces that
coupling without enabling a write. Resource, SANA device and ESP32Board discovery require a matched
ABI10 pair/full structure size; do not mix with historical ABI9/ABI8 outputs.
Fresh native builds and scoped registration/consumer tests pass: all eight
registration gate combinations at O0/O2 (80 checks), plus1057 SANA checks each
O0/O2 with ASan/UBSan. A source guard and missing-call counterprobe protect the
initializer hook call; this is not whole-init execution or hardware evidence.
No frontend update action, qualified full-image budget, live device proofs or
secured first-install recovery is supplied; UPDATE-C6 remains unavailable.
No package/image, deployment, serial or credential action has occurred.

Board maintenance foundation (2026-10-10): `Tools/ESP32Board` is restricted to
esp32p4/riscv targets; other targets explicitly refuse direct builds. It builds for
RV32-SMP with shared CLI/MUI read-only C6 image checks and an adaptive graphical
partition-table preview. Imported extents, labels, offsets, sizes, types and
flags determine the view; gaps are unlisted/reserved, not assumed free space.
Capacity is explicitly supplied, not detected; active/update roles are unknown.
Runtime P4/C6 layout discovery, native OTA dispatch, secured recovery and
actual local/HTTPS-acquired updates remain open. UPDATE-C6 fails explicitly.
No board, flash, serial or credential action accompanied this development.
Requirements: [board-tool plan](../../docs/plans/esp32-board-tool.md), roadmap N2M.
Independent pinned protobuf-c comparison passes 1,643 checks at each O0/O2;
normal WLAN control submission refuses all three OTA request types. These are
host protocol/safety checks, not evidence of a working C6 update or recovery.
The isolated OTA engine now distinguishes terminal maintenance policies:
no BEGIN attempt, incomplete transfer, unknown commit and acknowledged END
waiting for reboot. Only the first allows ordinary read-only lease release;
the others require a confirmed bounded fenced handoff, without automatic
radio restart, reset or request replay. Failed handoff blocks installation
confirmation. The owned queue and ABI10 hooks described here do not enable
UPDATE-C6 or prove physical recovery; frontend dispatch remains absent.
Terminal-policy regressions pass692 checks each O0/O2 under ASan/UBSan;
the compiled all-NO_BEGIN mutation fails at the expected assertion with exit1.
The standalone engine compiles for the configured RV32-SMP ilp32f profile
without undefined imports. That historical standalone object was not linked;
the synchronous owner binding now links the actual engine into the resource.
The low-level owner now has a separate canonical OTA transport entry, without
widening normal WLAN submission. Its native maintenance-job adapter, complete
transaction budget, terminal handoff and hardware qualification remain open;
private ABI9 exposes no OTA and UPDATE-C6 stays unavailable.
Native ABI9 adds a task/token-bound terminal fence hook (2026-10-11).
Accepted INCOMPLETE/COMMIT_UNKNOWN/WAIT_REBOOT dispositions queue a sticky
fence; only an owner-confirmed FENCED result establishes host ownership handoff.
Confirmation uses the original lease and a bounded fresh host acquisition.
After acceptance, ordinary release and teardown Stop/Deinit/CLOSE/restoration
are prohibited, even if confirmation fails. A confirmed fence remains silent
after lease expiry; no automatic reset/reboot/reassociation is performed.
This is not C6 outcome/health attestation or a full OTA job. The future native
dispatcher must establish the sticky obligation before its BEGIN attempt.
Matched ABI9 resource/device/tool builds are standalone, not repackaged or
installed. The previous ABI8 BSP/image remains a historical candidate; do not
mix it with ABI9 clients. Both privacy/recovery assertions remain0.
Source-extracted fence tests pass2057 checks each O0/O2 with ASan/UBSan;
ACTIVE-poll and sticky-cleanup mutations are rejected. These are host tests,
not C6 runtime qualification.
The isolated engine now requires one aggregate transfer deadline, a protected
finish reserve, a valid monotonic clock and explicit bounded callback timeouts.
No WRITE renews that deadline. A late END acknowledgement remains WAIT_REBOOT,
but timing failure or unconfirmed finalization blocks installed confirmation.
Total time and reserve have no production defaults: native binding and actual
full-image timing qualification remain open. UPDATE-C6 stays unavailable.
Aggregate-deadline host regressions pass962 checks each O0/O2 with ASan/UBSan;
NO_BEGIN and ignored-reserve mutations are rejected. The fresh standalone
RV32-SMP engine object has no undefined imports; its later resource binding
below is not deployed.
Native pre-BEGIN fencing prerequisite (2026-10-11): a sticky ACTIVE disposition
now refuses new discovery, suppresses any queued discovery without an RPC and
prevents copying a stored READY result while sticky.
The requester can queue confirmation of the same pre-armed disposition only;
changing it or repeating the request after FENCING is refused. Source-extracted
tests pass2450 checks each O0/O2 sanitized; the fresh Core1 resource passes its
loader audit. No OTA job sets this pre-arm yet, no ABI change or deployment
occurred, and UPDATE-C6 remains unavailable. Native job/engine integration,
full-image timing, authenticated target/layout and independent recovery remain
required; this prerequisite does not establish a usable updater.
The native resource now contains a port-private owner-only OTA exchange entry.
It requires the registered resource instance, owner task and client lock, no recursive shared-host ownership,
an ACTIVE live task/token/generation lease, C6/SDIO facts, both existing release
assertions and all four trusted caller update attestations. BEGIN arms sticky
INCOMPLETE before reservation/encoding/TX; WRITE counts matching status-zero
acknowledgements only; END requires the exact image length and arms
COMMIT_UNKNOWN before its attempt. Only a verified successful END advances to
WAIT_REBOOT. Entered failures close the transaction without replay or normal
radio release. Its local image ceiling matches the engine, not live capacity.
This entry is linked into the standalone resource but has no ABI job/queue or
frontend dispatch. Native engine binding, aggregate full-image lease/budget,
immutable authenticated source and live layout/recovery proofs remain open.
Both release assertions remain0; UPDATE-C6 is still unavailable, not deployed.
Source-extracted wrapper regressions pass4480 checks each O0/O2 ASan/UBSan,
including genuine-instance admission, atomic refusals, exact acknowledged-byte
ordering, failure closure and late acknowledgements. Three unsafe mutations
are rejected. Fresh Core1 resource299324B SHA256
601d7e0a79cb79e431f36289eec74ae58723ee826d940a7b605af7f3524af0ce
passes its loader audit; the private entry and actual lower transport/decoder
are linked. Exec/transport callbacks are modeled; no runtime OTA or deployment
is claimed. Full evidence is recorded in roadmap N2M (2026-10-11).
Native owner-side finalization now maps engine modes explicitly to ABI9 rather
than waiting on the requester hook from the owner task. A pristine NO_BEGIN
lease clears discovery and confirms one bounded restart; begun modes close
the transaction and confirm a fresh host fence without an RPC. FENCED is
published under host ownership; a late release remains sticky FAILED.
This is still a private prerequisite: queued jobs, engine binding, an explicit
qualified full-image lease and actual updates remain open. UPDATE-C6 stays
unavailable and no hardware has been changed.
Direct-finalizer source tests now pass5913 checks each O0/O2 ASan/UBSan,
with four unsafe mutations rejected. Fresh Core1 resource303616B SHA256
ef9547e5c8330c355f51583696ade6262bbfbe11240ef19931898612056c3352
passes its loader audit. Exec/host ownership and RPC callbacks are modeled;
this is not a full engine-bound job or runtime handoff acceptance. Direct
host-generation-change and failed pre-BEGIN restart cases also pass; neither
may confirm handoff or replay a request.
The resource now binds the actual engine synchronously to native BEGIN/WRITE/
END and direct owner finalization. Input bytes must be frozen and owned by a
trusted future dispatcher, with nonblocking ownership/cancellation queries.
The copied descriptor and temporary context are not a persistent queued job.
The engine clock starts at the original maintenance request, including prior
quiescence; explicit total/reserve/call budgets cannot renew or extend its30s
preparation lease. Callback pre-arm precedes local BEGIN/END admission, and
skipped finalization retains sticky failure. Full-image lease qualification,
asynchronous CLI/GUI dispatch, authenticity/live layout and secured recovery
remain open. The corrected native build and loader audit pass; independent
review is complete and coupled tests pass7065 checks each O0/O2 ASan/UBSan,
with six unsafe mutations rejected. Accepted early engine
exits finalize the already-held lease as NO_BEGIN within the original budget;
clock/deadline failures instead mark FAILED without a late restart. Invalid
admission leaves the original requester responsible for its lease.
Both release assertions remain0, no public ABI change or deployment occurred.
Private native queue implementation now copies an entire trusted image before
requesting quiescence. The submitting task owns a non-reused token; the owner
alone runs the actual engine. Poll/cancel/discard use a distinct metadata lock;
cancellation is atomic and a busy progress poll cannot cancel a valid transfer.
RUNNING results are never copied; terminal result and acknowledged-byte counts
survive owner teardown, with source storage freed exactly once after execution.
Close requires the worker to survive cancellation through DONE/discard; killed
requester recovery is not provided. Fresh native build and loader audit pass;
queue tests pass9121 checks each O0/O2 ASan/UBSan with seven unsafe mutations
rejected. Independent implementation and bounded fixture review are complete;
deferred publication through later service or retirement cannot replay OTA or
free the source twice in the fixture. Exec/allocator/transport are mocked, not
native SMP or full-image hardware qualification. The optional ABI10 bridge
above supplies no CLI/GUI update action or source/layout/recovery authorization.
UPDATE-C6 remains unavailable,
both release assertions0, full-image lease qualification still open.
Private transport verification (2026-10-11): canonical codec11806,
pending1638 and coupled owner8093 checks each O0/O2 ASan/UBSan pass;
ordinary OTA-allowlist mutation is rejected. Fresh Core1 resource291220B
passes the loader audit with both privacy/recovery assertions0. This resource
is not yet repackaged or installed; the prior282952B discovery-only resource
remains the member in the previously audited BSP/image candidate.
Native private ABI8 introduced a task-bound, nonblocking maintenance lease and
queued read-only discovery with copied results (2026-10-11); scoped host tests
and native builds pass, runtime qualification remains open. ABI7 introduced
the original lease handshake.
The owner drains pending work, confirms WiFiStop and suppresses normal traffic
and FIFO polling while ACTIVE. Release confirms WiFiStart but leaves SANA
offline/unassociated. The 30-second lease is handshake preparation, not a
full-image transfer budget; no OTA job is exposed. Ordinary lease expiry retires
the session with bounded owner-only cleanup, not silent traffic resumption.
ABI9 accepted terminal fences prohibit that cleanup. Original ABI7
actual-source host tests pass186 checks each O0/O2 with sanitizers and a negative
ACTIVE-poll mutation; Exec/RPC are mocked. Its historical RV32 Core1
resource254956B/device67604B passed individual loader audits; these are not the
current ABI8 artifacts. Resource/device must be replaced together;
package/image regeneration and runtime acceptance remain open.
A separate C6 read-only metadata producer now compiles against actual pinned
IDF5.4.3 headers. It distinguishes detected physical and usable flash sizes,
normalizes internal IDF registry entries and correlates sorted running/boot/
update roles. Unknown bootloader rollback policies stay unknown, and virtual
eFuses never establish hardware-security facts. This is not raw installed-table
attestation or live discovery. The C6 candidate now includes the handler and
advertisement; native P4 negotiation and GUI binding now exist in source,
while installed/runtime qualification remains open. No board or
credentials touched.
Actual-source host tests pass35 cases each O0/O2 under ASan/UBSan:205 checks
for normal security APIs,204 for virtual-eFuse builds. IDF runtime is mocked;
object compilation and these tests do not establish installed C6 behavior.
The normalized inventory now has a strict bounded1056-byte codec, checked in
45 cases/136 assertions each O0/O2 sanitized. Generated Protobuf discovery350/
606, response cleanup and version1 startup TLV0x17 are linked in a fresh
undeployed C6 candidate1110496B. The dispatcher bounds inputs by the4096-byte
serial reassembly capacity before unpacking and releases requests on error.
Actual handler/generated-code host tests pass145 checks/10 cases each O0/O2;
the collector is stubbed and live IDF dispatch/transport is not qualified. It
rejects mismatched UID/type/version/nonce and reports failure without inventory.
Native INIT parsing now retains the advertised discovery version and supports
version1 only on C6/SDIO. Separate native discovery-envelope functions leave
ordinary RPC ranges unchanged. The native request codec requires the supported
capability; response decoding validates ID/type/UID, echoed nonce, status and
canonical inventory. This is not live epoch/lease admission. Fresh Core1 RV32
resource builds and passes the loader audit. Generated-reference host tests pass
1854 checks/12 cases each O0/O2 under ASan/UBSan, including32-entry inventory
and maximum UID/nonce. These are codec tests, not runtime or stack qualification.
Native owner transport, runtime binding and frontend invocation now exist;
hardware qualification remains open. No new RPC has been sent on hardware;
UPDATE-C6 remains unavailable.
The private pending transport now separates discovery350 submission and606
correlation from ordinary WLAN traffic. Exact reserved UID/generation,
supported INIT and canonical version/nonce are required before TX. Transport
admission does not prove a live maintenance lease or peer freshness; runtime
owner invocation and frontend publication now exist through ABI8, but are not
runtime qualified. Private RX now
admits one606 response only for an explicitly armed generation/UID, retaining
duplicate rejection after pop and clearing admission on invalidation. Host
tests and a fresh RV32 resource/loader audit pass; this is not live discovery.
The exclusive transport owner now provides a separate bounded discovery
exchange: exact reserved350, supported INIT and nonce, no pre-existing partial
SERIAL stream or admission arm, one606 response and cleanup on success/failure.
This uses the existing absolute deadline and shared-controller arbiter. Runtime
maintenance-job dispatch and snapshot publication are now wired through ABI8.
Actual-source helper tests pass1239 checks each O0/O2 ASan/UBSan and reject the
ACTIVE-poll mutation; Exec, codec/UID/transport and host acquisition are mocked.
Separate actual codec/coupled-owner suites also pass. Fresh RV32-SMP Core1
resource282952B/device67584B pass individual loader audits. Only the owner performs transport; the
requesting task queues and copies results within its lease. A fresh shared-host
acquisition fences final publication, and unsupported peers receive no query.
CLI/GUI now share DISCOVER-C6 through a native ABI8 adapter and bounded
read-only transaction (2026-10-11). It checks the exact ABI/size/hooks, uses
timer.device ReadEClock rather than wall time, and reserves eight seconds for
release within one original30-second client budget. Only a correlated,
decoded inventory followed by confirmed release reaches the CLI/GUI. GUI
closure cancels discovery but waits for bounded cleanup. Unsupported peers,
failed/late release or malformed replies do not publish a new view. The
query stops WLAN and leaves it offline/unassociated; it does not write flash,
send credentials, reset or automatically re-associate. Both frontends label
copied inventory as a historical normalized registry observation, not raw
installed-table attestation, peer authentication or write/recovery authority.
The graph shows reported roles and security-known/enabled masks in tooltips;
unknown policy is not disabled policy. UPDATE-C6 remains unavailable.
Matched ABI8 package/image regeneration passes host verification
(2026-10-11):3971508B BSP,42 loader-valid members,41 other payloads unchanged;
64MiB AROSDefault image,332 exact mounted manifest entries, original unchanged.
Tools/ESP32Board and its generic tool icon are included in the85-file WLAN
overlay; eighteen staging tests pass. These candidates are not deployed and
are not backups of the installed board. Actual native/C6 runtime, UI, clock,
cancellation/release and stack qualification remain open.
Actual portable client plus real inventory decoder/validator pass286 checks
across41 cases each O0/O2 ASan/UBSan, with an accepted-release mutation rejected;
adapter hooks are mocked. Fresh RV32-SMP ESP32Board121796B compiles and links
without warnings/errors. Six generated-make target checks still refuse other
ports; shared frontend source guards and imported-layout regressions pass.
These are source/build evidence, not native adapter, GUI or device acceptance.
Owner tests pass7316 checks each O0/O2 under ASan/UBSan, including private
success, terminal cleanup, subsequent normal exchange and atomic refusals.
CLI examples (not yet executed on hardware):

    ESP32Board ACTION INFO
    ESP32Board ACTION CHECK-C6 IMAGE app.bin SHA256 <trusted-64-hex-digest>
    ESP32Board ACTION LAYOUT TABLE partition-table.bin SHA256 <trusted-digest> FLASHMB 4
    ESP32Board ACTION DISCOVER-C6
    ESP32Board GUI

The digest must come from trusted metadata. File/hash acceptance is neither
publisher authentication nor installed-target compatibility/recovery proof.
DISCOVER-C6 needs both the matched ABI8 owner modules and an installed
compatible C6 extension; stock Hosted receives no extension RPC. The C6
candidate is still undeployed and requires independently secured recovery
before first installation. A compiled command is not a working hardware query.

WLAN bootimage development (2026-10-10): explicit JC1060 WLAN image targets
now consume the native overlay and exclude saved build Env-Archive preferences.
Stock source Wanderer preferences and ABI add five files (83-file overlay).
Seventeen host stage tests pass; ESP32 WiFi uses a volatile compile-time default,
validated in the actual ELF. Normal image profiles retain their behavior.
The 64 MiB image builds and passes independent host FAT32/fsck and exact
329-file manifest verification; original hash remains unchanged. GUI/radio
qualification remains open; no
hardware, credentials or privacy assertion changes have occurred.
Initial deployment pairing audit: the local BSP package contains an older
31652B radio resource, not the standalone254812B owner resource. The image
alone cannot qualify scanning; rebuild/check the matching ABI6 BSP before
deployment. C6 recovery/privacy and actual radio tests remain open.

WLAN loader correction (2026-10-10): default module linking produced runtime
references to loader-discarded .eh_frame in both the resource and SANA-II
device. BSP static linking removes those invalid references in the fresh
249944B resource/67492B device; individual loader audits and42-member test
package audit pass. The WLAN stage now rejects loader-invalid devices before
writing output. Earlier329-file FAT verification did not establish driver
loadability; the old image is superseded, never deployed. Current image is
regenerated with the corrected device. Package and image remain undeployed;
CN5 cable is not yet available, so independent C6 UART recovery is not established.
The published schematic exposes six pins (3.3 V, GND, TX, RX, IO9, CHIP_PU),
but connector family/pitch and actual-board orientation still need verification.

WLAN lease-expiry development (2026-10-10): generic2298252dad is integrated.
The chooser rejects missing, malformed, duplicate or expired lease timestamps,
including expiry exactly at now; an unusable latest matching record clears old
values. Actual helper tests pass203 checks in host/32-bit time_t at both O0/O2
with ASan/UBSan, plus6 source guards. UTC calendar, epoch and never formats
match dhclient's writer; DOS DateStamp supplies the clock without a worker-owned
lazy libc timer request. Unset/invalid/32-bit-overflow clocks are refused.
Fresh RV32-SMP app151956B builds and stages successfully. This closes the
source expiry issue, not real clock/GUI/resolver/routing or WLAN qualification.
No hardware or credentials; bootimage and C6 secured recovery/privacy remain open.

WLAN lease-selection development (2026-10-10): generic 6d06770289 is integrated.
Prefs/WiFi selects the last complete lease matching the exact interface and
kernel-reported address; options cannot cross records. Duplicate identities
are rejected. Static gateway lookup respects interface rows, continuations,
case-insensitive GW= and the configured database. Actual-source host tests
pass144 checks each O0/O2 ASan/UBSan plus5 source guards. Fresh native app
141332B builds successfully. Lease expiry is not yet evaluated: a matching
record can still be historical, and does not prove installed global route/DNS.
That review issue remains open before first-use acceptance. No hardware or
credentials; bootimage, C6 recovery/privacy and WLAN qualification remain open.

WLAN lease-path correction (2026-10-10): generic 0517b53be9 is integrated.
Prefs/WiFi uses PATH_DHCLIENT_DB or DHCP's /T/dhclient.leases default through
the existing POSIX C runtime, not raw DOS Open. Actual reader tests pass265
assertions each O0/O2 ASan/UBSan plus7 source guards; native app131100B links
and stages with WifiVolatile=1. Independent review confirmed the path delta,
but exposed existing last-lease selection across interfaces: fix matching to
the current interface/address before first-use acceptance. Bootimage/runtime,
C6 secured recovery/privacy and real WLAN qualification remain open.

WLAN runtime staging (2026-10-10): the credential-free whitelist generator
now stages 78 files from actual RV32-SMP outputs, including Network protocol
modules, AROSTCP, DHCP and C runtime libraries. Five defaults tests and twelve
stage tests pass; independent scoped source review found no blocker. Compiled
WiFi must have initialized WifiVolatile=1. Package lookup is predefined and
sample LAN routes/DNS are removed. This remains an overlay, not a bootimage:
mmake consumption, lease selection qualification, parser/runtime and
radio qualification remain open. No hardware, credentials or privacy change.

WLAN chooser development (2026-10-10): generic Prefs/WiFi gains an explicit
VOLATILE mode, hex SSID serialization, runtime-only stack database selection,
checked backup/rollback and owned secret-buffer clearing. Generic d43e1b9d8b
is integrated; macOS O0/O2 ASan/UBSan tests pass222 formatter and1538 extracted
worker-helper assertions plus20 source guards. Independent static review is
clear within this delta; orchestration/Exec/radio are not host-tested. The native RV32-SMP
application links with WIFI_VOLATILE_DEFAULT=1 (124752B, SHA-256
1f5f253fc364e665feb3f5e628d3479f4ed887d69eb7e4059018088f9eed3255).
Workbench uses that compile-time default; ENV must be RAM-backed and contain
the complete database. Image staging, stack dependencies and actual GUI/radio
acceptance remain open. No hardware, credentials or privacy-gate changes.

Network preference classification (2026-10-10): generic NSD capability queries
replace the legacy device-name list. Exact remembered device/unit identity is
owned, loaded and preserved only for an UNKNOWN configured device; positive
wireless results take precedence, and removed selections are cleared on disk.
Mac O0/O2 host tests pass with and without ASan/UBSan; Exec/timer calls are
mocked. No scan, association or ONLINE command is issued. The two-second
request timer does not bound OpenDevice or a nonconforming AbortIO/WaitIO.
Native RV32-SMP Network app links successfully (268728B, SHA-256
f6b4c42b3dac97488e663bb89799bceb1df323e769a47456fb6ed457594c000d).
Protocol modules, image integration and actual device/GUI acceptance remain open.

WLAN build defaults (2026-10-10): a separate credential-free preferences
overlay generator and four host tests now pass. Native device/unit0, DHCP,
RAM database and DHCP temporary paths are predefined; autostart stays off.
Not yet consumed by image targets. Standard database/binary dependencies,
single-manager GUI launch and runtime acceptance remain open. First delivery
does not persist credentials or relax read-only SYS/SD protection.

C6 legacy logging extension (2026-10-10): fresh full build passes with
Wi-Fi, coexistence and PHY shared formatters suppressed specifically for C6.
App1105312B SHA-256
149ec760ce40bbf159d1d780f001f44de19477b5076e40a91a15764410ce0b86.
Actual-body Mac/Linux O0/O2 sanitizer tests and exact sink guards pass for
all three helpers. Static error-message puts routes remain; this is not a
whole-UART privacy certificate. Earlier candidates/reference are unchanged.
No deployment or credentials; installed identity, secured restore, actual UART
privacy and association/IP/reconnect/load acceptance remain open.

C6 logging candidate build (2026-10-10): fresh full build against a separate
patched IDF5.4.3 tree passes; app1105760B SHA-256
d4a992b9aa806d61b0e406b848dfc4e449f3a06eab102120b84453c7ae7c358e.
Remaining Hosted Wi-Fi/Bluetooth MAC and direct country/HE AP prints removed;
RPC payloads/events retained. Both vendor log callbacks have no outgoing calls
in the ELF. Mac/Linux O0/O2 sanitizer tests pass18 callback and490 string checks
each, with source guards. Earlier candidate/reference unchanged. Direct vendor
stdio/PHY diagnostic paths, secured recovery and actual UART privacy remain
separate gates; no deployment, credentials or WLAN acceptance.

C6 vendor logging development (2026-10-10): a pinned IDF5.4.3 C6 adapter
patch sinks both vendor logging callbacks without formatting their arguments.
Exact-body macOS/Linux O0/O2 ASan/UBSan tests pass18 checks each; original
callbacks fail the negative control. This is a source-tested recipe extension,
not a rebuilt/deployed firmware or whole-UART privacy proof. The earlier
1106624B candidate is unchanged; direct diagnostic paths, remaining Hosted MAC
logs and physical secured recovery/UART qualification remain open.

Caller Close cleanup (2026-10-10): generic9a0a0d8346 integratedd50ca3ba1e
cancels keepalive, unregisters its exact callback tuple before freeing state,
drains/frees the event request and clears the owned signal before port deletion.
Amiga signal dispatch defers compaction during callbacks. Independent source
review and macOS/Linux O0/O2 ASan/UBSan scoped suites pass (Close4/55,
association84/555, failure23/93, state554). Full RV32 rebuild passes679652B,
SHA-2564a74b5bcdd73571cf77932c42a360331ad021b1c455b63efc77c89af560822ec;
all three changed compiled copies match source. Exec/radio lifetime acceptance
remains open. No hardware or credentials used.

Native failure publication development (2026-10-10): owned attempt cookies and
opener-local failure latches are wired into the device. A failure is delivered
once, separately from link edges, and discarded on cancellation/supersession/
offline/close. Owner execution waits until a forced message acknowledgment has
returned; PA_CALL callbacks run without the unit lock and post-reply lookup
uses only the non-reused selection token. Independent source review is clear;
expanded macOS/Linux O0/O2 ASan/UBSan model suites pass: options91/275,
events9/113, queue1040, lifetime1506, datapath16507 and scan295.
Clean RV32 device build passes72936B,
SHA-2567c5085dd63c03a6622f343d2b7e0b96a737ffe9ba38ed40b1011fb45d7f4e234.
No capability advertisement, credential permission or hardware acceptance added.

Native failure metadata preparation (2026-10-10): the bounded options parser
can copy a nonzero 32-bit AssociationCookie independently of the radio target
ABI, rejects duplicates/overflow and clears it on refusal. Legacy consumers
reject rather than silently discard a supplied cookie. macOS O0/O2 ASan/UBSan
parser tests pass (91 cases/275 checks); native queue/event binding, caller
close cleanup and runtime qualification remain open. No new advertisement,
credentials or hardware action.

Association failure caller (2026-10-10): upstream-based commit46e9387fee,
integrated as16a13dd08d, negotiates a cause-free failure event with a nonzero
non-reused per-open cookie. Local pending cancellation reaches standalone leave,
retires the cookie and stops keepalive; canceled CONNECT with no active attempt
is ignored. Independent review found no remaining blocker in this delta.
Mac/Linux O0/O2 ASan/UBSan scoped suites pass: association84/555,
failure23/93, state554, integration38/76, summary88/264. Public RV32 ABI and
full WirelessManager build pass (676928B, SHA-256
b1df162ae0f6e25f66df011fb2e2bef1de5630ac86bd6a52f8b6ea95139a6d41).
This is not event-loop or radio acceptance: native provider/cookie/latch binding,
caller close-time timer audit and actual C6/WLAN qualification remain open.
SUMMARY-only advertisement, both privacy gates0 and hardware are unchanged.

Native radio lifecycle development (2026-10-10): private ABI6 adds bounded
owner-service Disconnect/absence confirmation, queued administrative teardown,
standalone Disassociate options and correlated periodic current-AP observation.
UID0 hints close traffic but cannot claim loss; confirmed DOWN or a different
current AP revokes a previously accepted link. A matching confirmed UP restores
only that accepted link, never creates unsolicited CONNECT. Selected RPCs drain
before caller cancellation cleanup under the original deadline, preserving a
healthy session for a later selection. Source-extracted models pass on macOS
and network-isolated Linux at O0/O2 with ASan/UBSan: association25999,
AP observer393, bridge219, options89 cases/258 checks, queue785,
lifetime1482, datapath16507, events6/70, scan295; sibling link102,
reservation63 and owner scan37447. Clean Core1 RV32 modules
build with no undefined imports; this is not hardware evidence. Explicit
teardown requires confirmed absence: an AP still present, including a foreign
AP, leads to bounded retry and then quarantine rather than a loss notification.
Both privacy assertions remain0 and
SUMMARY-only capability remains. Caller-visible asynchronous association failure,
real reconnection, C6 recovery/privacy and WLAN/IP/load acceptance are still open.

Native association queue development (2026-10-10): private bridge ABI5 adds a
credential-free correlated AP snapshot and native SETOPTIONS/GETNETWORKINFO
binding. One pending and one active opener-pinned attempt own copied inputs;
no acknowledged IOSana2Req is retained. Supersession, Close, Flush and Offline
invalidate attempts and native traffic. The owner service performs RPC without
the unit lock, then commits CONNECT, the copied AP identity and both traffic
gates under the lifecycle fence. RX delivery and TX commitment check link tokens;
EAPOL reads may park before association. Pool-owned GETNETWORKINFO data survives
later selections. Clean Core1 RV32 modules build. Source-extracted queue models
pass529 checks, lifetime1061, datapath16507, events6/70, scan295, options78/247,
bridge219 and association snapshot14939 each O0/O2 ASan/UBSan on macOS and
network-isolated Linux, with actual process exit0. This supersedes the earlier unbound-queue
status below, not the outstanding radio/privacy acceptance gates.
Queue tests exercise helpers and selected dispatch arms, not full BeginIO or
actual concurrent tasks. The ABI6 stage above adds active Close drain and
pending/active association-specific Offline host models; actual concurrent
task/radio qualification remains required before enablement.
SUMMARY-only and both privacy assertions0 remain. Explicit radio teardown,
confirmed ongoing DISCONNECT/reconnect, caller-visible late-failure handling,
capability enablement, C6 secured recovery/UART privacy and real WLAN/IP/1800s
qualification are still open. No package, deployment or hardware action.

Association IE extent contract (2026-10-10): generic5b2cf0b82b is locally
integrated. WirelessManager validates one complete readable IE before its
association callback performs device I/O and sends WPAInfoLength only when
the actual device negotiates FOURWAY+IE_EXTENT. Old offload and legacy devices
receive no effective new tag; public capabilities remain version1/32 bytes.
Host association models pass68 scenarios/451 checks and capability models35/70
each O0/O2 ASan/UBSan on macOS and isolated Linux. Public RV32 ABI and a fresh
full RV32 GUI build pass (680384B); these are not hardware evidence.
The native parser now consumes the explicit length tag independently of tag
order, with no separate helper extent parameter. The subsequent native queue
stage above implements the binding; acceptance remains open. No new capability is advertised;
SUMMARY-only and both privacy assertions0 remain unchanged. No hardware,
credentials, C6 update or deployment occurred.

Native association input development (2026-10-10): a bounded options parser now
copies the shared SANA caller's borrowed SSID/BSSID/passphrase or binary PSK into
an owned target. It accepts only managed open or singleton RSN-PSK/CCMP, requires
an explicit readable IE extent, and wipes the full output on refusal. The shared
caller now has the negotiated handover above; native helper consumption is
verified; the subsequent queue stage above binds BeginIO and lifecycle handling.
The parser alone was an input/lifetime prerequisite, not a queued
S2_SETOPTIONS implementation. Actual hardware acceptance, cancellation, link events
and teardown remain open; SUMMARY-only capability and both privacy assertions0
are unchanged. Actual-header host tests pass78 cases/247 checks each O0/O2
ASan/UBSan on macOS and network-isolated Linux; a standalone real-header RV32
fixture compiles. These do not test the native request queue or hardware.
No hardware, credentials or C6 firmware are touched.

Native association development (2026-10-10): private bridge ABI4 adds an
owner-service-only, aggregate-eight-second Disconnect/absence/SetConfig/
Connect/current-AP path, owned credential copies and separate associated
traffic gating. Successful AP snapshots leave traffic closed until the future
native owner binding commits a still-current attempt under the unit lock.
ABI3 and ABI4 clients reject each other. Native committed-link RX/TX fences,
queue cancellation and CONNECT publication were unimplemented at this stage;
the subsequent ABI5 queue above adds them without hardware qualification.
It remains default-disabled behind separate C6 and host privacy/
recovery release assertions; neither build flag is a runtime attestation.
Initial profile coverage is open or WPA2-PSK/CCMP infrastructure;
other security profiles are not silently downgraded. This backend is not yet
bound to S2_SETOPTIONS, and SECURITY_SUMMARY remains the only advertised
wireless capability. Source-extracted association workflow models pass14,406
checks each O0/O2 with ASan/UBSan on macOS and network-isolated Linux;
native event/configuration models pass6 scenarios/67 checks at both levels.
RPC transport, host, clock and Exec boundaries are fakes. No real credential transmission, C6 update or hardware
acceptance occurred. Models, independent review and final artifact evidence
are recorded in ROADMAP.md; development is not a connectivity pass.
Host-side privacy development (2026-10-10): private host ABI8 now makes DMA
preparation own TX staging and tracks residue in the permanent motor. Successful
completion/reset clears it with volatile writes and serialized cache writeback
only after DMA/FIFO quiescence. An unconfirmed reset retains residue and faults
the host until a physical controller reset can safely scrub it. TX bounce aliases
are refused; RX remains staged until final command-status validation. This path
passes source-extracted host models and clean RV32 module builds, not hardware
acceptance. The corrected privacy model passes7 cases/197 checks each O0/O2
ASan/UBSan on macOS and network-isolated Linux; reset/cache/MMIO boundaries
remain modeled. Both C6 and host privacy assertions stay0. Native credentials
remain disabled; C6 identity/recovery/UART privacy is still open.

Binary PSK caller contract (2026-10-10): S2INFO_PSK now represents exactly
32 borrowed binary bytes and requires explicit per-device FOURWAY+RAW_PSK.
The caller prefers a valid passphrase when both forms are supplied. Raw-only
requests without RAW_PSK are refused before I/O. Actual-source models pass
59 scenarios/402 checks each O0/O2 ASan/UBSan on macOS/Linux; capability
models pass35/67, and the public RV32 ABI and clean full GUI build pass.
Generic2eb274ff63 is locally integrated, not deployed. The native device
still advertises SECURITY_SUMMARY only: no credential transmission or actual
association is enabled. Older strict callers reject the new capability flag;
connectivity to offload-only devices requires matching caller support.

C6 recovery/privacy audit (2026-10-10): matching Hosted protocol/version is
not verified flashed-image identity. Stock OTA can erase the inactive fallback
and changes boot selection before a possible timer failure; automatic rollback
is unproved. Native AROS has no OTA updater. The schematic CN5 UART route
requires actual JC1060-v2 connector/pin verification, fresh physical readiness,
non-secret backup/security checks and explicit C6 write authority before use.
Fabian granted conditional C6 update authority on 2026-10-10: inspect firmware
and update only with secured recovery. Physical readiness and a proven restore
path remain outstanding. No C6/serial/credential action was taken. See
network/HOSTED-REFERENCE.md for the gate.
The initial reproduction tree lacked protobuf-c and IDF Python dependencies.
A separate private C6 candidate now builds from the complete pinned source:
Hosted SSID/password/AP prints and payload dumps removed, fixed-width response
length reads bounded. Real-macro models pass490 checks each O0/O2 on macOS/Linux;
the original macro fails the ASan negative control. App1106624B,
SHA25693cc504db3109c33efebef1ff70701f3caadf195f62da8303a8078e6c749229d.
Config/artifacts/dependencies are archived and patch application independently
reviewed. This is undeployed build evidence, not installed identity or secured
recovery. IDF binary SSID/BSSID log formats and device/client MAC logs remain;
whole-firmware UART privacy still needs verification. Recipe and scope:
tools/c6-private-firmware/README.md.

Association-caller correction in development (2026-10-10): the separate
upstream-based wireless checkout now validates representable SSIDs and
borrowed options before device I/O, removes the leaked temporary allocation,
propagates key/options errors and gates credential forwarding on the actual
per-device FOURWAY result. Firmware offload does not imply S2_SETKEY support;
legacy host-handshake key setup remains separate. Raw-only PSKs were initially
refused; the later explicit binary contract above permits them only for a
device advertising RAW_PSK, never by passphrase reinterpretation. Independent review identified empty-slot
clear compatibility and key callback return/pointer-lifetime defects; corrected
code tracks attempted WEP state, tolerates only unsupported empty clears with
no WEP history, and normalizes key callbacks to0/-1. A corrected clean full
RV32 GUI build passes (680316B), as do old summary-helper regressions.
Focused actual-source association/key/cipher models pass50 scenarios/339 checks each
O0/O2 ASan/UBSan on macOS and network-isolated Linux after a recorder portability
correction. SANA/DoIO/Exec are fakes, not a connection or link-up proof.
Independent non-author review accepts the bounded test scope; its noted cipher,
invalid-capability and non-NULL sequence coverage gaps were added and repeated.
Generic commitda74d7f23a is locally integrated on the port branch; no remote push.
No device association or credential deployment is implied.

Native SANA scan development (2026-10-10): S2_GETNETWORKS now queues one
owner-service scan, copies its optional bounded SSID filter, and publishes
one pool-owned result block only after cancellation/Close checks. Unit locks
are not held during the existing bounded radio scan. Active cancellation
drains through its completion, while queued cancellation removes its queue
node. Security projection preserves exact open/WPA-PSK/WPA2-PSK singleton
TKIP/CCMP tuples; ambiguous or unsupported profiles remain unknown protected.
C-string SSID tags cannot represent embedded NUL: such records and invalid
channels are omitted, never silently renamed. At most eight records are
retained by the existing private scan; no complete-radio inventory is claimed.
The device reports SECURITY_SUMMARY only, never FOURWAY. Corrected clean RV32
device build passes (59236B); independent source review finds no blocker.
Existing lifetime782 and datapath14902 model checks each O0/O2 ASan/UBSan
pass on macOS/Linux. Dedicated source-extracted scan models pass295 checks
each O0/O2 ASan/UBSan on macOS and network-isolated Linux after correcting
a zero-length NULL memcpy in the test fixture and enforcing sanitizer failure.
Radio, pool, semaphore and completion are deterministic fakes; BeginIO,
AbortIO/Close integration and actual scheduler execution are not qualified by
these scan-helper models.
No deployment, scan hardware acceptance, association or credentials.

WirelessManager security/offload integration (2026-10-10): generic changes
originated separately on pr/wireless-offload-summary from verified upstream
master2875ef32831c, in /Volumes/Dev/Build/aros-wlan-contract.qToy75. Generic
commit49972c6241 is retained on upstream/wireless-offload-summary and has
been integrated locally onto feature/esp32p4-wifi; it is not pushed upstream.
The explicit versioned per-device capability and pool-owned scan-summary
contract avoids reconstructing AP security IEs. Unknown, malformed or
ambiguous security must never select an open-network fallback. Initial
summary association is bounded to singleton WPA/RSN PSK TKIP/CCMP; SAE, EAP,
WPS and PMF require additional truthful metadata, not invented support.
Pure summary predicates pass88 scenarios/264 checks each O0/O2 ASan/UBSan
on macOS and isolated Linux; the explicit RV32 ABI fixture also compiles.
Driver and caller target syntax pass with existing warnings. The disjoint
pairwise/group capability refusal is corrected using separate role masks;
the legacy cipher mask is their union, not authority for either role.
A clean standalone RV32 WirelessManager build, including the existing MUI GUI,
now compiles and links with the current SMP Developer sysroot (678472B).
Source-extracted conversion/capability/selection-helper models now pass34
scenarios/63 checks each O0/O2 ASan/UBSan on macOS and isolated Linux.
Numeric fixture constants are extracted from production headers, and legacy
and summary-only profiles leave both offload role masks zero. Both generated
public headers were refreshed through compiler-includes and match the source.
Device queries, complete selection/association flow and deployment remain
unverified. This build is not association, DHCP
or hardware acceptance.

Persistent owner scan development (2026-10-10): private bridge ABI2 now adds
an owner-service-only default-config scan, using fresh persistent UIDs rather
than the single-use diagnostic IDs. Start/count/records and failure Stop share
one eight-second budget; callbacks cannot re-enter transport during the scan.
Only complete owned results are published; late success and failed cleanup
retire the session. Zero-count records retain the named N1E workaround; eight
records are a local cap, with explicit truncation. The Core1 resource and
matched device and owner-off resource clean-build; independent source review
accepts the corrected absolute-deadline guards. Dedicated source-extracted scan
models pass at O0/O2 with ASan/UBSan on macOS and isolated Linux (37,447
assertions each, including per-byte checks, not independent test cases).
Exchange, cancellation, Exec, clock and SDIO are mocked; outer owner-preflight
reservation settlement and actual concurrency/timing remain separate gates.
Follow-up development settles an exact reserved UID after a proven unsubmitted
exchange refusal only while the complete owner/FIFO generation remains healthy.
Previously the next service step retired the owner unnecessarily. This source
correction now passes actual-owner boundary models on macOS/Linux and a fresh
Core1 resource build; independent review finds no settlement blocker. Earlier build/model
identities above precede it.
ABI1 modules must not be mixed with ABI2. This private path is now connected
to the in-development S2_GETNETWORKS queue above; per-device WPA offload and
runtime qualification remain required. No deployment or hardware pass.

Private SANA-II bridge development (2026-10-10): the opt-in Core1 owner now
exposes nonblocking permanent-unit registration, synchronous borrowed-frame
delivery outside SDMMC ownership and bounded direct STA/0 Ethernet TX using
the existing FIFO/generation. An initial clean RV32 resource build succeeds;
independent bridge tests/review now pass. Source-extracted bridge models pass
202 checks each O0/O2 with ASan/UBSan, including minimum/maximum Ethernet
frames with negotiated checksum disabled/enabled, repeated on macOS and
network-isolated Linux. Exec/SDMMC are mocks, not runtime concurrency proof.
The separate SANA device now clean-builds for RV32 (53840B). Initial
cancellation, RX traversal and Close-drain defects are corrected; independent
source review and source-extracted lifecycle models pass782 checks each O0/O2
on macOS and isolated Linux with ASan/UBSan. Exec/scheduler are mocked.
AbortIO refuses a write after its lock-protected irreversible TX commitment;
Close still drains its references. Source-extracted RX/TX callback models now
pass14,902 checks each O0/O2 with ASan/UBSan, independently repeated on macOS
and network-isolated Linux. They cover cooked/raw frame boundaries, copy
failures, filter progress, multi-opener delivery and the actual pre-send abort
transition. Exec, hooks and radio remain mocks; actual runtime/SMP concurrency
and transport qualification remain open. The device
does not yet implement wireless association or advertise WPA offload.
Registration/MAC readiness is not association. No credentials, deployment,
DHCP, IP traffic or hardware acceptance is implied. Default-off builds retain
an unavailable bridge rather than silently starting the radio.

Networking update (2026-10-10): an exact historical official ESP-Hosted RPC
source match has been established for the JC1060 vendor project. See
[HOSTED-REFERENCE.md](network/HOSTED-REFERENCE.md) for pinned artifacts,
corrections, mandatory reset/timeout adaptations and the complete-host
comparison gates. The source match does not establish installed C6 binary
identity. The complete reference now passes one hardware MAC/scan gate;
native AROS Wi-Fi remains open.
Association source prerequisites are now recorded in HOSTED-REFERENCE.md:
mandatory STA threshold/PMF nesting, exact RPC/event IDs and a slave debug-log
privacy gate. No association codec/runtime or credentials are deployed.
Standalone typed STA SetConfig/Connect/Disconnect and owned link-event codecs
now pass239 self-checks and603 independent pinned protobuf-c comparisons each
at O0/O2 with ASan/UBSan in isolated Linux. Strict freestanding RV32 compilation
passes. These are pure codecs: no association runtime, association or IP
traffic is implied. The persistent pending transport already admits request
IDs282..284, but its allowlist is not a credential-safety gate or a runtime
association caller. The credential/privacy gate remains open.
The opt-in owner now decodes775/776 into an owned, generation-bound observation
ledger. UP_HINT is deliberately not CONNECT: the event UID is zero and cannot
identify the Connect request. The typed correlated294/550 current-AP query
now runs once per new hint revision in the opt-in owner, outside client/SDMMC
locks, with a one-second acceptance budget. Valid remote error status is not
transport loss; malformed/late replies fail closed. Events during a query
discard its result, and later events clear the stored snapshot. Matching owned
SSID/BSSID/channel/auth metadata remains diagnostic, not atomic proof of an
association attempt or permission to forward traffic. Default-off and core1
owner resource builds pass; no new firmware is deployed. Native S2_SETOPTIONS,
active-target confirmation, real link notifications and traffic gates remain
open alongside C6 privacy/recovery and association/DHCP acceptance.
Existing AROS wireless options, WirelessManager and BWFM's asynchronous
association worker provide the intended SANA-II integration reference. The ESP32 driver,
wireless command integration and DHCP/data acceptance remain unimplemented;
the basic asynchronous Ethernet device lifecycle is now implemented as
described above, without runtime acceptance.
Correction from runtime integration audit: BWFM is an asynchronous association
and request-lifetime reference, not a current WPA four-way offload contract.
WirelessManager's SANA2 get_capa does not set WPA_DRIVER_FLAGS_4WAY_HANDSHAKE;
its passphrase/PSK inputs are available only for drivers advertising that flag.
ESP-Hosted therefore needs explicit per-device offload capability negotiation,
without changing BWFM or legacy wireless devices to claim that capability.

Build-time WLAN defaults can use the existing AROSTCP interface database and
AutoRun/WirelessAutoRun/WirelessDevice preferences, without embedded SSIDs or
credentials. This is not yet staged in the P4 image. The standard package
startup uses NOGUI; user network selection requires an accessible GUI path.
WirelessManager's current Connect action updates in-memory configuration but
does not save it. Persistent first-use credentials therefore require explicit
user-preference storage; the read-only SYS/SD contract is not silently relaxed.

Persistent receive implementation is now build verified separately from the
installed one-shot probe: one bounded RX batch per call, immediate idle return,
generation-bound assembly retained between calls and a four-item owned RPC
inbox. Transport callbacks cannot dispatch events under shared-host ownership.
Queue overflow and ambiguous consumed bytes retire the matching epoch. This
control-plane receiver does not yet route Ethernet frames or run as an AROS
worker, and has not been deployed. Inbox134 and coupled RX364 checks each at
O0/O2 pass under Linux ASan/UBSan; a fresh149696B RV32 module links both APIs.
The bounded queue is local policy, not a negotiated guarantee that all peer
bursts fit. Overflow recovery and bus fairness still require runtime evidence.

OPEN-to-RX transfer now preserves the initial SERIAL suffix instead of
discarding it. An accepted handoff consumes a one-shot marker before parsing,
copies complete RPCs into the owned inbox and retains incomplete assembly;
it performs no second FIFO read or counter advance. Consumed malformed OPEN
input poisons the matching FIFO epoch. Legacy OPEN716 checks each O0/O2 pass
and a fresh155388B module builds. Independent handoff332 new and RX364
regression checks each O0/O2 pass under Linux ASan/UBSan, with actual exit0.
No production caller or hardware deployment is implied.

Asynchronous reserved RPC submit/correlation now builds separately from the
existing synchronous diagnostic: one TX leaves its identity outstanding;
owned events and unrelated replies cannot finish it. Exact UID/response-ID
matching completes envelope correlation only, not remote success. Private
serial scratch is wiped on every accepted-call exit. A fresh160680B module
links the API; independent427 checks each O0/O2 pass under Linux ASan/UBSan,
with a primary repeat and UID105/RX364/OPEN716/handoff332 regressions passing.
These are host-model tests, not hardware qualification. The
persistent AROS owner worker and SANA-II device were not implemented at that
foundation stage; their current build/model status is described above.

Ethernet RX routing now copies complete STA/0 frames into a separate four-item
owned queue, preserving interleaved fragmented SERIAL RPCs. Its14..1518-byte
bound is local policy, not a negotiated peer limit; unsupported AP/interface
profiles and nonzero throttle fail closed. Overflow or malformed consumed
input wipes/retires matching-generation state. Dispatch remains outside host
ownership. Preserving OPEN still accepts SERIAL suffixes only. A fresh164788B
module links the new frame queue; primary frames136/RX364/OPEN716/handoff332/
pending427 regressions each O0/O2 pass under Linux ASan/UBSan. Independent
coupled Ethernet285 checks each O0/O2 pass, repeated by the primary in a
network-isolated snapshot with actual exit0; source review finds no blockers.
This has not been
deployed. That foundation stage did not implement Ethernet TX, an AROS owner
or SANA-II; the current private bridge/device adds those source paths without
runtime acceptance.

The actual owner integration is now in development behind the default-off
P4_WIFI_HOSTED_OWNER switch. Its process placement is configurable through
P4_WIFI_OWNER_CPU (default1, comparison0), using explicit affinity before
radio access. Corrected clean CPU0/CPU1 modules now link (194792/195008B), and
strict freestanding RV32 owner compilation passes without libc imports.
Independent coupled owner6723 checks each O0/O2 pass under Linux ASan/UBSan,
repeated by the primary with actual exit0; source review confirms the OPEN
partial-age and TX-to-RX FIFO-direction fixes. These are fake-adapter tests.
There is no deployment, runtime or measured performance claim;
WirelessManager and legacy GUI placement remain unchanged. The opt-in caller
currently consumes event metadata; its private bridge can deliver Ethernet
frames to the separately built SANA-II consumer. Neither module is deployed,
and no working network interface is claimed.

The native control foundation now supports bounded sequential Init/SetMode/
Start/Stop/Deinit/GetMAC envelopes on one existing FIFO ledger, with owned
response bodies and exact UID/operation correlation. Linux O0/O2 sanitizer
models pass3,529 control,2,614 TX,1,245,361 FIFO and716 OPEN checks each.
Unread acknowledged RX failures invalidate the session; stale-generation calls
cannot alter a replacement ledger. Standalone RV32 objects compile without
libc imports. The full typed WiFiInit body and SetMode/Start/Stop/Deinit status
codecs now pass149 O0/O2 Linux sanitizer checks each and standalone RV32
compilation. The working IDF6 host's20 configuration values are documented in
HOSTED-REFERENCE.md; they are derived source semantics, not a packet capture or
installed C6 binary identity. An independent pinned protobuf-c oracle now
passes810 O0/O2 sanitizer checks each. Runtime Init/STA/Start/GetMAC plus
conservative Stop/Deinit is integrated into the opt-in fresh-epoch gate and
the new42-member package passes loader/capacity checks. Sequencing models
pass551 O0/O2 sanitizer checks each. The initialization candidate is now
deployed at0x820000 and independently read back byte-identically after two
matching full-erase-span backups. One coordinated JC1060 startup now reports
successful native Init/STA/Start/GetMAC/Stop/Deinit and MAC10:bd:a3:9b:bc:34.
The latest pre-armed60.181s repeat again contains an HP watchdog reset,
despite successful native initialization/MAC and cleanup afterward. USB removal
and arrival were observed; the first serial chunk arrived6.525s after open.
The initial ROM prefix is incomplete and serial-open influence is unresolved.
Thus clean cold-start acceptance
and native WLAN remain open. The original baseline is recoverable.

Persistent caller identity support now builds: one reserved RPC at a time,
monotonic non-reused UIDs even across host-generation changes, permanent
exhaustion rather than wrap, and exact stale-completion refusal. The new
reserved-exchange API is linked into a fresh138984B module, but existing
single-use diagnostic sequences still use fixed UIDs and no persistent worker
calls the API. Ledger105 and legacy control3529 O0/O2 sanitizer checks pass;
independent reserved-exchange qualification passes300 O0/O2 sanitized checks
each against the real control implementation and fake transport. This does
not add event delivery or idle RX. Losing the ledger on a P4 restart is not
safe proof that a retained C6 has forgotten earlier UIDs.

A generation-bound serial reassembly helper now passes1,318 checks
each at O0/O2 with ASan/UBSan and freestanding RV32 compilation. It copies
partial replies into owned memory and checks the pinned slave's per-fragment
sequence increments. It is now linked into the control path, retaining partial
replies across batches under the same epoch and aggregate deadline. Terminal
partial failures wipe assembly and invalidate only the matching FIFO epoch;
whole-batch validation continues after a response match. Responses are owned
up to4096 protobuf bytes; requests remain capped at256 bytes. The fresh113900B
module builds, but this integration is not in the deployed initialization
package. Native scan and WLAN acceptance remain open.

Typed scan requests/responses now pass230 checks each O0/O2 with ASan/UBSan
and standalone freestanding RV32 compilation. Default-config ScanStart accepts
blocking or nonblocking mode; owned results retain all pinned AP/country/HE
fields with binary string lengths and a local eight-record bound. Independent
pinned protobuf-c oracle qualification passes1068 checks each O0/O2 sanitized.
The opt-in native scan runtime now builds after successful initialization/MAC,
with bounded blocking scan, count, owned records and conservative failure
cleanup. Its control regression passes3529 checks each O0/O2; the dedicated
sequence models pass537 checks each O0/O2 with ASan/UBSan. These use actual
scan/RPC codecs with modeled control exchanges, not physical hardware.
ScanStart alone may poll500 times but remains
under the same8s aggregate deadline; other RPCs retain100 polls. The135284B
scan-enabled module is now in a separately prepared3823840B package:42/42
loader checks and the4063232B size limit pass;41 non-WiFi records remain
byte-identical to the installed initialization package. It is not deployed
and is not a hardware scan pass. The larger3825664B erase span requires two
new matching full-span backups before any eventual write; the existing shorter
initialization backup does not cover it.

Native diagnostic log validation is available in
`tools/check-native-wifi-log.py`. It checks one completed passive capture,
typed initialization/MAC/optional scan results and restored stable shared-host
registers. Independent positive/negative fixtures pass16 tests; this is not a
hardware pass or proof of physical power continuity.

Fabian confirmed sole USB-C power and coordinated disconnect/reconnection.
The first armed reader timed out before arrival; a subsequent45.122s passive
capture completed successfully but received0 bytes. This is an inconclusive
retrieval, not initialized-MAC acceptance or proof of a firmware failure.
The coordinated repeat received16340 bytes over60.112s with actual exit0 and
status=ok. All five WLAN operations report status/error0; MAC matches and all
six stable shared-host registers are restored. However, the initial
HP_SYS_HP_WDT_RESET and truncated ROM banner prevent a clean-boot claim;
port opening can disturb startup. The strict checker rejects this capture.
Its cleanup expectation is corrected to the source enum CLEANUP_OK=1 and
all14 flags;16 regression tests pass. No further reset is captured. The USB
observer was intentionally interrupted (exit130), not qualified as a pass.
No capture is running. Next: resolve startup/reset continuity before scan
deployment. JC1060 WIFI_RESET_GPIO=-1 provides no established independent C6
reset alternative. Installed initialization and prepared undeployed scan
artifacts remain unchanged; native WLAN is incomplete.
The passive reader's optional timing sidecar now also records port-open
begin/end, separating open latency from byte waiting. Ten host-only tests
pass; this instrumentation is not a new hardware result or a reset fix.
After another reconnection confirmation, a single late passive retrieval
completed45.114s with exit0/status=ok but0 bytes. The new sidecar measures
1.425ms serial-open latency in that retrieval only. No arrival-time observer
or armed early reader was running, so this does not qualify the cold boot,
explain the earlier6.729s gap or establish a firmware fault. No reset or flash
was requested. A fresh coordinated, pre-armed startup capture is still needed.
The subsequent user-confirmed reconnection also lacked armed early coverage:
one45.118s passive capture exits0/status=ok with0 bytes; diagnostic validation
exits1. USB identity matches the JC1060, but boot/WLAN continuity remains
unqualified. No reset, flash or silent repeat was performed.
Fabian has now reconfirmed readiness and sole USB-C power. A30-minute node
observer and removal-then-arrival wait are armed before physical instructions;
the latter opens the serial port once, only after observing removal/rearrival,
for a60-second passive capture. Outcome is pending; installed firmware unchanged.

The complete-host build now links with IDF6.0.1 after explicit dependency,
DMA-allocation and legacy reserved-field adaptations. Image-header inspection
also exposed and corrected the default P4>=3.x target; the corrected build
targets1.0..1.99. The reference front end and compatibility patch are retained
under `tools/hosted-reference/`. A bounded MAC/scan candidate now removes implicit
initialization, guards absent reset GPIO, uses ten-/forty-second connection/app
deadlines and panic-halt behavior, and corrects scan result validation/count
publication, and preserves the existing bus mutex after worker startup.
Its IDF6 link, independent review and actual-source startup/scan/app models
pass. Its first headless deployment fails before application entry: the
generated 400MHz setting is unsupported on revision1.3. Defaults now select
360MHz; a generated-configuration checker and corrected build pass.
In that first failed attempt exact full-span AROS rollback/readback matches, but the return boot
faults in ROM at address0x50091000 and watchdog-resets. Independent bootloader/
partition readback matches the originals. A read-only snapshot finds Crypto
selecting240MHz while that parent is gated off. A single masked parent-clock
enable restores a complete30.948s headless AROS return (PSRAM/both harts/modules/
graphics updates). This manual recovery workaround is not a permanent reset
fix or a physical-cold/visual gate; N1R tracks its uncompensated removal.
The subsequent corrected360MHz hardware run completes MAC/scan in4.085s:
slave chip13 (0x0d, C6)/capabilities0x0d, MAC10:bd:a3:9b:bc:34,13 APs/eight records.
Complete45.822s capture has no further reset/fault. Exact full-span rollback
and complete30.925s baseline return pass without another manual clock write.
This is one official-host reference gate, not native association, frames,
SANA-II/shared-SD/recovery or installed binary lineage. Empty-scan behavior was
not exercised (13 APs); N1E remains open. Board runs baseline AROS.
IDF5.4.3 is an optional isolated comparison environment, not a port downgrade.

The native bootstrap candidate now follows CMD52 RES with identification and
a20ms settling interval after successful CMD0. P4_WIFI_SDIO_PROBE=1 therefore
now resets the quiesced slave's SDIO state; it is no longer a read-only register
probe. Only classified response timeout/CRC of the exact reset command may
continue, with reset_acknowledged=0; complete identification must still pass.
Generic R5/card/bus/DMA errors, generation changes and budget expiry remain
fatal. One aggregate budget covers reset and identification. Strict O0/O2
sanitizer models pass6,358 checks each; fresh RV32 bootstrap-only and setup
module builds pass. The bootstrap-only package now passes one complete40.928s
JC1060 warm-start capture: reset attempted1/ack1/status1, identification
ok1/stage8, two functions, RCA1,394088Hz and all24 controller snapshots.
Only the Wi-Fi member changes; the other41 records remain byte-identical.
This does not qualify repeated recovery, the timeout/CRC exception on silicon,
native OPEN/RPC/association/frames/SANA-II or SD coexistence.
The entire BSP erase span is restored/readback verified byte-identically;
the complete30.934s baseline return passes PSRAM, both harts, watchdog,
41-module loading, SD enumeration and graphics-update dispatch without
another captured reset/fault. No visible-frame acceptance is inferred.
Extracted runtime adapter models pass41 cases/501 checks each O0/O2 with
sanitizers; these are not real controller or elapsed-time measurements.
The separately selected P4_WIFI_SDIO_RESET_QUERY=1 historical diagnostic stays
fail-stop and does not enable function setup.

The separate setup-enabled package now passes one complete41.028s warm gate:
function1 enabled/read back, ready after one poll,512-byte block size verified,
then saved IO_ENABLE/FBR1 block bytes restored and verified (stage7/error0).
The full3735552-byte BSP erase span is restored byte-identically; complete
30.885s baseline return passes both harts/PSRAM/watchdog/SD/graphics dispatch
without an additional captured fault. Original/restored register bytes are
not printed individually; success is the checked runtime result, not a raw
register dump. No OPEN/IRQ/FIFO/RPC or WLAN acceptance follows from this gate.

Native INIT/FIFO now link into an opt-in bounded OPEN/INIT diagnostic
(`P4_WIFI_HOSTED_INIT=1`, setup required), host tested and RV32 build verified,
not deployed. OPEN tests pass716 checks each O0/O2 with sanitizers; extracted
CMD53 adapters pass15 cases/160 checks and wire regressions157590 each.
The first linked candidate was rejected by the loader audit for libc startup
references to unplaced .eh_frame; explicit volatile-byte initialization/copy
remove that dependency. The corrected42-member package passes the loader audit
and capacity gate. Fn0 state is snapshotted/restored; OPEN is followed by
bounded polling and one512-byte-rounded FIFO receive, then explicit CLOSE.
Ambiguous FIFO failure poisons the session without retry/counter advance.
Following outer-valid serial frames may be consumed/discarded opaquely;
this is not RPC acceptance. The pinned C6 application emits INIT only once,
not on later OPEN or SDIO reset. A zero pre-OPEN counter is necessary but
does not establish a fresh slave epoch. Live first-INIT acceptance therefore
needs a fresh C6 startup; warm recovery, reply/RPC, association, traffic and
SANA-II remain open. Board remains on the last verified original baseline.

Native RPC envelope parsing/encoding was first host tested and standalone RV32
compile verified, before the opt-in MAC linkage below; it is not deployed.
It enforces matching type/operation/
oneof, nonzero request/response UIDs, bounded unknown-field skipping and
transactional refusal. Except for the new typed GetMAC body, operation bodies
remain opaque. Tests pass44278
checks each O0/O2 with sanitizers; a separate host-only oracle compiles the
pinned official protobuf-c sources and passes600 checks/40 messages each.
This proves envelope interoperability, not runtime UID correlation or WLAN.
The serial TLV codec is now freestanding without libc imports (116 checks each).
Source audit also establishes the missing SDIO host configuration reply after
INIT and distinguishes the twenty-buffer hardware token pool from software
queue depth. A bounded opt-in TX startup reply is now integrated as described
below; live credit/config acceptance remains unverified.
WiFiInit must supply the complete validated configuration: the reference slave
overwrites its defaults with decoded scalar values, so an empty/default body
is not a safe initialization shortcut. Fresh cold-start synchronization is
pending; no hardware state changed during this implementation.

The FIFO planner now enforces its documented output/state non-overlap rule
before checking the generation, preventing a refused aliased plan from
corrupting transport state. Sanitized tests pass1245355 checks each O0/O2,
including preceding/partial overlap, unchanged state and exact adjacency;
OPEN regressions remain716 each. Standalone RV32 compilation passes, but the
previous OPEN candidate has not been rebuilt or deployed with this correction.

The C6 SDIO host-configuration frame encoder is now host tested and standalone
RV32 compile verified. It emits the pinned17-byte configuration payload in a
29-byte private frame (packet_type0), selecting checksum from INIT bit7,
refusing incompatible/missing/raw-throughput profiles and overlapping outputs.
Literal checksum-off/on vectors, bounds and refusal tests pass120243 checks
each O0/O2 sanitized. The explicit `P4_WIFI_HOSTED_REPLY=1` diagnostic now
connects this encoder to a persistent FIFO transmitter after accepted INIT,
under the same eight-second operation budget and separate two-second cleanup.
The source-pinned C6 pool of20 is explicit, not inferred from INIT queues;
installed binary identity and live token/config acceptance remain unproven.
TX tests pass2586 checks each O0/O2 sanitized. Actual-source controller adapter
tests pass36 cases/233 checks each, including expired budget, lost ownership,
generation/peer-clock changes and malformed motor results. Only complete
motor/DMA success commits credits; uncertain started writes poison the session
and are never retried. The fresh reply-enabled RV32 module is76,024 bytes;
the3,764,580-byte package passes42/42 loader audits and the4,063,232-byte capacity
gate, with all41other members byte-identical. This includes the FIFO alias
correction. No candidate was deployed or native frame sent. The new3,768,320-byte
erase span requires an independent full-span double backup before a write.
No additional hardware operation occurred during that implementation.

The opt-in `P4_WIFI_HOSTED_MAC=1` diagnostic now links one GetMAC(STA) request
after the startup reply, using the same FIFO ledger and aggregate deadline.
It requires the reply/INIT/setup gates and a confirmed fresh C6 startup.
Response type, operation513 and UID must match; events and unrelated responses
are counted, not treated as success. Full RX batches are validated, including
frames following a match; malformed/ambiguous consumed RX poisons the ledger.
Only a status-zero six-byte nonzero unicast MAC succeeds. UID1 is used once;
timeout does not permit reuse within the epoch. Persistent request allocation,
fragmented RPC reception and the WLAN data-plane client remain open.
Sanitized O0/O2 tests pass154 codec,1250 pinned protobuf-c oracle and1690
control-gate checks each; actual controller adapters remain233 checks each.
The fresh93,756-byte module and3,782,312-byte package pass42/42 loader checks
and capacity, with all41other members unchanged. At that build stage it was not
flashed. The subsequent native hardware gate below is not WLAN acceptance.
Its3,784,704-byte erase span was independently read twice before deployment.
Fabian confirmed USB-C disconnection and reconnection. The legacy CMD52 fixture
is repaired using extracted production definitions and passes28cases/193checks
each O0/O2 sanitized; earlier failed attempts remain recorded.

The first fresh-C6 native hardware gate now completes45.923s: OPEN/INIT
receives chip0x0d/caps0x0d, the configuration frame is sent once, and a matched
GetMAC reply returns status12289 (0x3001, ESP_ERR_WIFI_NOT_INIT). This is a
diagnostic ordering failure: the native path omitted WiFiInit/SetMode/Start,
which the working reference performs before GetMAC. It proves one transport
request/response, not a valid MAC or WLAN readiness. No retry or weakened
validation follows. CLOSE/Fn0 and Fn1 restoration succeed, ledger unpoisoned;
cleanup error1 is the enum's OK value. No additional captured reset/fault.
Exact full-span baseline restoration/readback pass; complete30.802s return
capture passes PSRAM/both harts/watchdog/SD/graphics updates/Touch heartbeat,
without another captured fault. No visible-desktop/touch gate is inferred.
Next implement the complete reference initialization sequence, with all20
WiFiInit scalar values validated, then repeat the native MAC/scan gate.

JC1060 Wi-Fi implementation has started
([ROADMAP.md](ROADMAP.md#track-n-networking), N0-N3). Pure CMD52/CMD53
construction and a byte-oriented ESP-Hosted frame codec in `network/` have
host-test coverage and RV32 compilation; they are not linked into the
production firmware and are not a working Wi-Fi
driver. SD storage and the C6 share one SDMMC controller. The development
branch now adds a permanent ownership resource, reset generations and
transaction-scoped SD hooks with extracted-source sanitizer tests and successful
RV32 resource/device builds in an isolated JC1060-v2 build directory. The shared
clock/reset control is now implemented in the resource with peer-preserving
two-slot changes, host-tested and RV32 build verified. Command/DMA engine
extraction is in progress: ABI3 now moves the single descriptor chain, bounce
buffer, cache handoffs and FIFO/IDMAC reset into the resource. The adapted
resource and read-only SD client build; extracted-source DMA tests pass at
O0/O2 with sanitizers (15 cases, 2,253,671 checks each), with clock and
ownership regressions passing too. These are host models, not hardware evidence.
A shared command-engine candidate now adds explicit slot/direction,
staged RX/TX, card-response masks, error-first completion and bounded waits;
its RV32 build and independent source review pass. Independently repeated
extracted-source tests pass at O0/O2 with ASan/UBSan (20 cases, 2,001,069
checks each). It was exposed through private resource ABI5 (212 bytes on
RV32), and SD refuses a missing callback or mismatched version/size.
The diagnostic migration used ABI6 (same host size; changed command
request/result layout), host tested and RV32 build verified. All SD builds
invoke it for all commands; radio integration remains open.
Modeled MMIO/DMA callbacks
are not hardware evidence.
Global controller programming now moves into the shared reset path; SD joins
an initialized host without resetting its peer and changes only its own reset
and width bits. A sticky first-initialization marker prevents a previous
clock/reset fault from being mistaken for first boot. Matched RV32 builds
pass; independent source review and independently repeated extracted-source
models pass (clock/reset:18 cases/292 checks; SD ownership/init:25 cases/400,338
checks each O0/O2 with ASan/UBSan). These do not verify silicon behavior.
The production SD adapter, including data reads, is host tested and RV32 build
verified (35 cases/592 checks each O0/O2 with ASan/UBSan). CMD12 retains client-side
CMD13 TRAN/ready confirmation before publishing its response. The legacy
diagnostic loop is now removed: all builds use the shared motor, with explicit
per-request command/data CRC or timeout injection and bounded observation hooks.
Independent review and primary repeated sanitizer tests pass: command engine
26 cases/2,001,566 checks each O0/O2; SD adapter35 cases/629 checks each O0/O2,
plus all three trace-off fault modes. The12-sample trace is best-effort and may
omit stop/finish milestones; it is not recovery acceptance evidence. Hardware
regression and open-unit recovery remain open. The accepted production branch
through acd62ff237 is now merged locally into this Wi-Fi branch; its boot-budget,
quiet-profile and qualification host regressions pass. A complete integrated
RV32 core/package build and hardware regression still precede deployment.
The integrated ad7e4171f6 core now links successfully with the production XIP
layout (189,984 bytes, SHA-256 bbc265fad7b4e18a15b67fcea7dcf029edb671bc88387eac91050b837f92cf68).
Complete BSP assembly, checksize and loader audit now pass: 3,694,032 bytes,
41 accepted ELF members, SHA-256
860fa1f461aaa0758852e3fb57eab9d558feb9ff4bd118ab34f595fec2f5cf09.
The first integrated pair fails the headless hardware boot: the shared resource
accidentally pulls C-runtime startup with a relocation to the loader-discarded
`.eh_frame` section. The accepted production pair has been restored and verified.
A correction removes libc memory-operation dependencies from the early resource;
command/DMA sanitizer regressions and the RV32 resource build pass. The corrected
package passes the strengthened relocation-placement audit and loads all 41
modules on hardware. Removing selfinit corrects missing Exec registration:
SD now accepts the shared host and enumerates the 121,942 MB card on hardware.
The first corrected capture has a pre-AROS ROM watchdog reset; the 45-second
repeat completes without another reset/trap/alert. Its cause remains open.
This is not SD data-integrity/shared-WLAN qualification.
Installed C6
communication remains pending. This is not a working WLAN driver.
An opt-in `P4_WIFI_SDIO_PROBE=1` package runner is build verified: an
AFTERDOS child process joins the healthy shared host, uses a ten-second
aggregate budget and reads slot1 SDIO identification/CCCR/FBR registers.
It never resets the global controller, enables functions/IRQs or replaces C6
firmware. Independent runtime review found no concrete defect; strict C99
ASan/UBSan protocol tests pass 4,597 checks each at O0/O2. The 42-member
package passes checksize and the loader audit. The first complete headless
hardware run fails at initial CMD5 (no usable OCR/functions). Runtime console
saturation loses detailed status lines; the precise cause remains unknown.
The prior package is restored with matching independent full-range readback.
None of these results demonstrates WLAN.
Paced probe diagnostics are now build verified: reporting waits until host
ownership is released, then prints short status snapshots between DOS delays.
Their purpose is to resolve the initial CMD5 failure, not change the bus setup.
The paced hardware run now identifies response-timeout bit8 (raw0x00010100,
shared command status ERROR, IDMAC status0); no additional reset was observed.
Electrical/startup cause remains unresolved, not firmware incompatibility proven.
Read-only controller/matrix snapshots before and after the identification
sequence now confirm expected slot1 clock/divider/mux and CMD/D0/CLK routing
on silicon. CMD5 still times out. Independent IDF comparison identifies the
missing no-CD-pin constant-low card-detect route; fixing it remains open,
and no causality between that omission and the timeout is established.
The corrected no-CD-pin route now reads back0xbe, with CDETECT0 on hardware.
CMD5 still times out with the same raw status. This correction did not resolve
the observed failure. An independent ESP-IDF-only reference probe is now
implemented under `tools/idf-sdio-reference`: slot1, one-bit,400kHz,
CMD0 and at most three CMD5(arg0) attempts through the official driver.
It does not initialize a card, enable functions or change C6 firmware.
Its silicon-specific build passes (P4 revisions1.0-1.99). IDF startup does
not quiesce AROS-retained display DMA. The opt-in
`P4_REFERENCE_QUIESCE_PARK=1` AROS handover diagnostic now passes strict host
tests, RV32 image validation and a complete30.896s JC1060 capture:31 stopped
readbacks, READY only after ten timed intervals, watchdog alive and no further
reset. It parks before PSRAM/Exec and forces only the bridge register clock.
This is not a physical cold boot or an all-DMA qualification.
The subsequent IDF reference run fails before CMD0 at an official-driver
clock-frequency assertion; CMD5 was never reached. It cannot compare native
CMD5 behavior or identify C6 firmware. The original entire core erase span is
restored and independently verified. Two restored AROS boots recover32MB PSRAM
and both harts, but fail DSI PLL lock; graphics restoration is not accepted.
Those failed returns remain recorded; the parent-clock correction below
restores headless graphics startup. No WLAN qualification follows; C6 firmware
remains unchanged.
The IDF API-order workaround now builds and passes image validation
(189,168 bytes, SHA-25691512905cb18a923e5b958cb9c61df833d05cb5ea74dc189402ab8e1bac25a49);
it is not deployed. It lets CMD0 initialize the probing clock and checks the
reported frequency before CMD5. An opt-in read-only trace measures the parent
F20M gate OFF with the correct /24 divider. AROS now enables that parent before
the local DSI gates, matching the official rev1 driver. The190016-byte corrected
core dea08a121687a9b7f6f55758761dea0cd10033d030cc2585ccce716750de59a0
is written/readback verified; complete31.008s headless capture recovers PLL lock,
32MB PSRAM, both harts,41 modules and accepted graphics updates, without a
further reset/trap/alert. This is one headless return, not a visual, sustained
or physical-cold gate. The trace is before the correction and may still report
the prior gate value. Next: corrected bounded IDF comparison with safe
quiescence and verified AROS return, then native C6 identification.
The corrected IDF build-v2 now has a complete15.952s hardware capture:
CMD0 succeeds and actual probing clock is400kHz, but all three CMD5 requests
report peripheral ESP_ERR_TIMEOUT despite API dispatch returning ESP_OK.
The zero OCRs printed by that older helper are invalid, not received responses.
The helper now checks both error channels; strict sanitized extracted-helper
tests and fresh build-v3 pass, but v3 is not deployed. Original corrected AROS
core erase span restored/readback verified, complete30.917s return recovers
PLL/PSRAM/both harts/graphics updates without another fault. Native and IDF
both fail to obtain a C6 reply in retained-power tests. Installed firmware,
C6 startup and actual variant wiring remain unverified; a fresh coordinated
rail-off is requested because a P4 reset does not reset C6. WLAN remains open.
Pinned vendor stock C6 artifact is now inspected offline: merged image
1,146,768B SHA-2568b16a042713bca67d64a1d7af6a296432c1c5f8116fe015ae24acfca3a66fec0,
valid extracted app `network_adapter`, version `54aafd0-dirty`, built
2024-12-26 with IDF5.4-dev. It contains ESP-Hosted-MCU/SDIO slave code;
this is not an installed-firmware identity or a successful startup. The
reference example selects SDIO streaming and disables checksum, whereas the
independent codec already accepts either negotiated checksum mode. Do not
infer the stock binary's exact configuration from that separate sdkconfig.
After Fabian confirmed complete power disconnection and reconnection, the
corrected IDF v3 probe now receives three valid CMD5 R4 responses:0x20ffff00,
two SDIO functions, OCR0xffff00, ready0. Both error channels are ESP_OK at
400kHz. This proves a responding SDIO slave in this post-disconnection run,
not voltage negotiation, installed firmware identity or working WLAN.
The previous retained-power timeouts remain unexplained; no C6 startup log
or electrical rail measurement was captured. Exact AROS core-span rollback
and a complete30.933s headless return pass PLL/PSRAM/both harts/graphics startup.
Next is the native shared-host identification gate, followed by ESP-Hosted
compatibility negotiation; do not require another blind physical reset.
The native AROS identification now also passes on the retained C6 state after
that cycle: complete CMD5 negotiation/CMD3/CMD7/CCCR/FBR1 reads, two functions,
RCA1,394088Hz, no command error and unchanged slot0 clock configuration.
CCCR IO_ENABLE0/IO_READY2 are separate observed values, not WLAN readiness.
The probe package adds only one module to the byte-identical41-member baseline.
Exact baseline rollback and complete31.057s headless return pass. A regression
fixture for this wider OCR/register snapshot passes4730 sanitized checks each
O0/O2. This is one identification gate, not SD data integrity/coexistence,
installed firmware identity, sustained recovery or WLAN traffic. Next N1:
bounded function setup and ESP-Hosted capability negotiation.
The pure function-1 setup helper is now host tested and RV32 compile verified
(3997 checks each O0/O2 with ASan/UBSan). It snapshots IO_ENABLE and FBR1,
preserves peer enable bits, verifies IO_ENABLE independently of IO_READY,
bounds readiness polling and verifies the512-byte block size. Failure retains
cleanup metadata even if a failed write may have reached the card. It is not
linked into the runtime and does not restore card state itself. No new hardware
result follows. Before OPEN, resolve the target-specific interrupt address:
C6 IDF5.4 documentation says0x8d, while an unmatched legacy FG host uses0x8c.
The pinned slave sends its startup event only after OPEN; waiting for that
event before OPEN would not initialize this reference. Installed compatibility,
runtime cleanup/transport/RPC and WLAN traffic remain open.
An opt-in `P4_WIFI_SDIO_SETUP=1` runtime candidate now adapts CMD52 through
the shared command engine. It performs setup only after identification and
attempts bounded restoration under the same host ownership. Identification
and setup share8s; cleanup has a separate2s reserve. CMD52 writes have no
data-transfer phase. Clean RV32 module builds pass with setup enabled and
disabled; extracted actual CMD52 adapter tests pass21 cases/165 checks and
restoration tests602 checks each O0/O2 with sanitizers. Its first headless
deployment failed before setup: after a P4 USB reset, CMD5 timed out again
(stage2, command5, error3/status3). Setup attempted0 and mutated0; real setup
and card-state restoration are therefore unverified, not passed. The earlier
post-power-cycle identification pass does not establish warm reidentification.
OPEN/IRQ/RPC and C6 firmware writes remain absent.
The entire3,731,456-byte BSP erase span was restored and independently
read back byte-identically; the complete30.981s baseline return boot succeeds.
The board is running the original41-member package without a Wi-Fi probe.
The C6-capable ESSL reference and explicit C6 TRM address mapping now support
Function1 byte0x8c; the protocol page's0x8d is an apparent documentation
inconsistency, not a silicon test result. No OPEN has been sent.
The warm-reset investigation identifies a concrete missing reference step:
IDF5.4.3 sends CMD52 Function0 CCCR0x06=0x08 (SDIO reset) before CMD0/CMD5;
our current probe starts at CMD0. A bounded opt-in reset-before-identification
diagnostic is build verified: P4_WIFI_SDIO_RESET_QUERY=1 issues exactly one
CCCR RES write, CMD0 and CMD5(0), with fail-stop and pre/post budget checks.
It excludes function setup at compile time and never continues to selection,
IRQ/FIFO/OPEN. Strict sanitized tests pass1168 checks each O0/O2. Its first
hardware gate stops at CMD52 with CARD_ERROR (status4, raw0x10004), not a
response timeout; CMD0/CMD5 were not sent. Reset attempted1/acknowledged0
does not prove the write failed to reach the slave. The entire BSP erase span
was restored with independent byte-identical readback and a complete31.003s
baseline return. A reset-query-only rejected-R5 observer is now host tested
and build verified (22 cases/72 checks each O0/O2 ASan/UBSan): it reads RESP0
under existing command ownership without weakening response rejection.
Its separate hardware run instead reports a CMD52 response timeout
(status3/raw0x10100), so rejected-R5 valid0 and no response can be decoded.
The baseline is again restored/readback verified; complete30.869s return passes.
Neither result establishes reset acknowledgement or the timeout root cause;
slave-side hardware-FSM recovery may also be needed, and no host-controlled
C6 reset GPIO has been established for this board.
The recovery foundation now separates card identification into an unpublished
CID/CSD/RCA/geometry snapshot. Initial registration stages that snapshot before
allocating/publishing a unit and caches its media identity. This shared-device
change is host tested and RV32 build verified: extracted identification and
registration tests pass 41 cases/27,369 checks each at O0/O2 with ASan/UBSan.
Those scripted host tests do not establish hardware or concurrent-open behavior;
automatic open-unit recovery is not enabled yet.
Strict card activation is host tested and RV32 build verified: required
command/status and configuration failures refuse activation. Extracted-source
activation tests pass 26 cases/140 checks and SD/MMC helper tests 16 cases/37
checks each at O0/O2 with ASan/UBSan. This is not yet a same-media recovery
path or a hardware WLAN driver; real concurrent publication remains unverified.
A same-media recovery candidate now stages protocol state without replacing
the mounted Exec unit and compares cached CID/CSD/capacity/geometry before
publication. Open/Close and publication use a matching short task-only SMP
metadata guard. This candidate is host tested and RV32 build verified:
recovery/Open/Close tests exercise extracted production code in modeled SMP and
non-SMP configurations. They do not prove real hart interleavings. At that
candidate stage backend preparation/finish hooks remained unbound; automatic
BusTask dispatch was still open at that stage; hardware recovery remains open.
The P4 client now tracks the live protocol RCA privately, clearing it on host
generation changes/reset/CMD0 and accepting CMD3 only after a successful,
valid SD R6 or MMC R1 response. CMD12/CMD13 confirmation no longer reads the
mounted unit's cached RCA; card-error bits also refuse TRAN confirmation.
This recovery prerequisite is host tested and RV32 build verified (adapter:
44 cases/825 checks each O0/O2 ASan/UBSan). CMD13 confirmation is source
reviewed but still stubbed in that model. It is not enabled recovery or WLAN
acceptance.
A shared aggregate-budget candidate now uses private resource ABI7: the owner
starts one bounded operation budget; command, clock and DMA polling consult
that same start time instead of restarting a duration for each phase. Nested
ownership preserves it and final release clears it. It is host tested and
RV32 build verified (ownership29/400,435; command33/2,001,779;
clock25/367; DMA22/2,253,994 cases/checks each O0/O2 ASan/UBSan).
At that budget stage recovery dispatch was still disabled; WLAN remains
disabled. This is not a hard real-time claim
for scheduler latency or kernel cache callbacks.
P4 task-context recovery hooks are now bound to the generic staged operation
and are host tested and RV32 build verified (40 cases/401,034 checks each
O0/O2 with ASan/UBSan). They establish one ten-second budget, reset a faulted
controller explicitly, join a healthy peer without resetting it, and refuse
publication after expiry, generation change or failed identity/configuration.
Failure leaves the private live RCA unavailable. Automatic BusTask dispatch
is integrated as a candidate below, not hardware accepted. Initial startup
now initializes the host before the
ready-guarded power callback; the previous order skipped power programming.
This is a development-branch source correction, not new cold-boot evidence.
Automatic SD BusTask recovery is now host tested and RV32 build verified:
the extracted dispatch model passes9 cases/822 checks each O0/O2 ASan/UBSan;
sector-read
requests check the mounted identity before dispatch. A failed
attempt is latched per unit until an explicit TaskSig wake rearms it; queued
requests cannot start repeated ten-second attempts. Failed IO is never replayed.
Control queries, unsupported direct SCSI and write-protection replies retain
their original path. The latch is reserved/published under UnitLock and also
blocks ordinary StartUnit after failure, including a close/reopen cycle.
Radio-triggered notification and real combined SD/WLAN operation remain open.
The independently implemented ESP-Hosted serial TLV codec now validates
RPCRsp/RPCEvt envelopes (116 checks each O0/O2 with ASan/UBSan and standalone
RV32 compilation). It does not parse protobuf, reassemble fragments or execute
RPC; its 4096-byte payload cap is defensive local policy, not a vendor limit.
Bounded RX serial reassembly is now host tested and RV32 compile verified
(312 checks each O0/O2 with sanitizers). It publishes only complete valid TLVs,
uses a fixed4108-byte buffer and checks fragment sequence and an aggregate
time limit. The pinned C6 sender advances sequence numbers per fragment,
whereas its reverse receive path expects matching numbers; RX and TX policy
must not be conflated. Other interfaces may appear between serial fragments.
No runtime transport or installed-firmware compatibility is verified.
The installed firmware is unchanged. The older
`cheops/JC1060P470C_I_W` package is Fabian's selected WLAN reference:
the profile declares P4-side slot1 CLK18/CMD19/D0-3=14-17 without enabling
radio initialization. Installed C6 compatibility remains unverified; no
C6 reset pin or firmware replacement is assumed. This older package is
not the Ethernet reference for the active `_I_W_Y` variant. Fabian confirms Ethernet hardware
on his board; exact PHY/wiring awaits the matching vendor archive. The earlier
claim of absent Ethernet based on older schematics is withdrawn. Neither
network interface is hardware accepted.

Native AROS platform port for the ESP32-P4 SoC. The target name follows the
AROS convention `$(AROS_TARGET_CPU)-$(AROS_TARGET_ARCH)`, so the platform is
configured with

    ./configure --target=esp32p4-riscv

Configure selects the `smp` variant for this target by itself; it is the
only build, with AROS's SMP exec on both harts ([SMP.md](SMP.md), S6), and
its output goes to `bin/esp32p4-riscv-smp`. The port is developed on
[AROS-NX](https://github.com/metaneutrons/AROS-NX) (AGENTS.md, "Where the
port is developed"). It shares the CPU layer in `arch/riscv-all` with any other 32-bit RISC-V
platform. Note the two orders: an AROS target string is `<arch>-<cpu>`, built
in `configure.in` as `$target_os-$target_cpu`, while the directory it selects
is `<cpu>-<arch>`. Hence `--target=esp32p4-riscv` for `arch/riscv-esp32p4`,
the same way `--target=opensbi-riscv64` selects `arch/riscv64-opensbi`.

The SoC target has a compile-time board selector with two profiles under
[board/](board/): `P4_BOARD=d1001` (Seeed reTerminal D1001, still the default)
and the Guition JC1060P470C as `P4_BOARD=jc1060p470c-v2` (new panel batch,
the active development board since 2026-10-03) or `jc1060p470c-v1` (old
batch; read the batch from the factory firmware, not the "V2" sticker). A
profile owns GPIOs, flash geometry, partition and sdkconfig input, and selects
the panel controller and table, the rotation, the panel power scheme
(PCA9535 or plain GPIOs), the touch driver (GSL3670 with firmware, GT911
without) and the SD detect/power wiring. An unknown name fails the build.
Keep a separate configured build directory for each board: mmake does not
invalidate existing objects when only a make variable changes. The D1001 core
is byte-identical to before this split; the JC1060P470C builds but has not
booted yet (ROADMAP D1/D2, display facts in
[display/JC1060-DISPLAY-CONTRACT.md](display/JC1060-DISPLAY-CONTRACT.md)).

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

Current JC1060 production cleanup (2026-10-09): the original bootloader
`27b99e03…` and complete original0x2000..0x7fff sector range are restored and
independently verified. Clean-built core189968B `17249045…` is written and
readback-verified at0x20000, with `P4_BOOT_TIMING=0` and `P4_C1_PROFILE=0`.
The quiet package `e0869292…`, development volume and SD are unchanged.
One complete60s headless warm boot passes PSRAM/two-hart/watchdog regression
with no captured subsequent reset/fault. Following fresh readiness and a
separate30s reset capture, Fabian confirms complete desktop, pointer motion,
drive double-tap and two-finger menu without flicker/hang on this exact pair.
Cleanup step1 is accepted. Boot-latency work is deferred, not solved. Combined
no-reader qualification has build-verified optional user-space tools on this
normal core (56 host tests). A volume-only deployment/readback and complete30s
headless boot pass. The first synchronized smoke displays SMOKE:none with
responsive touch, but incomplete late USB results reject acceptance. Paced
post-load reports fix the short-run retrieval: the repeat has all fields,
30s,80 CPU batches per hart,15 verified SD reads,27 window API updates and
zero errors. Its complete late capture/validator pass; Fabian reports
SMOKE:none, responsive pointer and visible bar updates. The short smoke
gate is accepted. Fabian confirms fresh readiness and manual start of the
1800s production run. Reader-free production IDf7ed5845 now passes the strict
validator:1800s,4762 verified batches per hart,900 referenced SD reads,
1588 window API updates, zero CPU/SD errors or mismatches, reason0/fixture1.
The single passive45.182s retrieval exits0 with a complete successful footer
and no captured reset/fault signatures. Following fresh readiness, Fabian
confirms the post-load desktop, following pointer, RAM Disk double-tap and
two-finger menu-item selection remain usable without flicker or hang, with
no restart. D2-Q is accepted on this JC1060 configuration. API counters do
not prove every frame during the load; the final sight check is separate.
Cleanup and qualification are complete; D1001 regression is now the next
authorized phase, not yet tested. Larger cold-cycle and old-panel gates remain open.

D1001 production regression artifacts now pass a fresh separate SMP build
in `AROS-ESP32-d1001-production-build`: core195216B `23bfb8c4…`,
matched40-member BSP3642928B `a6d478c2…`, FAT32 SD image64MiB `fe15d66a…`
(178/178 manifest entries), firmware-bearing FAT16 development volume4MiB
`27b48944…` (5/5 entries). SRAM, package limit/ELF and independent host
filesystem/manifest checks pass. Evidence is in
`AROS-ESP32-build/evidence/d1001-production-2026-10-09/`.
D1001 core/BSP are now flashed and independently readback-verified at
0x20000/0x820000. Existing firmware volume remains unchanged. A complete
60.958s first-byte warm capture reaches32MB PSRAM200MHz, both harts,
watchdog6000ms, graphics HIDD and running GSL3670 with4356 firmware records.
SD detection reports no card: only the flash fallback boots. No Wanderer,
SD-boot or visual/touch acceptance yet; fresh readiness/card insertion next.
This supersedes the older D1001 "Currently flashed" history table below.

Nothing in the table below is claimed to work until it has been observed
working on hardware. `stub` means the file exists and compiles; `works`
means the behaviour was verified on a board and how it was verified is
recorded in the milestone notes.  Documentation is part of the definition of
done: a status change must update this table, the matching roadmap phase and
its evidence entry in the same change.

| Area | State | Notes |
| :--- | :--- | :--- |
| graphics performance | hardware partial | C1P tiled rotation, CPU360 and framebuffer v2 coalescing reduce the measured first full update from 2.776 s synchronous/CPU90 to 0.296 s queued/CPU360. Initial visual speedup confirmed, then slowdown/hang reported. Console-only nonblocking fix is sanitizer-tested, flashed and independently verified; causality and fresh visual/touch/no-reader acceptance remain open. See C1P and dated evidence. |
| configure target | done | `--target=esp32p4-riscv`, configure completes and selects the `smp` variant, the only esp32p4 build (S6); any other variant is refused |
| board selection | hardware partial | `P4_BOARD=d1001`, `jc1060p470c-v1` and `jc1060p470c-v2` select wiring and drivers and reject unknown boards; the D1001 core is byte-identical after the 2026-10-03 driver split. The JC1060P470C-v2 boots from SD to Wanderer with correct picture and working GT911 touch (J0-J4, 2026-10-04); backlight control, cold boot and stress are open |
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
| watchdogs | done | the ROM's are off from `platform_init()`, the super watchdog self-feeds; timer group 0's main watchdog is armed once the tick runs (6 s, reset system) and fed from hart 0's tick only while both harts show a tick and a canary task each wakes; a hang or fatal trap on either hart resets the board, the next boot prints the cause (SMP.md, S7) |
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
| touch HIDD | C4/D3 hardware partial | since 2026-10-04 a portable stack (ROADMAP D3): `touchscreen.hidd` (`rom/hidds/touchscreen`: polling, gesture rules, calibration, `ENV:Sys/touchscreen.prefs`), the controller drivers `gt911.hidd` and `gsl3670.hidd` (`workbench/hidds`) on AROS's `hidd.i2c`, the bus driver `hidd.i2c.esp32p4` (`i2c/`) and `esp32p4board.resource` (`board/`), which creates them from the board profile; editor `Prefs/Touchscreen` (`workbench/prefs/touchscreen`). The JC1060P470C boots it; the D1001 builds it but has not run it, so the D1001 history that follows is that of the former kernel GSL driver and `p4touch.hidd`. The D1001 answers at I2C0 address `0x40` with Silead ID `0x50910000`; GPIO16, reset on PCA9535 output 12 and the initially empty RAM-firmware state are measured.  No firmware blob is committed because redistribution authority is not established; a validated converter accepts a user-supplied vendor header or binary outside the repo.  The maintained absolute `mouse.hidd` path, persistent task-context I2C0 polling, direct-X/mirrored-Y normalization, scheduler idle correction and bounded press/release policy pass synchronized motion, three-tap and centre/four-edge observation.  The complete coherent 44-byte frame supports up to ten contacts while retaining the closest continuing contact as the visible pointer; its FIFO-segmented long read and actual independently moving two-contact frames are D1001-verified with zero errors.  Both contacts report ID zero, so positions rather than IDs deliberately drive continuity.  Two contacts promote the active press to Button2 and latch it through sequential finger release; synchronized UART and direct Intuition observation verify the menu/right-button response, absence of a spurious left re-press and clean final release.  Version 4 removes firmware from the normal core and loads the validated 34,848-byte D1001 record image in HIDD task context from `DEVS:Firmware/silead/gsl3670-d1001.fw`, with a development-only `FLASHDISK0P0:Firmware/...` fallback.  D1001 captures verify both properties: missing firmware leaves only touch disabled and does not block Wanderer, while the external fallback loads all 4,356 records, reaches status `0x5a5a5a5a` and polls without errors.  The bounded reload after repeated I2C errors is implemented but still lacks fault-injection hardware evidence, as does the full 1,000-cycle C4 soak |
| second core / SMP | both harts, SMP the only build; hardware verified (JC1060P470C) | AROS's own SMP exec with the platform layer in the port, stages S0-S6 in [SMP.md](SMP.md). Ordinary tasks stay on hart 0 under upstream's affinity model; exec's housekeeper and tasks created for hart 1 or for any hart run on hart 1. Upstream's ten SMP tests and a twenty-minute run pass, and the visual and touch check passed (S5, 2026-10-05); the D1001 is open. A hardware watchdog resets the board on a hang or fatal trap on either hart (S7, 2026-10-09). S8 (2026-10-09) lets a program run on both harts when asked (`C:Affinity`); ordinary tasks stay on hart 0 by decision. Its test, SMP-Migrate, found and fixed a `ReplyMsg()`/`WaitIO()` race and `RemTask()` of a running task in exec, a torn time in timer.device, a race in the port's `core_Switch()` and a data loss in `krnP4SyncCode()` (SMP.md, "S8"). There is no single-hart build and no Giant (S6); the Giant and the E3 attempt remain as history in commits `ff3395ba7f` and `14f0290e21`. E0-E2 second-hart diagnostics remain; S1 (native atomics on cached PSRAM) passed on the JC1060P470C on 2026-10-05. |

S8 acceptance update (2026-10-09): Fabian confirms Wanderer and touch on
production core `a4179c1e…`, including the requested normal-use check.
The soft-interrupt wakeup candidate `f77784aa…` failed its closing run:
a worker's saved SP was on hart 0's interrupt stack, followed by a
software-interrupt timeout and a later watchdog reset. A trap-exit
correction now masks MIE before lowering the nesting depth; the actual-body
host regression passes at O0/O2 and rejects the old code. The clean-built
runner passes a 360-second hardware smoke capture with no fault signatures; the
full twenty-round closing run passed all 240 programs with twenty successes
from each test's own summaries and no fault or unexpected-reset signature.
The capture was stopped after completion at about 33 minutes, not 48 minutes.
Production core `0e1fa444…` passed its separate 360-second headless soak:
both harts online, watchdog armed, Wanderer started, no fault or reset.
The existing development volume is `aa7eadbe…`. The exact production
artifact also passed its synchronized Wanderer/display/touch check: Fabian
confirmed readiness, then reported everything fine after the requested
normal-use procedure. PR #70 merged as `0c15f90eb3` after required CI.
The unchanged production core also passed a separate thirty-minute
reader-attached idle soak; no-reader and physical cold gates remain separate.

Cold-start correction candidate (2026-10-09): the same production core stayed
black after a user-confirmed USB-only rail-off/reapply without a log reader;
a separate reader-attached USB reset recovered Wanderer. Saved PC pointed
into the early USB output wait. Source audit establishes repeated per-byte
wait amplification, not the full hardware cause. USB byte and flush waits
now share one failed-poll allowance per boot, retain single-probe late-reader
recovery and do not renew the allowance for blocking diagnostics. The
candidate is core `4d998b4e…`, 190,240 bytes, clean-built from
`fix/esp32p4-usb-boot-budget` and writer-verified at `0x20000`; it supersedes
the JC1060 core listed in the historical artifact row below. Package,
development volume, SD and bootloader are unchanged. A 120-second
reader-attached USB-reset boot completed normally: PSRAM, both harts,
watchdog, Wanderer and continuing touch polls, no captured fault/reset.
Four synchronized physical USB-only cold boots now pass visually:
after at least 30 seconds disconnected and reapplication without a serial
reader or reset, Fabian reports Wanderer booting cleanly after approximately
10 seconds initially, then 11, 14 and 11 seconds in three repetitions
(2026-10-09). Fabian also confirms pointer movement, drive double-tap and
two-finger menu operation on this core. No cold-boot UART was captured, so
internal reset, PSRAM and hart diagnostics are not inferred from these
observations. Larger cold-cycle reliability and sustained no-reader/combined
stress remain open; this is not full production qualification.

Boot-latency optimization (2026-10-09): the 10-14-second visual cold-start
estimates above used the verbose package. For the quiet comparison the s15 core
was paired with diagnostic-off package `e0869292…` (3,636,260 bytes, verified at
`0x820000`). Only DOS/FAT/dosboot records changed; the other 37 records and
all media/bootloader/core are unchanged. Three reader-attached reset captures
pass PSRAM/two-hart/watchdog/touch-polling and graphics-update checks. At the
same graphics update marker (n256), median host receipt is 3.955s versus
4.408s across three verbose baselines, a 0.453s difference; this is NOT
first-visible-desktop timing. Captured output falls from about 584KB to 15KB.
One fresh USB-only physical cold/display comparison now passes: Fabian
reports approximately 10s to fully visible Wanderer (2026-10-09). This falls
within the verbose baseline's 10-14s range, so material cold-start acceleration
is not established. Fresh touch and repeated-cycle acceptance of this pair
remain open; prior verbose-package acceptance does not transfer automatically.

Optional `P4_BOOT_TIMING=1` diagnostics now retain bounded SYSTIMER phase
measurements beginning before the kernel's first console output. A five-second
reporter rotates one retained phase per short packet (all eight in40s), so a
late reader need not recover discarded early narration. These are NOT time
since power-on or proof of a completed desktop; the counter's pre-kernel
origin is unknown. `MISSING`/`INVALID` and failure bits must not count as zero.
The s17 diagnostic core is now flashed at `0x20000`: 191,856 bytes, SHA-256
`e09de20b6d7f6ec2ea3c88185f4c275389cbb17a3f8c0d9e5073623d044fbf59`.
One45s reader-attached reset reports all phases with zero measurement failures:
PSRAM44ms, panel-probe return706ms, pre-Exec1145ms and update2563686ms.
The final late reader preserves this warm boot (now71/76/81s, same retained
values); the initial low-line reader instead caused a reset and is rejected.
These are warm diagnostic results, not cold timings or fresh visual acceptance.
Ordinary s15 core remains the rollback; quiet package/media/bootloader unchanged.

Fresh diagnostic physical cold comparison (2026-10-09): after confirmed
USB-C-only disconnection and an additional assistant-timed30s, Fabian reports
initially13s to Wanderer, subsequently corrected to11s with correct desktop
presentation confirmed. This is one cycle, not a second timed boot.
No serial reader during startup. A late45s reader obtains all
eight retained phases, fail0, now27..67s and no new boot banner: first output
844ms, pre-Exec1984ms and update2564772ms. Opening is consistent with boot
continuity in this cycle. Kernel-relative timing does not locate all11s;
update256 is not desktop completion. Touch and exact
power-on-to-kernel timing remain unmeasured. No firmware change in this test.

Host correlation: `tools/observe-usb-arrival.py` observes the
serial device node without opening it; `tools/late-boot-log.py --timing` records
chunk receipt times and raw offsets on the same host monotonic clock. A second
unchanged diagnostic cold boot is reported11s. All8 phases/fail0 are retrieved;
receipt-derived kernel-origin estimates6.804..7.014s after node appearance
motivate a pre-kernel investigation. Node appearance is not rail-on and
buffered receipt is not device generation; no exact bootloader delay or
material speedup is established. No bootloader write is authorized by this test.

An isolated diagnostic bootloader now disables only the ROM secondary USB
output after preserving original UART setup. Two clean builds reproduce the
22,624-byte candidate `e9660f4d…`, with the unchanged board sdkconfig and
verified wrapper linkage; all45 host tests pass. The normal bootloader/shared
IDF remain unchanged. After explicit sector-range approval0x2000..0x7fff,
candidate is flashed and independently readback verified; the full previous
range is backed up and the partition sector unchanged. One60s first-byte warm
capture completes with first-attempt32MB PSRAM, both harts, watchdog and all8
phase slots/fail0, no captured fault/subsequent reset. The first synchronized
no-reader cold comparison is reported12s versus prior11s; all8 retained phases
and receipt-derived origin estimates remain essentially unchanged. This
candidate has not demonstrated acceleration. The passive arrival observer
does not establish physical rail-on, and the late capture cannot exclude
earlier unseen resets. See the dated evidence; pre-kernel attribution remains open.

`tools/build-quiet-package.sh` clean-rebuilds those three modules and replaces
only their records in a supplied immutable baseline, then checks the board's
package-size rule. Keep verbose package `d9e40d75…` for reversible diagnostics.
Optional `tools/reset-and-log.py --timing <new.jsonl>` records host-receipt
time and raw-byte offsets without changing the raw capture; USB buffering and
the buffered ROM head prohibit claiming firmware phase or no-reader cold times.

E2, the two-hart primitives, is hardware verified on both boards: on the
D1001 on 2026-10-01 (219,776-byte candidate, five captures, two epochs each)
and on the JC1060P470C on 2026-10-05, where it runs together with S1. S1
shows that native AMO and LR/SC, including the masked loops GCC emits for 8-
and 16-bit operands, are correct on cached PSRAM across both harts under full
contention (`P4_S1_PSRAM_ATOMICS=1`, twelve of twelve epochs, results read
back from PSRAM). So AROS's own `aros/riscv/atomic.h` suffices once the
generic `atomic.h` selects it. Both diagnostics park the board before Exec;
they ran in the former normal build, and since S6 they build in the `smp`
variant, which links but has not been run. See SMP.md, "Current plan", and
ROADMAP Track E.

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
The legacy `p4touch.mode` file went with `p4touch.hidd` on 2026-10-04. The
mode (tap or direct), the gesture parameters and the calibration are lines
in `ENV:Sys/touchscreen.prefs`, written by `Prefs/Touchscreen` and applied
by `touchscreen.hidd` at boot and whenever the file changes. Save cannot
persist yet, because only RAM: is writable on these boards.

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
| Currently flashed | D1001 (`e8:f6:0a:e0:46:4c`): patched ESP-IDF v6.0.1 second stage unchanged; the B5R test core (Giant SMP, `ff3395ba7f`, 20 MHz D-PHY reference, no row-phase workaround), 213,600 bytes at `0x20000`, SHA-256 `153405e7a3ffe4485e40f2e9fff2e60a37281fa688630f18277de39f27e5dbdc` (2026-10-04); BSP `eadb8723…` at `0x820000` and firmware volume at `0xc00000` unchanged. The D3 core `fb16af6b…` and package `42771087…` are built but not written. Its SD card is in the JC1060P470C. JC1060P470C (`80:f1:b2:d3:3b:a6`): AROS bootloader `27b99e03…`, partition table `b71dde30…`, core `0e1fa444…` at `0x20000` (S7 and S8 stages 1 and 2, from `feature/esp32p4-smp-stage2`), package `d9e40d75…` at `0x820000`, development volume `aa7eadbe…` (Prefs/Touchscreen, SMP tests, `Affinity`, twenty-round list `s9-stress`) at `0xc00000`, card image `5f123f79…`; core flash verified 2026-10-09; corrected runner completed all 240 tests, production 360-second headless soak passed, visual and touch check of this exact core passed (Fabian, 2026-10-09) (the state before: core `bce06be9…`, package `ea3a23ab…`, volume `7a8b6828…`, from AROS-NX `main` `c25fb42946`, visual check passed 2026-10-08); factory image backed up (`03222de1…`). Backups/logs: build `evidence/`. Core and package go together: neither board's current core pairs with an older package. |

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
`bootloader/boards/d1001/partition-table.csv` (each board has its own under `bootloader/boards/`); `esp32p4-partition-table` runs ESP-IDF's
own `gen_esp32part.py` in both directions and leaves these checked artifacts
under `bin/esp32p4-riscv-smp/gen/rom/boot`:

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
    a memory header, MEMF_FAST since 2026-10-05, at priority -20

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

Configure adds `--enable-target-variant=smp` by itself (S6); the build
lands in `bin/esp32p4-riscv-smp`.

Out of tree, from a build directory of its own. Three things about this host
are worth writing down, because none of them is guessable:

  - **GNU Make 4 first in `PATH`, as `make`.** The system make is GNU Make
    3.81; Homebrew's is 4.4.1. Calling `gmake` at the top is not enough:
    mmake runs every sub-make as plain `make`, and since upstream's
    `config/make.tmpl` writes linker response files with `$(file >...)`,
    which 3.81 silently ignores, the crosstools stage then fails at
    `libmui.a`. Put `/opt/homebrew/opt/make/libexec/gnubin` before
    `/usr/bin`.
  - **Submodules.** The catalogs (among them `rom/dos/catalogs` and
    `workbench/libs/muimaster/catalogs`) are git submodules. A new worktree
    starts without them, and `dos.library` and the Zune classes then fail
    on missing `*_strings.h`. Run `git submodule update --init` once; with
    `--reference <main checkout>/.git/modules/<path>` per submodule it
    needs no network for commits the main checkout already has.
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
