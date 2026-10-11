# ESP32 board maintenance tool

Decision: C6-first implementation authorized by Fabian on 2026-10-10, with
both CLI and GUI. This plan defines requirements; execution and dated evidence
remain in [the port roadmap](../../arch/riscv-esp32p4/ROADMAP.md), item N2M.
GitHub issue tracking is unavailable: AROS-NX has issues disabled. No issue
numbers or completed tracking setup are implied.

## Outcome and boundaries

`Tools/ESP32Board` is an AROS maintenance application, not another network
manager. Both frontends use one operation registry and update backend. The
first target is the JC1060's ESP32-C6 through the P4's existing ESP-Hosted SDIO
transport. Subsequent backends update P4 Kickstart/core and associated modules,
including over-the-air acquisition. Existing AROS networking tools remain
responsible for WLAN selection, association and IP configuration.

Initial baseline: native AROS had an opt-in serialized Core1 SDIO owner and no
C6 OTA client. The private engine/queue below now exists, but no public update
action is enabled. Pinned Hosted d5184217f710d54c01f932e75b4c55e3f4933fc9 has OTA_BEGIN,
OTA_WRITE and OTA_END, but no transactional abort, offsets or replay protection.
Installed C6 identity, partition layout and rollback remain unverified. CN5
cabling is unavailable. Conditional C6 update authority still requires a
secured recovery path; source development and read-only file checks do not.

The separate [first-install runbook](esp32-c6-first-install.md) requires
CN5 ROM recovery before an app-only bootstrap. That procedure does not wait
for this product's OTA frontend/full-transfer qualification and does not
enable it. Discovery code not yet installed cannot qualify its own deployment.

## Design

- The CLI resolves actions and the MUI window dispatches the same operation
  identifiers. Capability differences must be explicit and tested, not hidden
  behind a successful command or an enabled button with no implementation.
- MUI handlers never perform flash or lengthy file/network I/O. A worker owns
  immutable requests; messages and source data live until their replies.
  Closing a window requests cancellation and retains it until safe completion.
- C6 operations must enter an exclusive maintenance lease in the existing radio
  owner. No second task drives SDIO. SANA traffic, scans and association are
  quiesced and fenced before maintenance; the shared SDMMC arbiter still grants
  bounded slices to storage. Resource version and size are checked explicitly.
- Validate an owned immutable image against a trusted manifest/digest before
  BEGIN. Validate C6 chip/app format, exact length, device/revision, partition
  capacity, Hosted compatibility and anti-rollback/security policy. A hash
  supplied alongside an untrusted download is not authenticity.
- The tool-private C6 source loader opens once, freezes and inspects the bytes,
  closes before publication, and retains the exact source until explicit
  disposal. CHECK-C6 uses this loader, then disposes. A future admitted updater
  must retain it, recheck the stored digest/metadata and submit these bytes
  without reopening a pathname; dispose only after synchronous submit has
  copied or refused them. The object is exclusively owned, not concurrent or
  protected from a malicious caller. Its digest is trusted input, not a release
  manifest or publisher-authentication producer. No OTA gate may be fabricated
  from this object or from the current user-supplied SHA field.
- Discover the actual P4 and C6 flash capacities and partition tables through
  their respective read-only backends. Never derive a live layout from build
  defaults. Render proportional partition extents and gaps in the GUI, with
  labels, offsets, sizes, types, encryption and active/update roles. An imported
  table is a provenance-labelled preview only, not active-slot or capacity
  evidence. Refuse writes for missing/ambiguous layout, overlap, out-of-bounds
  targets or unknown partition policy. CLI exposes the same layout data.
- Reuse the pinned protobuf format and persistent UID ledger. Never replay
  an ambiguously delivered BEGIN, WRITE or END. WRITE has no offset: retrying
  can duplicate bytes. Correlation needs both UID and response ID.
- BEGIN may erase the fallback. Failure/cancellation after it means an
  incomplete update, not restored firmware. Never send END as cleanup.
  END sets boot selection before its restart timer can fail: any failed or
  lost END completion is commit-unknown. A successful END acknowledgement is
  only waiting-for-reboot; installation requires fresh identity and health.
- Maintenance finalization must receive an explicit terminal disposition.
  Before any BEGIN attempt, ordinary bounded read-only release is permitted.
  After BEGIN, including rejected or ambiguous BEGIN, an incomplete transfer
  remains fenced for recovery. Any unsuccessful END is commit-unknown;
  successful END remains fenced waiting for reboot. These states may not
  call WiFiStart, resume normal polling/reassociation, retry OTA or reset the
  peer automatically. Finalization succeeds only after confirmed bounded
  ownership handoff, not after queuing a release request. Its failure must
  remain visible without changing the transfer/commit classification.
  Native ABI9 provides that separate terminal fence queue/poll boundary,
  but exposes no OTA job. The private engine/queue below sets the sticky
  obligation before BEGIN, not deferred until a possibly
  poisoned transfer completes. Original30-second discovery leases are not a
  full-image budget. Host/module tests cannot close runtime acceptance.
- The portable engine requires a single original aggregate deadline, a finish
  reserve, per-call timeout caps and a monotonic clock. Native integration must
  enforce the passed timeout in each blocking callback; clock checks cannot
  preempt an unbounded callback. Qualify total and reserve on actual full-image
  transfers and storage contention; there is no production timing default.
  Work cannot consume the finish reserve, no WRITE renews time, and a late END
  receipt must remain acknowledged-but-unconfirmed rather than installed.
- A port-private owner-only BEGIN/WRITE/END entry now provides task/lock/token/
  generation admission, sticky pre-BEGIN and pre-END disposition, exact
  acknowledged-byte accounting and no-replay closure. It is not a queued job,
  engine adapter, authentication/layout/recovery implementation or frontend
  permission. Both release assertions remain disabled. Its per-call deadline
  is bounded by the original30s preparation lease, not a production full-image
  budget; the native job must bind the engine's aggregate deadline separately.
- Native finalization now has a direct owner-only entry, not a call back into
  the requester's queue/poll hook. It maps engine dispositions explicitly to
  the different ABI enums and confirms bounded release or a fresh host fence.
  It is a prerequisite for the engine adapter, not a queued update operation;
  post-BEGIN failure retains sticky silence and never sends cleanup RPCs.
- The resource's synchronous engine binding validates an already ACTIVE
  owner lease and invokes the actual transfer engine. Its first clock sample
  is the original maintenance request, not engine entry; explicit budgets
  cannot exceed the existing30s preparation window. Input bytes and ownership/
  cancellation context must remain immutable/live through return. They are
  trusted internal callbacks, not a public queued API. The future dispatcher
  must supply owned storage, bounded cancellation independent of client_lock,
  a qualified aggregate full-image lease and device/source proofs; do not turn
  this synchronous entry into a GUI callback or merely increase the30s constant.
  Accepted early engine exits must finalize the already-owned lease even if
  the engine has not reached acquire-validation; clock/deadline failure must
  never authorize a late restart. Atomic admission refusal remains caller-owned.
- The private native single-slot dispatcher copies the entire source before
  requesting maintenance, then runs the engine only in the serialized owner.
  Its resource-lifetime result survives owner teardown. A distinct short-held
  lock protects copied observations; an atomic cancellation flag is independent
  of client_lock and metadata polling. Never copy a live engine result. Keep
  the frontend worker alive across window Close until DONE/discard; requester
  task-kill recovery is not provided. ABI10 now appends typed optional hooks;
  every hook is NULL unless the owner, host privacy/recovery and independent
  C6 update/recovery assertions are enabled. Do not require installed C6
  credential privacy merely to install its first privacy fix. Native OTA
  admissions use the resource-private update gate; association remains gated
  independently on both original credential privacy/recovery assertions.
  The new update assertion defaults0 and is not a CLI bypass: independent
  restore, full-transfer qualification and per-job source/target/layout proofs
  remain mandatory. No release assertion is qualified by a build/test alone.
  Check exact ABI and full resource size before reading hooks, never fall back
  to raw private symbols. This is not a frontend update action or full-image
  timing qualification, and release assertions are not runtime device proofs.
- A missing recovery proof is a refused write, not a command-line override.
  Recovery evidence must refer to the actual device and layout. Do not infer
  installed rollback from the candidate's build configuration.
- OTA acquisition uses existing AROS network/TLS facilities, not a parallel
  Wi-Fi stack. Download and freeze the complete verified image before stopping
  the C6 used to obtain it. Transport OTA (SDIO delivery) and remote HTTPS
  acquisition are distinct capabilities and acceptance claims.
- P4 updates need a deliberately designed privileged flash interface. The
  current P4 layout has no inactive app slot; do not assume atomic swap or
  redefine an in-place write as rollback-safe OTA. Bootloader, partition table,
  NVS and storage remain separately authorized targets.

## Delivery and normative acceptance

Requester drainage foundation (2026-10-11): the tool links the shared
`boardtool/ota_client` and private `boardtool_ota_prepared` native adapter.
Accepted submission is never replayed; cancellation remains pending through
correlated DONE and confirmed discard. Invalid metadata is not safe completion.
An owner stall retains the submitting task, not a fabricated timeout success.
No current operation dispatch calls the adapter. Authenticated prepared-job
admission, GUI progress/cancellation messaging, full-image lease qualification,
device proofs and independent first-install restore remain acceptance gates.

### Proposed C6 read-only discovery extension

Read-only diagnostic implementation (2026-10-11): COMPARE-C6 in CLI/MUI
combines an owned checked image with negotiated registry discovery and exact
source recheck. Rejected/unknown/risk reports do not populate the update-job
compatibility assertion. CLI uses FAIL/WARN, never OK for compatibility.
Policy/provenance stay unknown. App-descriptor eFuse-block bounds and MMU page
log2 are distinct from wafer revision; v2 has no matching block/loader facts.
Physical secure-bit count does not supply the installed bootloader's enforced
floor. Maximum wafer revision is conservatively screened without a detected
override bit. Flash mode/rate, signature/encryption-aware handling, installed
layout and independent first-install recovery remain separate requirements.

Compatibility audit (2026-10-11): the historical version1 inventory had no chip
model/revision or eFuse secure_version. Capacity and slot roles alone cannot
validate an image's revision limits or security version. A later explicitly
negotiated version must carry detected facts with unknown/virtual provenance;
do not reinterpret version1's required-zero reserved bytes. The pinned IDF
provides read-only esp_chip_info and full-field eFuse counting APIs. Discovery
version2 now carries independently known chip/revision/full physical counter
facts and requires matched private ABI11 consumers. The convenience
esp_efuse_read_secure_version helper reads an app-config-sized prefix and is
not used for that fact. The full counter does not establish the installed bootloader's anti-rollback policy,
and app sdkconfig cannot establish that policy either. The extension is
undeployed, so it cannot supply first-install proofs. Secure first-install
recovery and actual device/layout evidence through independently verified CN5
remain separate prerequisites, not fields populated from build defaults.

The pinned schema has only OTA_BEGIN/WRITE/END; none returns capacity,
partitions or active slots. Extend the owned C6 candidate rather than assuming
those facts from its build CSV. This is a design requirement, not an installed
capability or a completed protocol implementation:

- Explicit startup capability/version negotiation must precede any new query.
  A stock peer receives no speculative command. Allocate extension message IDs
  only after checking the complete pinned schema and negotiated envelope bounds;
  a version banner alone is not protocol or binary identity.
- Return an owned, bounded snapshot: actual flash size, partition extents,
  labels/type/subtype/flags, running partition, selected boot partition and
  next update partition. Every role references an exact validated table entry;
  missing, inconsistent or overlapping entries prevent target selection.
- Distinguish detected facts, unsupported capabilities and unknown security
  policy. Report flash encryption, secure boot, anti-rollback and rollback
  state when supported. Missing values must not deserialize as safe/disabled.
  A normalized partition list must be labelled as such, not raw table bytes.
- Correlate replies with the existing owner generation and unique UID. Snapshot
  consistency must cover all pages if chunking is needed; never merge records
  across peer resets or slot-selection changes. Revalidate before BEGIN under
  the exclusive maintenance lease. Discovery itself must not erase, select a
  boot slot, reset the peer or change Wi-Fi configuration.
- The first installation cannot use discovery code that is not installed yet.
  Its recovery evidence and residual brick risk need a separate explicit
  deployment decision. Neither a source-tested extension nor a successful END
  acknowledgement proves postboot health or independent recovery.

### M1: Shared foundation and read-only preflight

Dependencies: none; native tool build depends on the existing AROS SDK.

- M1-A1: Exact pinned OTA request/response codecs pass positive and malformed,
  duplicate, correlation, capacity and alias tests on O0/O2 with sanitizers.
- M1-A2: Shared transaction engine proves no retry, cancellation boundaries,
  exact byte accounting, single lease release, no END cleanup and the
  distinction between acknowledged, commit-unknown and installed in mocks.
- M1-A3: CLI and GUI invoke the same read-only image check. Actual C6 app,
  wrong hash, wrong chip, bootloader, truncation and malformed structure are
  accepted/rejected as appropriate; native RV32-SMP build succeeds. Read-only
  acceptance must not claim target compatibility or successful updating.
- M1-A4: One capability registry and source-linked frontend parity gate have
  both positive and deliberately divergent counter-probes. Builds cannot
  substitute for interactive GUI acceptance.
- M1-A5: Bounded partition parser accepts identified table inputs and rejects
  overlap, overflow, malformed/unterminated records and oversized capacity.
  CLI lists the same extents the GUI renders graphically. Imported previews
  are visibly distinguished from detected installed layouts.

### M2: Native owner maintenance backend

Dependencies: M1; audited live C6 layout/protocol and secured recovery evidence.

- M2-A1: Versioned maintenance API owns requests and sources through terminal
  completion; exclusive lease fences live SANA clients, prevents transport
  re-entry and releases storage slices. Close/cancel/owner loss are tested.
- M2-A4: Actual C6 capacity, table and selected/running slot are discovered;
  both frontends use them to show and validate update targets. Changing a
  qualified table changes the view and permitted extent without rebuilding
  the application. Missing or inconsistent discovery prevents BEGIN.
- M2-A2: Real BEGIN/WRITE/END use the existing owner and non-reused UID ledger.
  Fault injection covers lost ACK, short read, peer reset, timeout and END
  failure after selection. None produces success or automatic retry.
- M2-A3: Freshly recovered actual C6 accepts one qualified image. Postboot
  identity, UART privacy, SDIO startup/MAC/scan and recovery proof refer to
  exact installed bytes, not merely a version banner or build.

### M3: Usable C6 CLI and GUI updates, including OTA acquisition

Dependencies: M2; qualified AROS networking and trusted release metadata.

- M3-A1: Both frontends support check, explicit update, acknowledged byte
  progress, safe cancel and postboot result; close during update remains safe.
  CLI failures have nonzero return codes; GUI does not collapse uncertain
  commit into failed-with-no-change or installed-success.
- M3-A2: Both accept a local image and authenticated remote HTTPS acquisition
  with bounded size, exact length and trusted digest/signature validation.
  Failed TLS, changed image, wrong target and missing manifest cannot write.
- M3-A3: Real CLI and freshly synchronized GUI update/recovery qualification
  passes on JC1060; unaffected SD/desktop/touch and WLAN behavior are measured.

### M4: P4 Kickstart/module update backend

Dependencies: M1 interfaces; explicit P4 layout/recovery design, not implicit M2.

JC1060 target layout selected by Fabian on 2026-10-11 (MiB/KiB units):

| Region | Flash offset | Size |
| --- | --- | --- |
| Boot infrastructure, including existing metadata and alignment | 0x000000 | 128 KiB |
| KERNEL_A (`ota_0`) | 0x020000 | 512 KiB |
| KERNEL_B (`ota_1`) | 0x0a0000 | 512 KiB |
| BSP_A | 0x120000 | 6 MiB |
| BSP_B | 0x720000 | 6 MiB |
| DEVELOP, independent data partition | 0xd20000 | 2944 KiB (2.875 MiB) |

The last byte is 0xffffff. The 128-KiB prefix is not a bootloader binary
size: preserve the existing bootloader/table/NVS/otadata/PHY placement within
it. The inactive `partition-table-ab.candidate.csv` records this design;
neither the active board profile nor installed device uses it. D1001 remains
unchanged; this decision covers JC1060's 16-MiB capacity only.

Before activation, map the selected kernel to its matching BSP, remove the
current shared-BSP/development-volume split assumption, and derive DEVELOP
from its independent partition. Retain runtime partition provenance for the
tool's graphical layout. Enforce complete kernel-image size <=512 KiB and
package size <=6 MiB, including image-format overhead. Define a power-loss-safe
pair manifest/commit policy with bounded boot attempts and explicit successful
boot confirmation; both components must be authenticated/validated before
selection. A kernel slot alone is not a valid update pair.

Migration relocates the current BSP and shrinks the development volume from
4 MiB to 2.875 MiB. Preserve its files and rebuild the filesystem; truncation
is not a migration. Back up the actual device, qualify the bootloader/XIP path
for both application offsets and keep a ROM restore route before any table
write. Do not use the current legacy partition-flash confirmation for A/B.
The existing table at0x8000 limits the bootloader starting at0x2000 to24KiB;
the recorded production bootloader22544B leaves only2032B. A/B bootloader
growth must be measured against that actual bound, not the128-KiB prefix.
Moving the table and metadata within the same prefix may be required; that
needs a separately validated internal-prefix design and migration. The
candidate currently preserves their historical positions, not new headroom.
Complete these gates separately from the prepared WLAN test; no current
artifact or deployed layout is silently reinterpreted as A/B.

- M4-A1: Board profile and exact flash layout select permitted targets; coupled
  core/BSP compatibility and package bounds are validated before writes.
- M4-A2: Local and OTA-acquired updates share CLI/GUI actions and backend;
  interruption/recovery and real cold-boot tests use identified hardware and
  immutable backups. No protection boundary is bypassed for convenience.

## Risks, migration and verification

This is additive; existing AROS WLAN tools and production firmware remain
unchanged until an explicitly qualified update is deployed. First deliveries
may expose preflight while refusing update, but cannot close M2 or M3. Mock
tests prove their stated model only, not Exec, SDIO, flash timing or hardware
rollback. Keep default-disabled credential/privacy gates unchanged. SHA-256
detects corruption relative to trusted input, not publisher identity by itself.
No release publication or repository protection change is included here.
There is no measured end-to-end duration or committed completion estimate.

## Decision changes

2026-10-10: Fabian requires adaptive flash layouts and graphical partition
display. Added M1-A5/M2-A4 and the read-only discovery/provenance boundary;
hardcoded profile diagrams cannot satisfy installed-layout acceptance.

2026-10-10: Fabian restricts the native application to RISC-V ESP32 builds.
Current implementation permits only esp32p4/riscv, including its SMP variant;
aggregate registration and direct normal/quick targets enforce the restriction.
Other ESP32 variants need an explicit supported backend, not a generic build.

2026-10-10: Fabian identifies C6 protocol extension as a possible requirement
and states that the first update remains a risk. Record the proposed read-only
discovery contract above. Risk cannot be reduced to zero; this statement alone
does not lift the existing conditional write authority or waive recovery.
