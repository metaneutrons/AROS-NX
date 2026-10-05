# ESP32-P4 / RV32 SMP delivery plan

Requirements: this document. Execution state and dated evidence: ROADMAP.md,
Track E. Existing repository tracking is retained; no GitHub epic or milestone
issues have been created or authorized. Stage IDs below are acceptance IDs,
not issue numbers. User authorizes beginning this work on 2026-10-01.

## Outcome and boundaries

Eventually run ordinary AROS tasks safely on both high-performance P4 harts.
The low-power core is outside this SMP scope. A second bare-metal worker is
an intermediate capability, not Exec SMP or a claim that AROS uses two CPUs.

Baseline: one HP hart, CPU360/PSRAM200, framebuffer ABI v2 coalescing and
explicit +525 row-phase workaround. Corrected PNG SD image opens RAM Disk
without a requester and shows a toolbar symbol; drawing is reported normal
so far. Delayed slowdown/hang qualification remains open. Retain the exact
core/BSP/SD identities recorded on 2026-10-01 and rollback copies.

## Current plan: AROS SMP on the P4 (S stages, from 2026-10-04)

Decision (Fabian, 2026-10-04): the port moves to AROS's own SMP, the `smp`
build variant (`__AROSEXEC_SMP__`), with its SMP code in `arch/riscv-esp32p4`
and as few changes outside it as possible. The transitional Giant (branch
`giant-smp`, `ff3395ba7f`) and the fine-grained E3 runtime (branch `e3-smp`,
`14f0290e21`) are references, not the base. This branch,
`feat/riscv32-esp32p4-v3`, was rebuilt from `44336e404a` without either. The
E0-E2 diagnostics stay: the second hart's release, entry, mailbox and IPI
primitives are hardware-qualified and needed again.

### Starting point

What AROS SMP expects from a platform (research on the base and on upstream
`5da9fd5072`, 2026-10-04):

- `exec_platform.h`: per-CPU state reached through TLS (ThisTask, nesting
  counts, scheduler flags, quantum, elapsed time), `EXEC_SPINLOCK_*` mapped to
  kernel.resource and, at upstream, `EXEC_BLOCK_DISPATCH_INC/DEC`; the
  syscalls `krnSysCallReschedTask` and `krnSysCallSwitch`.
- kernel.resource: `KrnSpin*`, CPU count and number, CPU masks,
  `KrnScheduleCPU`, a per-CPU scheduler, an IPI layer (`kernel_ipi.h`,
  `core_DoCallIPI`), a per-CPU tick, one idle task per CPU and the secondary
  start. The generic files are stubs that every SMP port overrides in its arch
  directory. execlock.resource is generic.
- No RISC-V port has working AROS SMP, upstream or here. Configure allows
  `smp` only for x86_64 and the Raspberry Pi targets and sets
  `__AROSPLATFORM_SMP__` for no 32-bit RISC-V target; `arch/riscv-native` is an
  incomplete stub. The reference implementations are `arch/aarch64-native` and
  `arch/arm-native` at current upstream.
- Upstream reworked the generic SMP code between our base and `5da9fd5072`
  (Bo Ståle Kopperud): `tc_SpinLock` now guards a task's state, signals and
  list membership; semaphores, ETask children, system lists and allocators
  are locked; cross-CPU signals use cancelable IPIs. Still open upstream:
  `RemTask()` of a task running on another CPU (no eviction), the pre-launch
  hook running after a new task is published (`NewAddTask`), task.resource
  list locking and the unlocked scan in `ChildStatus()`.
- Gaps in shared RISC-V code: `compiler/arossupport/include/atomic.h` has no
  RISC-V branch, so atomics fall back to `Disable()`/`Enable()`, which does not
  exclude the other hart. `arch/riscv-all/exec/stackswap.S` reads
  `SysBase->ThisTask`, which an SMP build does not provide; upstream fixed the
  same for aarch64 with `FindTask(NULL)`.

P4 constraints from E1-E3 and the Giant:

- Native AMO and LR/SC are qualified only in cached internal SRAM. The PSRAM
  result of E3 (81,920 adds) came from a software service behind an SRAM
  lock, not from native PSRAM atomics. Generic AROS SMP updates fields of
  `struct Task` atomically, 8- and 16-bit ones included, and tasks live mostly
  in PSRAM.
- An LR/SC sequence must stay within three instructions with a forward
  compare only (E2); the code GCC emits for sub-word atomics has to be checked
  against that.
- A cache or flash window needs the other hart parked in SRAM, and code that
  runs meanwhile must be SRAM-resident (`check-sramtext.sh`).
- IPIs use the CLIC software interrupt lines (E2-A3). Each hart needs its own
  ISR stack. Hart 1 has no tick of its own. The SYSTIMER snapshot must be
  serialized across harts. There is one graphics producer. FPU state is per
  hart.

### Changes expected outside the port

Each needs its own justification and, where it is generic, an upstream PR:

1. `configure.in`/`configure`: allow the `smp` variant in the esp32p4 case and
   set `__AROSPLATFORM_SMP__` for it. Unavoidable.
2. `compiler/arossupport/include/atomic.h`: a RISC-V branch that selects
   `aros/riscv/atomic.h`, with sub-word forms there if missing. A generic
   RISC-V gap.
3. `arch/riscv-all/exec/stackswap.S`: get the task through `FindTask(NULL)`, as
   aarch64 does upstream. Generic.
4. Generic SMP fixes only where a gap actually affects the P4 (candidates
   above); they would help arm and aarch64 as well.

Everything else, from TLS to scheduler, IPIs, spinlocks and the secondary
start, belongs in `arch/riscv-esp32p4`.

### Stages

- **S0 Base.** Rebase this branch onto current upstream (`origin/master`), so
  the work starts on Kopperud's SMP code. Single-hart builds of both boards
  boot; JC1060P470C visual and touch check with a fresh "bereit". Set up a
  separate SMP build tree. Status 2026-10-04: rebased onto `5da9fd5072`;
  the JC1060P470C passes, visual and touch check included (ROADMAP, S0
  entries). Open: D1001, SMP build tree.
- **S1 Atomics on PSRAM.** Qualify native AMO and LR/SC on cached PSRAM across
  both harts on hardware, including the 8/16-bit sequences GCC generates, and
  check them against the LR/SC rule. If they hold, change 2 above suffices.
  If not, decide between an SRAM-lock software service for the port, which
  needs a hook in `atomic.h` and so a further shared change, and other
  placements. This is the main risk for the goal of few changes outside the
  port.
- **S2 SMP build on one hart.** Configure the `smp` variant for the P4 and
  provide the platform layer in the port: `exec_platform.h` with per-hart
  state, `KrnSpin*`, CPU count and masks reporting one CPU, the two syscalls.
  It builds, links and boots on hart 0 with hart 1 held in reset.
- **S3 Second hart online, idle.** Start hart 1 from the E1/E2 entry with its
  own ISR stack, CLIC and IPI setup, per-hart state and a tick (forwarded from
  hart 0 or its own timer, to be decided); one idle task per hart; park for
  cache and flash windows. Hart 1 only idles.
- **S4 Scheduling on two harts.** A per-hart scheduler after the
  aarch64-native model, `cpu_Switch` releasing `tc_SpinLock` of a waiting task
  (the upstream x86_64 fix), `KrnScheduleCPU` through IPIs and task affinity;
  first tasks bound to hart 1, then free migration.
- **S5 Qualification.** Upstream's SMP tests (`developer/debug/test/smp`), a
  sustained stress run, serialization of SD, console, timer and graphics, and
  boot, visual and touch regressions on both boards, each visual test with a
  fresh "bereit".
- **S6 Default decision.** SMP or single hart as the P4 default; the Giant
  branch is retired then.

Each stage records its evidence in ROADMAP Track E in the same change.

Open points:

- Whether native atomics on cached PSRAM hold across harts is unqualified (S1).
- Which of the generic gaps above actually affect the P4 is unknown until S4.
- Whether hart 1 takes its tick from hart 0 or from its own SYSTIMER alarm is
  open.
- How much of `arch/riscv-native` can be reused is unclear; it is incomplete
  and oriented to supervisor mode.

The sections below are the record of the earlier approaches: the Giant, which
ran on hardware, and E0-E3. Files, switches and tools they name that belong
to E3 or the Giant exist only on the branches `e3-smp` and `giant-smp`.

## Transitional Giant Exec SMP (`P4_GIANT=1`, decided 2026-10-02)

Not on this branch: preserved on branch `giant-smp` (`ff3395ba7f`).

Fabian changed the delivery strategy on 2026-10-02: instead of finishing the
fine-grained E3 lifecycle/semaphore protocols before hart1 ever runs Exec,
deliver a hardware-tested transitional "Giant" first. This is a **named
workaround**, not the E3 design and not a fix for the races E3 documents.

What it compensates: the absence of fine-grained, SMP-safe Exec
synchronization. Exec on a single CPU already protects every task list and
task state with Forbid() or Disable(); upstream AROS SMP makes both per-CPU
and then has to add a lock to every structure. The Giant instead keeps their
Amiga meaning system wide:

- F, held by a hart while its current task is inside Forbid() or Disable();
- D, held by a hart while its task is inside Disable(), and by every trap.

Lock order is F before D. Disable() takes F first; a trap holds D and may only
try-lock F (the dispatcher, for a task that resumes inside Disable()).
Both locks must be fair. Plain test-and-set starved hart1 for 30 minutes
under load (2026-10-02): every Exec call on hart0 releases and retakes F
within a few instructions. A waiting hart therefore announces itself, and
a hart about to take F or D, including the dispatcher's try-lock, first
lets an announced peer go, bounded at about 55 us.
Interrupt code takes only D, so interrupts still run while some task holds
Forbid(). Both lock words live in canonical internal SRAM (E2 qualified AMO).
ThisTask, ID/TD nesting, scheduler flags and quantum are per hart, selected by
mhartid with MIE masked. Every trap from task context moves to a per-hart ISR
stack through mscratch, because two harts must never share a task stack and
the dispatcher idles inside the trap. Hart1 has no tick of its own: hart0's
timer interrupt posts a tick IPI. Soft interrupts run on hart0 only. Before
hart0 suspends the cache to rewrite the flash MMU it parks hart1 in SRAM and
refuses the mapping if hart1 does not answer within a second. The shared
console, the cache controller's ROM commands and the SYSTIMER snapshot are
serialized across harts.

It needs no Exec ABI change: only the core image changes, and the unchanged
normal BSP package runs with it. `P4_GIANT` is a make switch of the normal
target, not the `smp` variant, which the port's kernel makefile refuses,
and Codex's E3 sources were removed on 2026-10-04 (branch `e3-smp`, see E3
below). Hart1 runs only tasks that opt in
through the private tc_Flags bits 1 (may run on hart1) and 2 (must not run on
hart0), which exec/tasks.h leaves unused.

Known limits of this stage: code that writes SysBase->TDNestCnt directly
(AROSTCP's fast Forbid) bypasses F; code that reads SysBase->ThisTask
directly sees hart0's task; Forbid() does not stop a task that is already
running on the other hart and needs neither lock; priority is not a mutual
exclusion between harts. RemTask() of a task running on the other hart evicts
it through an IPI. With `P4_GIANT_MIGRATE=1` every task created by
NewAddTask() may run on either hart; Exec's boot task and anything older
stay on hart0. A hardware driver must not hold a short-term lock across a
point where its task can be preempted, or it may migrate holding it (found
and fixed in the console lock, 2026-10-02).

Removal test, kept as its own item: each narrowing of the Giant (a subsystem
moved to its own lock) must show, on hardware, unchanged results of the
Giant self-test plus a contention measurement that motivated it. The Giant is
removed only when no Exec path depends on F/D for cross-hart exclusion.

## Design boundaries

- SoC core release/cache/IPI support belongs in the P4 kernel, not D1001 GPIO
  code. RV32 context, atomics and reusable CPU-local contracts belong in the
  shared RISC-V layer where proven generic. Preserve upstream Exec APIs.
- Never send hart 1 to `_start`: it clears shared BSS and uses the boot stack.
  Use a private SRAM entry/stack/report and private fault park. Until E3,
  hart 1 must not call Exec, DOS, HIDDs, allocators, console or device drivers.
  E2-A3 explicitly permits only CPU/CLIC/software-IPI control registers;
  it does not grant access to storage, display or touch peripherals.
- No second graphics producer until its ownership/locking is re-audited.
  `volatile` and a local `fence.i` are not cross-core coherence protocols.
  Account for both fetch pipelines, data publication, cache-off rendezvous,
  secondary quiescence before retained-scanout/PSRAM reset and bounded failure.
- Current local ESP-IDF v6.0.1 is the hardware reference: cpu_start.c
  start_other_core()/call_start_cpu1(), P4 cpu_utility_ll.h, and ROM
  ets_set_appcpu_boot_addr at 0x4fc000a8 in both ROM linker variants. Reimplement
  the minimal AROS sequence; do not import FreeRTOS initialization wholesale.
- Do not simply set the SMP build option: current P4 scheduling uses
  SysBase->ThisTask, and trap nesting is a global. Per-CPU runtime and a
  verified atomic/lock/IPI contract are prerequisites.

Existing source reuse must be reviewed, not presumed qualified:
`rom/exec/exec_intern.h` contains SMP list/runqueue locks, and
`arch/riscv-native/exec/exec_platform.h` and `kernel/tls.h` sketch spinlock
bindings and CPU-local state. Their TLS accessors use `mv x4, %0` with an
output operand, writing rather than reading the thread pointer; do not copy
them into P4. The shared `arch/riscv-all/include/aros/atomic.h` uses compiler
atomic builtins. The documented P4 toolchain already includes zaamo/zalrsc;
generated instruction and hardware contention checks are still required.

The P4 SMP primary entry explicitly initializes kernel-owned `tp` to zero;
CPU-local state is addressed through `mhartid`, not through native TLS. The
future production secondary entry must establish the same invariant before
calling C; trap/context restore continues to omit `gp` and `tp`. A local
GCC16.2 probe lowers C `__thread` to `__emutls_get_address`, with pthread and
allocation dependencies, rather than `tp`-relative relocations. This is not
general per-Exec-Task TLS qualification: untracked Tasks in the current pthread
implementation fall back to its main-thread slot. Early kernel TLS use and
foreign native-TLS modules remain unqualified; matched-link relocation and
per-Task TLS checks are prerequisites to SMP activation.

### Unselected secondary Exec-entry candidate (2026-10-02)

`kernel/p4_secondary_exec.c/.h` and `p4_secondary_exec_entry.S` compile
separately with the matched SMP defines. They propose a private16KiB stack,
kernel-owned tp=0, uncached generation-bound READY/GO exchange, owner-local
runtime/ISR/CLIC/IPI setup and genuine SC_DISPATCH ecall. They have no launch
controller or hardware caller and are deliberately absent from mmake members.
The SRAM entry calls ordinary Bootstrap/Dispatch text and references the
ordinary trap entry, all XIP on the flash linker. Owner1 cache-on handoff and
SRAM residency must be qualified without weakening the residency checker.
Isolated compilation is neither a matched-link nor runtime qualification.
READY must not expire while primary publishes online state; primary must own
bounded pre-GO timeout/reset recovery. Stack guards and validated online
publication remain open alongside public synchronization/lifecycle.

### Public single-semaphore routing candidate (2026-10-02)

The gated SMP mapping now selects `Exec_P4ObtainSemaphore` for generic
InternalObtain (exclusive/shared) and `Exec_P4ReleaseSemaphore` for public
Release. Normal compilation has neither hook. Uncontended/recursive acquisition
uses the existing semaphore gate without an arena scope; blocking acquisition
uses reserved nodes, clear-before-queue signal ordering and exact-generation
completion before Wait. Durable Release pokes the protected worker after gate
unlock. Refusal fails closed rather than returning apparent void success.
The actual adapter bodies plus real pins/registry/ledger/nodes pass host
O1/O2 sanitizer tests (379 baseline +466 new checks), independently repeated.
IRQ/gates, Wait/signals and task scheduling remain mocked. Review identifies
the generic TS_REMOVED pre-hook bypass and unresolved retirement between
reservation, completion and Wait; these must not become silent void success
or a fatal response to legitimate concurrent removal.
This is isolated-compiled source routing, not qualified public runtime support:
retirement integration/review, bootstrap, capacity-wait policy, LIST/Procure/Vacate,
correctness readers and removal lifecycle remain open. Do not combine legacy
stack/message queues with this candidate or remove configure activation refusal.
The failed-arm boundary in gated Wait now transfers a registry-proven retired
current RUN after Task unlock with interrupts masked. Root actual-body O1/O2
sanitizer455 and isolated RV32 compile pass; proof/transfer/scheduling remain
mocked; independent strict repeats455 and narrow source review accept only
this branch, not integrated runtime retirement. This is not the external-removal
protocol or a fix for public Obtain refusal and the generic pre-hook guards.
Public Obtain now probes the same retired-current boundary outside all gates
on reservation/configuration/acquisition refusal and before waiting on an
incomplete generation. Negative completion is fail-closed for live owners.
Root unretired O1/O2 regressions379+474 and isolated RV32 compilation pass;
actual public/registry retirement injection passes379+143 at O1/O2 for Acquire
refusal and incomplete completion, with SetSignal/Wait/transfer mocked.
Actual SetSignal now diverts failed effects after Task unlock, with MIE masked,
to no-return removal only for registry-proven retirement. Root/author actual
body sanitizer110 at O1/O2 and isolated RV32 compile pass; query/transfer are
mocked; independent narrow source review accepts the failed-effect branch,
not the successful-effect/unlock retirement race or full transfer. The composed production
retirement path is not qualified. Pre-hook guards, external removal and
signal-clear semantic compatibility still block activation.
The analogous failed-effect SetExcept path now has the same retired-current
diversion; actual Signal/SetExcept wrapper/protocol95 passes author/root
O1/O2 sanitizers and isolated RV32. Task/lifetime/query/transfer are mocked,
and poison starts only inside the exit mock. Independent narrow source review
accepts local ordering; successful SetExcept WAIT-to-READY is a fixture gap.
Actual simultaneous signal writer/removal closure remains unproven.
Removal-bank startup routing passes strict isolated SMP/normal RV32 compilation;
the normal object has no removal hook references. Full link and runtime remain
unqualified.
Actual attached-bank retirement/cancellation passes root/author O1/O2 sanitizer
151 plus admission384 with real registry/ledger/bank bodies; IRQ/locks/wakes
remain mocked. Independent narrow production and bootstrap-routing reviews
accept those source boundaries, not public RemTask, actual final-free ACK or
concurrent Exec execution.

### External removal binding constraints (2026-10-02)

Public external RemTask must bypass the generic debug/state/free path before
any target dereference. Existing `krnP4TaskRetireLocked` serializes Task and
READY/WAIT/RUN queues, publishes service retirement plus ledger cancellation,
then detaches the node. Existing retained service claim is eligible only after
RUN0 and all non-service pins drain. Keeping an ordinary target pin until ACK
would prevent that claim and deadlock cleanup; do not use that design.

The binding needs task-independent, generation-bound completion storage with
separate service and requester references, one completion per requester, and
target and requester registry indices. Publication must atomically establish
the requester's designated reference and completion ownership before target
retirement, under the registry gate and existing Task/queue lock order. A
caller-stack ticket or a pointer held across an unprotected allocation/preempt
window is not sufficient: retiring the caller can abandon that continuation.
Boot-reserved publication storage avoids that window; exhaustion still needs
a nonfatal, progress-capable policy before runtime acceptance.

Requester retirement must cancel its ticket interest and discharge its
designated reference without consuming the service's cleanup reference. The
reference belongs to the task-independent ticket, not necessarily to Task
storage: the selected integration direction is no ordinary requester pin held
across the completion wait. Its registry-owned handle must be cancelled under
the registry gate before requester cleanup becomes eligible; a recipient Task
pin is acquired only for durable notification delivery. This is an ownership
decision. Requester-wide cancellation is now bound to successful registry
retirement as recorded below; public publication and ACK remain open. Multiple
removers need separate generation-bound acknowledgements, not one stack wake.
The service must finish context/ETask/memory-entry cleanup before ACK, while
the independent target registry record remains indexed. ACK must establish a
durable recipient notification reservation before releasing the gate; a wake
is only a hint, and a requester returns only after exact generation completion.
No target fields may be read after its first possible freeing operation.

Registry minimum-pin floors, reclaim predicates, retained-service validation,
requester cancellation, ticket reuse/ABA, notification delivery, concurrent
joiners during cleanup and topology must be implemented together. A new ticket
leaf alone is not public removal support. These are integration constraints,
not a completed implementation; configure refusal remains mandatory.

The fixed portable bank and requester-wide cancellation prerequisite now pass
root actual-source sanitizer116 at O1/O2 and strict isolated RV32 compilation.
Independent narrow review accepts that local snapshot. Permanent boot-prefix
storage now passes author/root O1/O2 actual-source composition81982 (mostly
byte-clear assertions) and strict isolated RV32. SMP startup source prepares
and attaches the fresh bank before Exec vectors/allocator conversion; only
SMP source lists select it. The actual pins binding cancels requester interest
after successful retirement under the same gate before readiness wake/unlock;
dedicated actual-source binding151 plus admission384 pass author/root O1/O2
sanitizers, and non-author narrow review accepts that ordering. Bootstrap
source-routing review also passes; mocked IRQ/locks/wakes and isolated startup
objects do not establish a linked or running SMP system. No public reserve/join,
recipient reservation or post-free ACK/detach integration existed at that
qualification point. Actual cleanup Finish now selects a fused registry/bank
transaction after final FreeEntry: exhaustive bank/recipient/headroom preflight,
durable SIGF_SINGLE batches, all target ACKs and target detach in one gate.
Isolated RV32 registry/pins compile and legacy non-SMP regressions pass; new
transaction726 and actual pins binding87 (each with admission baseline379)
pass root strict O1/O2 ASan/UBSan repeats. Independent review accepts corrected
V2 local ordering/accounting, preserving the earlier alias-order defect and
correction. Notification Claim/Finish executes metadata/pin accounting only,
not actual SIGF_SINGLE delivery or concurrent operation. The old void
Finish still fails closed on headroom refusal; post-free retry ownership and
progress wake must be bound before public admission/activation.
Optional actual cleanup/registry/bank composition195 O1/O2 sanitizer checks
pass: Task is poisoned after first FreeEntry, ACK stays false through all
entries, and final fusion reserves recipient pins with ACK before record free.
Claim/finish gate wrappers and allocator/context/topology are mocked; actual
pins binding, real delivery/Wait and concurrency are not covered by this test.
Normal cleanup171 regressions also pass.
Current selected SMP cleanup now replaces that void call with postfree Try/Retry
bindings. Each retained record has a validated service_postfree marker set only
after final Task/context/entry freeing. Pin/count pressure retains that record
and retry interest under the same gate; the retry scan skips blocked records,
returns one detached record for out-of-gate FreeMem, and permits fresh cleanup
when no retry can progress. Resource drops and requester cancellation latch
wakes; blocked retries do not self-poke. New-batch generation exhaustion is
separately -2/fail-closed and still requires pre-free public admission policy.
Fresh strict isolated RV32 registry/pins/cleanup compile. Root O1/O2 sanitizer
repeats now pass actual pins retry674+admission baseline379 and actual cleanup
composition597, with normal cleanup171 preserved. Cleanup pin wrappers are
shims around actual registry leaves; the separate pins fixture compiles the
actual wrappers. IRQ/locks/wakes/allocator and real SIGF_SINGLE/Wait remain
unqualified. The initial all-record scan review identified
IRQ-masked gate occupancy risk; current source limits each retry invocation to
eight attempts and retains only scalar address/blocked/restart state across
slices. Capacity events and newly marked records below the cursor request a
revisit; remaining slices latch continuation wakes, while blocked-only passes
eventually become quiet. The per-fusion latency still needs measurement.
Independent follow-up source review accepts the current slice/cursor ordering;
it does not execute retries or qualify concurrency. Host tests now cover ten
retained records crossing slices and a lower-address resource-event restart;
a newly marked record below an advanced cursor remains a dedicated test gap.
Read-only audit supports one globally issued service-target generation reserved
before irreversible retirement, reused across identity-tagged recipient batches.
Current source implements that token in plain and ledger service retirement,
checking capacity before irreversible cancellation and clearing it at detach.
Final fusion no longer allocates a serial after free. Normal non-SMP keeps the
token zero. Strict isolated RV32 registry/pins/cleanup/publish compile and O1/O2
retry/cleanup/normal regressions pass. Root O1/O2 generation324+baseline379 now
tests plain/ledger pre-free exhaustion, mismatched cancellation/arena refusal,
service-token lifetime, identity-tagged shared fan-out and old-but-distinct
inflight/pending generations. Late joins are leaf-level composition, not public
admission. No injected post-validation cancel failure is claimed. Independent
source review accepts local ordering/lifecycle,
not execution. Revised transaction713+baseline379 and actual pins binding88+
baseline379 pass root O1/O2 sanitizers under the new pre-free policy. Legacy
detached service claim clears the token; final-free ACK
removal must use retained claim/fuser. Public admission is still open.
Earlier passing artifacts are historical
qualification points, not validation of these new fields/bindings.
Per-call full-pool validation makes repeated fan-in
iteration/ACK potentially quadratic; the final gate-owned batch must avoid
that latency before concurrent runtime qualification.

## Delivery and acceptance

### E0: baseline and isolated entry foundation

Dependencies: none for source/build work; preserve current running baseline.

- E0-A1: separate SRAM entry masks interrupts, avoids shared initialization,
  has a private aligned stack/report and private fault park, with no C calls.
- E0-A2: RV32 assembly/isolated linking verifies section placement, 64-byte
  entry/trap/report alignment and 16-byte stack alignment. Intentionally XIP
  placed fixture must fail the residency checker. Full-core size/link audit
  is a separate gate; standalone ELF is never flashed.
- E0-A3: before disturbing hardware, retain the known core/BSP/image and
  write-range backups; qualify existing delayed-hang behavior separately.

### E1: bounded HP-hart release and park

Dependencies: E0, audited ROM/revision/register and report-visibility contract.

- E1-A1: opt-in release starts mhartid=1, records entry/fault state and parks;
  hart0 remains sole Exec owner. Bounded timeout returns to safe single-hart
  operation with secondary reset/stall confirmed. No unbounded IDF loops.
- E1-A2: positive report plus suppressed/wrong-report counter-probes; private
  stack guards intact. Twenty warm resets with secondary retained state
  repeatably recover PSRAM and boot; no physical-cold-start substitution.
- E1-A3: untouched baseline is restored after diagnostic testing until a
  separate user-consented desktop/touch regression passes.

### E2: two-hart primitives, not task scheduling

Dependencies: E1.

- E2-A1: request/ack sequence and ownership transfer pass SRAM and PSRAM
  checksummed exchanges, stale/duplicate/timeout refusal and reset recovery.
- E2-A2: atomics are verified against P4 ISA/toolchain, generated instructions
  and real concurrent contention; no unsupported A-extension assumption.
- E2-A3: per-hart CLIC and software IPI route/ack, remote fence.i and cache-off
  rendezvous have bounded positive/failure tests. Counter-probes show tests
  detect missing publication/ack rather than accepting local cached data.

E2 initially used a SRAM-only slice under `P4_E2_MAILBOX=1` (requires
SECONDARY_PROBE, excludes SECONDARY_RETAIN). The private bounded worker has
no Exec/console/peripheral/allocator calls. Request and response each occupy
a separate 64-byte lane; input/output are independent 1-KB ranges. Both
harts access these through the internal-SRAM uncached alias, never through
cached/uncached aliases concurrently. One writer owns each lane and payload.
Payload/epoch/sequence/length/checksum precede release-fenced ticket
publication; acquire the matching response ticket before validating output.
The candidate attempts two reset-separated epochs, 130 exchanges, 12 refusals
and two missing-ack timeout counter-probes. Every exit stops hart1 before
logging and parks hart0 before Exec. Worker and primary waits have cycle and
iteration bounds. PSRAM exchanges/coherence were still open at that milestone;
host fixtures do not establish hardware memory visibility. On2026-10-01,
the exact206,560-byte SRAM candidate passes three ordinary headless D1001
captures (390 exchanges,36 refusals,6 expected missing-ack timeouts), each
ending with secondary reset-held/clock-off. The full normal flash range is
restored/verified afterward. This verifies only the SRAM slice; no no-retry
warm-reset, PSRAM, atomic, IPI or Exec SMP acceptance follows from it.

The full E2 diagnostic (`P4_E2_PRIMITIVES=1`, requires E2_MAILBOX and
SECONDARY_PROBE) reserves 4 KiB immediately below the framebuffer outside
BSP placement and Exec memory. It tests PSRAM with shared-cache handoffs
and alternating forced physical reloads, including publication/refusal
counter-probes. Atomics use only canonical internal SRAM, never PSRAM or
mixed cached/uncached aliases. Bounded single-attempt LR/SC loops avoid
compiler-generated unbounded CAS retries. Both private interrupt handlers
preserve caller registers and trap CSRs and never call shared Exec traps.
Software IPI sources 79/80 route to raw CLIC line22 on their respective harts;
each acknowledgement includes its hart and generation and executes fence.i.
Cache-off requires a matching remote SRAM park acknowledgement and an
internal primary stack. An expired remote rendezvous parks fail-closed.
The full candidate must pass exact-artifact headless gates before acceptance;
it always stops the secondary and parks before Exec. All defaults stay off.
On2026-10-01, the exact219,776-byte core08 (SHA256
f26186e41d04469e7de4d0b6a35e94882a9bfc25e8630f22138abae2dd4512bc)
passes all five captures/ten epochs below; E2-A1/A2/A3 are complete.
The full normal221,184-byte flash range is restored/verified and normal
headless PSRAM/Wanderer/touch boot passes. Detailed hashes, failed earlier
candidates, build/relink procedure and remaining risks are in ROADMAP.md.

Acceptance campaign: five ordinary 18-second headless captures of one
unchanged candidate, two reset-separated epochs per capture, every stage
passing and every secondary exit reset-held/clock-off before final READY.
The ordinary reset helper may retry ROM-download starts: this is not the
E1 no-retry retained-hart campaign or a physical cold-boot qualification.
The log checker requires both MISA A bits and nonzero measured CAS retries
in each epoch. Both harts snapshot the same CAS value before release of
their first competing update; at least one real attempt must lose/retry.
The missing-park counter-probe calls the actual cache-window admission
function, verifies zero cache-off admissions, then verifies exactly one
admission with a current remote SRAM stack and restored predictor bits.

E3 must keep atomic/lock words in internal SRAM or independently qualify
another region. Generic atomics on PSRAM-allocated objects are NOT qualified
by E2. The tested diagnostic IPI/cache rendezvous must be integrated with
CPU-local scheduler/runtime state before ordinary tasks run on hart1.

### E3: experimental Exec SMP

**Status 2026-10-04: E3 removed from this branch, kept on branch `e3-smp`.**
At Fabian's direction E3 was taken out in two commits. The shared half: the
`EXEC_PLATFORM_*` hooks only E3 defined and the `__AROSEXEC_SMP__` changes in
`rom/exec`, `rom/task` and `rom/kernel`, task.resource's direct core binding
(`task_bootstrap.c`, `taskresource_cleanup.h`) and
`EXEC_DISPATCH_LAUNCH_IN_CPU`. The port half: the `smp`-variant sources in
`exec/` and `kernel/`, the `P4_E3_*` diagnostic switches (`CPU_LOCAL`,
`SPINLOCK`, `SOFT_ATOMIC`, `FPU`, `FPU_TEST`, `RUNTIME_PREPARE`), the atomic
service behind `KATTR_AtomicOps` (`aros/atomic_ops.h`,
`aros/platform_atomic.h`), the P4 timer service (`aros/p4timer.h`), the
isolated secondary-Exec candidates, the host tests of all of these and the
tools `smp-e3-check.py`, `p4-fpu-check.py` and `configure-smp-build.sh`. None
of it built for the P4 (the `smp` variant is refused); the shared half
changed the x86_64 and opensbi SMP builds and collided with upstream's later
SMP rework. Both normal and Giant cores, and the E2 diagnostic core, are
byte-identical before and after.

Kept on this branch: E1/E2 (secondary entry and probe, mailbox, primitives,
`smp-e2-check.py`, `smp-retained-reset.py`) and the generic fixes that came
in their own commits (ChildWait signal order, NewCreateTaskA failure paths,
the task.resource expunge). The generic fixes that came in only with E3
(task.resource hook node, list locking and init order, NewAddTask failure
paths) are not on this branch; they are to be offered upstream from
`e3-smp`. The Giant is not on this branch either (`giant-smp`).

To bring E3 back, start from branch `e3-smp` (`14f0290e21`, the last commit
with all of it; local, not pushed). Everything below that names those
switches, files or tools refers to that branch. The requirements stay as the
design record for a later fine-grained SMP.

Dependencies: E2 and explicit review of shared Exec SMP integration.

- E3-A1: CPU-local ThisTask, interrupt nesting, scheduler state, idle/timer,
  FPU ownership and context/TLS are correct on both harts. Runqueue, allocator,
  signal/message and semaphore locking obey documented lock order; audit
  Forbid/Disable semantics and compatibility before ordinary tasks migrate.
- E3-A2: two CPU-bound tasks demonstrate concurrent progress and CPU identity;
  context/FPU isolation, signals, messages, locks and affinity pass contention
  and failure tests. Reuse existing Exec SMP support where applicable.
- E3-A3: all modules whose SMP ABI changes are rebuilt as one matched core/BSP
  set. Do not mix single-core stubs or a stale generated include tree.

E3-A1 is delivered in smaller steps without weakening the complete gate:

Current production launch gap (2026-10-01, E3-RT): both runtime slots are
prepared on hart0, but the secondary Exec entry, actual worker-ready launch
consumer, local ISR/runtime/IPI setup, validated ACK/online publication and
first-context restore chain are absent. CPU count remains1. The diagnostic
secondary trap/4KiB stack/park is not this chain. Keep the configure refusal
and hart1 task admission disabled until public synchronization/lifecycle and
this launch path are coherently bound. A first dispatch requires a real
common trap/restore frame, not synthetic trap depth. The ordered source map
and its headless phase/FPU/contention gates are recorded in ROADMAP.md.

1. Internal-SRAM CPU-local foundation (`P4_E3_CPU_LOCAL=1`, diagnostic only,
   requires E2_PRIMITIVES): two cache-line-separated owner-only slots,
   bounds-checked mhartid lookup without modifying tp, explicit reset-state
   initialization while hart1 is held in reset, nested-depth underflow and
   overflow refusal. Two harts each mutate only their own slot for 8,192
   iterations; acquire the completion publication before inspecting the peer.
   Invalid hart IDs and wrong-owner mutations must fail without side effects.
   Reinitialize and repeat in a second reset-separated epoch. Require all E2
   stages and confirmed secondary reset-held/clock-off on every exit.
2. Production SRAM lock qualification (`P4_E3_SPINLOCK=1`, diagnostic only,
   requires CPU_LOCAL/E2): packed read/write state with placement rejection,
   one constrained LR/forward-compare/SC attempt, bounded external retries,
   forced writer/read refusal, two shared readers and protected updates.
   This is not yet the public KrnSpin owner/fail-hook or scheduler contract.
3. Runtime integration: ThisTask, ID/TD nesting, scheduler flags, trap depth,
   idle/timer accounting and FPU/context ownership must use CPU-local state.
   Exercise real trap and context switching, not synthetic depth helpers.
4. Exec synchronization and matched ABI: audit lock placement, runqueue and
   allocator order, signal/message/semaphore paths and Forbid/Disable before
   permitting task migration. Rebuild all ABI consumers together.

Private cached-PSRAM synchronization is now a separate prerequisite gate
(`P4_E3_SOFT_ATOMIC=1`, requires SPINLOCK/CPU_LOCAL/E2). Each reset-separated
epoch executes4,096 ADD32 operations per hart through one SRAM-backed service,
checks final8192 and returned-old-value sum33550336,8/16/32-bit widths/wrap,
guards and alias/alignment/out-of-policy refusal. MIE is masked before taking
the shared lock. Probe operands occupy only the first64 bytes of the owned E2
scratch and are restored at the guard/cache-test boundaries. No native PSRAM
AMO/LRSC or public atomic-header substitution is permitted by this gate.

Remaining runtime integration order (full E3 still open):

Current source checkpoint: software CAS is implemented through the private
SRAM-serialized service, and staged BootstrapPrepare/BootstrapBind/Finalize
runtime state passes 149 sanitized host checks. The guarded SMP trap body
passes 21 sanitized nesting/isolation/supervisor-state checks; KrnIsSuper and cpu_Switch select
CPU-local state. The P4 Exec_PreparePlatform hook binds SysBase before its
first runtime macro. Staged-bootstrap/CAS and public-spin independent reviews
pass their stated contracts; real SMP build/hardware gates remain pending.
A fully invalidated normal kernel rebuild remains exactly
byte-identical to the verified normal 201,504-byte baseline. No second-hart
Exec release or configure bypass has occurred. A public atomic macro/service
candidate is now implemented and awaiting independent review; it has not reached a
matched whole-system SMP build or hardware. The normal image identity above
precedes the later shared task-creation error-path fixes (BSP rebuild pending).

1. Adapt public KrnSpin owner/failure-hook semantics and all shared task-field
   accesses to a single audited synchronization protocol; the private service
   does not make plain Wait/SetSignal/Exception accesses safe automatically.
2. Integrate CPU-local ThisTask/TaskRunning membership, ID/TD/scheduler state,
   actual trap depth/FPU and timer ownership with the affinity-aware runqueue.
3. Add online CPU mask and remote scheduler IPI before secondary Exec release.
4. Build core/BSP and every changed ABI consumer against one refreshed generated
   include/Developer tree; override the relocated compiler's stale sysroot.
5. Only then qualify real concurrent Exec tasks, affinity, signals/messages,
   semaphores and per-hart FPU isolation under contention and failure, followed
   by baseline restoration and separately synchronized interactive regression.

The first diagnostic stores sentinel identities, not real Exec tasks or
libraries. Passing it does not qualify scheduling, actual FPU isolation,
timer delivery or normal KrnIsSuper semantics on hart1. The normal core and
its default behavior remain unchanged until runtime integration is tested.

CPU-local foundation acceptance (2026-10-01): exact core02, 222,144 bytes,
SHA256 `719b2ec327795cebc92784b187f940fad0133471d37383633121e5b33312f2d9`,
passes five ordinary headless captures, ten reset-separated epochs and
163,840 owner iterations. All E2 prerequisite stages and every secondary
reset-held/clock-off teardown pass. Both harts have MIE masked during the
CPU-local exercise; hart0 restores only its original MIE bit afterwards.
The production CPU-local implementation passes 34,123 ASan/UBSan host checks;
25 E2/E3 parser tests pass and the E3 parser rejects an actual E2-only log.
This closes only the first diagnostic delivery above, not E3-A1, A2 or A3.

SRAM lock diagnostic acceptance (2026-10-01): core06,224,912 bytes, SHA256
`13e536307bc1cf0f92310e559255d41085bf540991a071973c52eb89bca3c8a9`,
passes five18-second captures/ten reset-separated epochs,81,920 protected
writes, forced contention, shared readers, placement refusal and all prior
E2/CPU-local gates. Every secondary exit is reset-held/clock-off.115 sanitized
host lock checks and29 parser tests pass; independent review of the actual
linked RV32 LR/SC intervals confirms three instructions with only a forward
comparison between LR and SC. Evidence is in build
`evidence/smp-e3-lock-runtime-2026-10-01/`.

Guarded runtime mappings pass68 sanitized host checks and preserve generic
non-SMP macros. They remain preparatory, gated by `P4_E3_RUNTIME_READY`, with
no actual startup/scheduler binding enabled. StackSwap's SMP branch uses
FindTask(NULL); its normal text is byte-identical to the previous assembly.
Neither synthetic runtime tests nor the private SRAM lock qualify ordinary
Exec scheduling, signals/semaphores or task migration. Full E3 remains open.

Source audit decisions for runtime integration (2026-10-01):

- Current source checkpoint: independent staged-bootstrap review passes after
  correcting variant source selection and MIE publication ordering. Guarded
  startup prepares the software-atomic policy before Exec initializes locks;
  36 sanitized production-body ownership checks pass (transport/capture mocks).
  High SRAM now uses live L2 carve-out validation at Prepare, with 2,594
  software-atomic host checks including rejection without publication and copied
  high-bank bounds. Independent CPU-mask review passes; it found a shared
  Exec_InitETask caller allocation-failure defect, now corrected with 13
  production-prefix sanitizer checks and awaiting followup review. Public-spin
  source/helper/object verification passes 93 checks, independent review runs.
  The rebuilt normal core remains byte-identical (201,504 bytes; SHA-256
  `1d77ae9251e702b00ac68163bdcf61944f34f5a1207a90afa8fc5a8ad1a645e6`).
  No matched SMP build or actual concurrent Exec tasks have been qualified.
- P4 currently selects the generic `rom/kernel/kernel_scheduler.c`, not the
  native RISC-V affinity/runqueue implementation. P4's direct
  `SysBase->ThisTask` and global trap-depth paths must be replaced together
  with scheduler/nesting/quantum state, not independently called SMP-safe.
- `rom/kernel/getcpucount.c`, `getcpumask.c` and `schedulecpu.c` currently
  report one CPU, leave the mask unset and schedule locally. A real online
  mask and remote scheduler-IPI protocol must precede task migration.
- Embedded spinlocks are not the whole atomic surface: `Signal`/`Wait`
  manipulate task fields atomically, and semaphore owner/waiter transitions
  still rely on local Forbid. Audit these paths before choosing SRAM-backed
  lock indirection; it cannot silently qualify PSRAM atomic accesses.
- Semaphore audit checkpoint: InitSemaphore initializes its embedded spinlock,
  but current obtain/release/procure/vacate paths still use only per-hart
  Forbid/Permit at the audited baseline. Queue/count/owner transitions need one consistent shared lock
  protocol, released before Wait/Signal/ReplyMsg/PutMsg. A separate Luna audit
  completed stack-request lifetime and lost-wakeup tracing; implementation and
  deterministic interleaving tests are in progress before integration;
  this is a known missing runtime prerequisite, not a private-lock test failure.
- Detailed task-field audit: Signal updates tc_SigRecvd (32-bit OR) and
  tc_Flags (8-bit OR); Wait and semaphore waits use32-bit AND. Other accesses
  in Wait/SetSignal/Exception are plain reads/writes and must join the same
  synchronization protocol. MEMF_PUBLIC task/ETask allocations may be PSRAM;
  auditing only embedded spinlocks or atomic macros is insufficient.
  E2 does qualify staged cached-PSRAM producer/consumer handoffs, sometimes
  without cache flush, but not competing PSRAM RMW. Direct PSRAM LR/SC remains
  prohibited. The private single SRAM-lock service for audited8/16/32-bit
  load/store/RMW and CAS is implemented with local IRQ save/mask/restore and
  full fences; its ADD32 hardware prerequisite has passed. The public
  checked bridge/macro candidate is awaiting independent review, not qualified
  for actual concurrent Exec use. A single lock provides
  the initial global ordering; hashed stripes need a separate SeqCst proof.
- Generic `KrnSpinInit`/`KrnSpinLock`/`KrnSpinUnLock` are stubs, not a usable
  P4 SMP backend. The private internal-SRAM packed-state lock algorithm is now
  hardware qualified. P4 public placement, owner and failure-hook adapters
  now pass independent source/object review and 93 mock fixture checks, but
  concurrent Exec/hardware qualification remains open; this alone does not
  resolve task-field atomics or semaphore queues.
- ExecBase is created using the low internal-SRAM MemHeader, so its embedded
  global locks are not automatically PSRAM locks. Task/semaphore/message
  allocations have separate placement and lifetime obligations.
- `--enable-target-variant=smp` now fails explicitly for P4 until reviewed
  runtime prerequisites are integrated. The report/refusal-only
  `tools/configure-smp-build.sh` cannot override this gate.
  Add explicit experimental configuration only after runtime contracts are
  ready; use a separate configured tree and refreshed copies of public and
  generated headers for the matched core/BSP build. Do not force macros on
  the existing single-hart module set.

### E4: graphical qualification and default decision

Dependencies: E3; separate closure of the prior delayed slowdown investigation.

- E4-A1: exact-artifact boot/reset, read-only storage, touch/menu/double-tap,
  window drawing and scanout regressions pass with fresh readiness for visual
  tests. Measure throughput/latency against the same single-hart workload.
- E4-A2: sustained concurrent CPU/SD/PSRAM/graphics/touch workload has explicit
  duration, workload, timeout and integrity counters; preserve complete logs
  and hashes. Define thresholds before running; no inferred speedup.
- E4-A3: SMP remains opt-in until qualification and a documented default
  decision. B5R and battery-cold-start obligations remain separate.

## Migration, risks and verification cost

Reproduce E0 from the configured build directory with the checked-in test:

```sh
sh /Volumes/Dev/Source/Amiga/AROS/arch/riscv-esp32p4/kernel/tests/secondary-entry-test.sh \
  /Volumes/Dev/Source/Amiga/AROS-ESP32-build/bin/darwin-aarch64/tools/crosstools \
  /Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-foundation-2026-10-01/repeat
```

These are this workspace's current paths, not an SDK installation contract.
The command has been executed verbatim; both fixtures are diagnostic only.

E1 actual-controller host regression (no hardware):

```sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  /Volumes/Dev/Source/Amiga/AROS/arch/riscv-esp32p4/kernel/tests/secondary-probe-test.c \
  -o /Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-release-2026-10-01/secondary-probe-test
/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-release-2026-10-01/secondary-probe-test
```

Current build output is the freshly rebuilt normal core (201,504 bytes, SHA256
`1d77ae9251e702b00ac68163bdcf61944f34f5a1207a90afa8fc5a8ad1a645e6`),
byte-identical to the saved baseline prefix with every E3 test flag off.
Startup core01 was rejected before flashing: KernelBase is initialized later
than its original hook point. Core02 boots, but its truncated metadata report
does not pass the gate. Startup core03 now passes three complete metadata
reports followed by headless Wanderer/touch-heartbeat boot. Private PSRAM
core02 was rejected for E2 guard failure; corrected PSRAM core03 passes five
captures/ten epochs and81,920 adds with all E2 gates. The normal core range is
now restored and independently digest-verified.
core03 passes three complete headless hart0 task-isolation runs; core04
passes five further complete runs with hardened transport. The board's
full 262,144-byte baseline range was restored before the startup campaign from
`evidence/smp-e3-lock-runtime-2026-10-01/core-range-before-262144.bin` and
independently digest-verified; a35s headless boot reaches Wanderer and
the touch heartbeat. The normal core itself remains 201,504 bytes.
Before changing SECONDARY_PROBE or other E3 flags, invalidate kernel objects;
mmake does not track compile-flag changes. The aggregate target also hits the
separate oversized 4-MB flashdisk-stage dependency; do not expand or rewrite
that partition as an SMP workaround.

CPU-local host and saved-log regression (run from the repository):

```sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  arch/riscv-esp32p4/kernel/tests/cpu-local-test.c -o /tmp/p4-cpu-local-test
/tmp/p4-cpu-local-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s arch/riscv-esp32p4/tools/tests -p 'test_smp_e[23]_check.py'
PYTHONDONTWRITEBYTECODE=1 python3 arch/riscv-esp32p4/tools/smp-e3-check.py \
  /Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-e3-cpu-local-2026-10-01/boot-02-0{1,2,3,4,5}.log
```

After invalidating all kernel objects, build the P4 architecture target,
then `kernel-kernel-riscv` (shared RV32 architecture overrides), then
`kernel-kernel-kobj`, then the direct core binary target. Building the generic
aggregate before the RV32 overrides selects incompatible fallback objects.
Use `P4_SECONDARY_PROBE=1 P4_E2_MAILBOX=1 P4_E2_PRIMITIVES=1
P4_E3_CPU_LOCAL=1` with the unchanged graphical/touch/200-MHz PSRAM settings.
Require successful SRAM residency checks and a newly created binary; a
failed link must never be followed by a flash of the old generic output.

Early stages are deliberately independent of the desktop image. No flash of
an isolated test ELF or changes to bootloader/partition/storage. Any diagnostic
core write requires identity, exact range, recovery backup and matched-artifact
verification. Headless testing is authorized; interactive gates require fresh
readiness. A hang may destroy diagnostic state on USB-console reopen; avoid
reopening solely to inspect a retained failure.

E1/E2 qualify bounded release, internal-SRAM atomics, publication, software IPI
and remote instruction replacement on the tested rev1.3 board. Remaining
unknowns include ordinary Exec scheduling/traps/timers/FPU migration,
non-SRAM atomic placement and shared Exec ABI/module integration. These
diagnostics do not qualify all silicon revisions or arbitrary memory regions.
No schedule or speedup estimate is justified yet.

### E1 diagnostic implementation contract (2026-10-01)

`P4_SECONDARY_PROBE=1` includes the private entry and controller; it does not
enable Exec SMP. Primary startup holds core1 in global reset with its CPU
clock gated, checks both control registers before clearing shared BSS, and
clears the retained boot address. The late probe runs after BSP/PSRAM setup,
before Exec, and always resets/gates the secondary again before continuing
into Exec. `P4_SECONDARY_RETAIN=1` requires SECONDARY_PROBE and instead
retains a verified hart1 private park, keeps its clock on/reset clear and
parks hart0 before Exec; it never launches a second scheduler. Every negative
or invalid retention path still stops hart1. Only a valid suppressed-launch
counter-probe plus validated hart1 report/guards/control readbacks can produce
the retained READY marker. This marker is a reset-test prerequisite, not a
desktop-boot assertion.
Failure to confirm isolation withholds Exec. Stall status is not accepted as
proof because IDF documents its WFI limitation.

The mailbox and two 64-byte guard lines use the internal SRAM uncached alias
(`+0x40000000`) on both harts. Hart0 evicts the initial cached BSS aliases
before initializing them, publishes the entry through `krnP4SyncCode`, and
hart1 executes its own `fence.i`. This is a limited diagnostic contract, not
qualification of general cached/PSRAM cross-hart access. Reference:
[ESP-IDF memory synchronization](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32p4/api-reference/system/mm_sync.html).

Reports require state=entered, hart=1, the matching launch nonce and private
SP; faults/mismatches fail. The wait has both a 36-million-cycle deadline
(100ms at CPU360) and one-million-iteration cap. A suppressed launch must
time out before the real launch. Host tests exercise the actual controller
with mocked MMIO/cycles; they cannot establish silicon/cache behavior.

The connected rev1.3 ROM was read without writing flash: the setter stub at
0x4fc000a8 jumps to 0x4fc058e2, whose implementation is `lui a5,0x50110`,
`sw a0,356(a5)`, `ret`. Thus the diagnostic writes the proven core1 boot
register 0x50110164 directly; it imports no ROM/IDF wait loop. This evidence
does not qualify other silicon revisions. On2026-10-01, fresh campaign02
passes twenty consecutive retained-hart warm resets with core1 reset clear
and clock on at AROS entry; exact baseline range restoration/verification
and normal headless boot also pass. The earlier user-interrupted campaign
is not combined with these passes. Following explicit readiness, Fabian
confirms the requested restored desktop/pointer/two-finger-menu regression
after a synchronized60-second normal boot. E1's bounded diagnostic gates
pass on D1001 rev1.3; E2's bounded SRAM slice now passes as recorded above,
and Exec remains single-hart. Prior sustained
stability, complete aggregate packaging and physical-cold-power gates are
not closed by this result.

Retained reset gate: keep one serial connection through an initial seed
boot and twenty consecutive single-pulse USB EN resets. Each prior boot
must end in retained READY before the next pulse. Each next boot requires
exactly one ROM banner, CHIP_USB_UART_RESET/run mode, valid pre-BSS isolation
snapshot, recovered 32-MB/200-MHz PSRAM with one-word-per-MiB window checks,
35 BSP modules and the complete retained probe sequence. Any missing,
duplicate, download, fault or timeout aborts the series; no hidden retries
or replacement boots count. Save partial logs and reset-pulse counts.
Record inherited core1 reset/clock controls as observed at primary entry;
the ROM/bootloader may already normalize them. Success qualifies warm
recovery from a previously released parked hart, not proof that its running
state survived ROM or a physical battery-cold-start gate. A retained-hart
failure must be quiesced via the diagnostic boot before restoring a normal
core which lacks this opt-in early-stop code.

## Decision changes

2026-10-01: SMP-only trap entry now switches to per-hart ISR stacks through
mscratch before saving the frame. Nested traps keep the ISR stack; return
restores the outer ISR top and leaves MIE clear until mret. The 288-byte FPU
frame is mandatory for SMP. Primary startup holds hart1, initializes CPU-local
runtime and installs its stack before kernel C. Trap handling checks guards,
frame range, a 2-KiB C margin and maximum depth eight. Storage and trap-handler
fixtures each pass 28 sanitizer checks; the actual preprocessed trap assembly
passes the bounded QEMU M-mode fixture (author and root), covering nested
entry, original SP, markers, integer registers, simulated stack migration
and deferred MIE restoration. Independent review reproduces these results. Fixed margins
are not measured stack limits;
pre-handler stores, synchronous entry/exit faults and hardware nesting remain
qualification risks. SMP-only source selection does not remove configure's
full-runtime refusal or qualify the SRAM budget by a complete link.

The Task registry/pin binding is also selected only for the gated SMP kernel.
Its actual-source fixture passes 80,651 assertions under ASan/UBSan (author
and root); these include repeated timeout-loop assertions, not that many
independent scenarios. Creation hooks are now source-integrated; removal and semaphore cancellation remain
unwired. Ready-to-Running admission now reserves a unique lifetime owner and
publishes iet_CpuNumber before TS_RUN and Running-list insertion, under the
canonical Task lock and Ready/Running locks. This helper does not select a
Task, install ThisTask/runtime/context or release the outgoing owner; its
host fixture passes 528 sanitizer checks, and independent review finds no
ordering defect. Current queue RV32 object
is 8,052 bytes, SHA-256
94928e9714064ff84872af4a74a13078278fa6b2bc26a16bc663ca399a5314ab,
including the new creation/bootstrap transactions (their fixtures are in progress).

SMP cpu_Dispatch now selects krnP4Dispatch rather than generic core_Dispatch.
The new dispatcher snapshots/pins identities under Ready, drops Ready before
taking a canonical Task lock and revalidates through lifetime-backed admission.
It installs per-hart ThisTask/nesting/quantum/counters, releases all temporary
pins, and returns a task protected by its unique running owner for CPU restore.
Its strict standalone RV32 compilation and 1,614 actual-source sanitizer
assertions pass (author and root); independent review finds no remaining
local dispatcher ordering defect after the guard correction.
Independent review caught the lost generic stack guard: admission now checks
saved context/ETask, SP range/equality/alignment, nonzero even PC and a non-null
aligned FPU image when flagged before acquiring Running ownership. Malformed
context is currently fail-closed, not the generic WAIT quarantine behavior.
These checks do not validate arbitrary pointer memory domains or code bounds.
The SMP cpu_Switch path now selects p4_switch rather than generic core_Switch.
It saves context/FPU/ID nesting under the canonical Task lock, commits the
wait token and RUN-to-Ready/Wait list move, and retains the unique RUN reference
through an unlocked READY-only switch callback. Ready remains coherently listed
but new admission refuses until the old owner drains. It clears current_task
and fpu_owner under the Task lock, unlocks while still lifetime-protected, then
releases RUN ownership by raw identity with no later Task access. It never
unconditionally re-enqueues after the callback. Strict isolated RV32 compilation
passes; the earlier actual-source live-handoff fixture passed 1,277 strict
ASan/UBSan assertions (author and root), with independent local review.
The newer Switch entry routes an already removed current Task through no-save
discard before any context/ETask/FPU-image read. Task unlock retains RUN
ownership until discard revalidates list detachment and actual owner/service
proof; the normal switch returns immediately after discard. This closes the
retire-then-Switch state incompatibility in source. Combined Switch/discard
fixture passes 2,027 strict sanitizer checks (author and root); independent
local follow-up review passes. Queue/registry/poke operations are fixture
mocks; this is not cross-hart or hardware qualification.
The outer trap now invokes p4_retired_trap before ordinary scheduling or
syscall handling, irrespective of per-hart Forbid. It checks removal state
under the Task lock, retains RUN through unlock and actual discard validation,
then always invokes cpu_Dispatch to replace the return frame. Live or empty
current identity falls through. Only outer depth1 may invoke it. The helper
and whole trap translation unit compile with strict matched RV32 flags;
actual-source helper/discard plus extracted outer branch pass679 strict
sanitizer checks (author/root); independent local review passes. Dispatch,
runtime/locks, list/registry proofs and IPI remain mocks. A late retirement
after a live-state check requires a guaranteed latched/retriggered owner IPI;
RUN protects lifetime but does not guarantee instantaneous no-resume. Actual
remote removal/IPI delivery and public Wait/Exception races remain unwired.
Focused Task-writer audit confirms generic SetTaskPri chooses a stale queue
lock before Task-state serialization and retains unpinned Task/ETask reads;
generic core_Schedule compares priorities without Task/Ready locks. Lookup
functions have cross-list migration gaps and scan TaskSpinning outside the
current three-list contract; legacy raw returns promise no post-return lease.
A gated P4 scheduling-decision helper is under implementation with Task then
Ready locks and bounded list validation; it is not yet source-selected or
qualified for runtime. Its actual-source host fixture passes446 sanitizer
checks (author/root); isolated strict RV32 compilation passes. Root's local
source review confirms current Task then Ready locking, full bounded shape
validation before head-priority access and no queue mutation. A matching
SetTaskPri candidate now pins before Task access, serializes state/priority
with Task then all three queue locks and validates exact membership/live RUN
owner before mutation. READY is reordered under Ready; WAIT only changes its
priority; RUN pokes its copied owner. READY copies all online eligible hart
bits, not only the first eligible hart. Outputs survive final ordinary Unpin;
no Task/ETask read follows it. Retirement after a successful pin returns the
locked old priority without mutation or poke. Both writer/queue objects
compile strictly; independent local review passes after correcting the READY
target mask and issuing all copied pokes before outer Enable, after locks and
final Unpin. This requires an IRQ-safe asynchronous backend, still absent.
The outer Disable now precedes initial Pin, preventing its nested Enable from
discarding the caller while it holds a foreign Task reference. Signal and
SetExcept candidates also retain the outer mask through final unpin/poke.
Updated actual-source priority fixture passes2,359 sanitizer checks (author/root)
and refreshed source-local wake-order review passes. Signal/SetExcept fixture
passes67 actual-body sanitizer checks (author/root); adapter/pin/queue/poke are
mocks and public wrappers remain unselected;
Signal's lifetime-readiness compile guard still refuses activation. Durable
service-ready binding/claim-next passes675 sanitizer checks (author/root) and
strict RV32 compilation. Successful ordinary Unpin/LeaveRunning reserve drained
work before releasing the leaf gate; claim-next consumes the queued service
reference and detaches atomically. No Task is dereferenced by these leaves.
Independent local review and regression repeats pass; this does not prove FIFO
fairness, wake liveness or caller-owned Task storage safety. No housekeeper wake or production cleanup is
installed yet.
The SMP-selected IRQ-safe p4_ipi backend now implements krnP4SignalPoke with private
SRAM pending flags/tick counts, no Task identities and no remote runtime writes.
The bounded leaf gate serializes producer publication/latch with owner-side
peripheral/CLIC acknowledgement and pending snapshot/reset. Pre-arm schedule
work is relatched on owner arm; unarmed timer ticks are discarded, armed tick
count overflow is fatal. MMIO/shared state ordering uses fence iorw,iorw.
Owner arm removes retained foreign line22/opposite-FROM routes in its own matrix;
CLIC uses its documented current-hart alias, never a gated remote aperture.
Arm/Drain use disabled gate helpers without Exec Enable, preserving caller raw
MIE/IDNest; independent review found and root fixed the original startup unmask
gap. Backend local re-review passes. Startup prepares after atomics/pins and
arms hart0 after runtime finalization; line22 drain and owner-only ApplyBatch
are bound in the trap, without an ungated second acknowledgement. Hart0 alone
owns global VBlank/tick advancement, forwards counted ticks to armed hart1 and
updates global IRQ diagnostics; wrong-hart timer/DSI lines refuse. Post-SysBase
tick readers/writer use the soft-atomic service; early primary-only access stays
plain. ApplyBatch saturates the owner hart1's quantum and changes no Task fields.
Strict isolated RV32 SMP/normal objects compile, normal objects contain no IPI
references; revised actual-source mailbox fixture passes602 sanitizer checks
(author/root), with mocked gate/MMIO/fence/route and deterministic post-unlock
producer, not actual concurrency. Route2,720 and owner-runtime81 actual-source
sanitizer checks pass (author/root). Independent primary startup/trap integration
review finds no lost-doorbell or duplicate-ack defect in this slice, but leaves
the shared krnTimerCount snapshot unsynchronized and secondary Arm/online
publication absent at that reviewed snapshot. A subsequent source-selected
SMP-only p4_timer_snapshot gate now serializes UNIT0 latch/read with raw MIE
preservation, bounded polling/acquire and fail-closed timeout. It is prepared
after the software atomic service before SysBase publication with hart1 held.
Strict isolated RV32 objects compile; its actual-source fixture and independent
review are pending. Review subsequently found the selected SD time helper
independently latches UNIT0 without this gate; whole-system snapshot coverage
was incomplete. The subsequent selected SMP SD binding now resolves a
versioned immutable P4-specific KATTR_PlatformTimer callback table before
RegisterBus; it calls the same kernel snapshot gate and refuses mismatched
service instead of falling back to direct MMIO. Strict isolated RV32 kernel/
SD objects compile. Frozen snapshot60,351 and SD callback133 sanitizer checks
pass with root repeats; source re-review confirms initialization before SD
publication. The prior80,353 count is superseded by the corrected lock mock.
Official public-header copy and full matched module/core build remain pending. An
unselected portable softint queue transaction candidate now compiles: enqueue
and duplicate suppression, owner0 active claim, highest-priority dequeue and
atomic empty/dispatcher-release under a caller-held shared gate. Its gate,
Cause/Enable/trap/idle bindings are now SMP-source-selected and compile as
isolated RV32 objects: private raw-MIE-masked shared gate prepared before
SysBase, copied primary IPI wake before producer Enable, and owner0-only
dispatcher active claim. Callbacks run outside the gate with interrupts
enabled; dequeue and empty/active release are gate transactions. P4 pending
hooks replace plain SysFlags reads in Enable, core interrupt exit and idle;
hart1 never drains. Actual-source queue170 sanitizer checks pass with root repeat.
Independent review finds a return-path gap: a wake drained by a nested IPI after
empty-release can leave queued work pending until a later unrelated interrupt.
The hook path now remasks MIE after each dispatch and repolls before ordinary
return/scheduling; strict isolated RV32 compilation passes. Deterministic exit
fixture now passes106 strict sanitizer checks with root repeat; corrected
independent source review accepts the bounded
interleaving fix. The first94-check exit fixture modeled queue delivery inside
a nested IPI incorrectly; root rejected that race claim and the replacement
requires outer second-Cause delivery. Gate include/CSR test boundaries compile
to the same RV32 object; actual gate fixture89 checks pass with root repeat.
SMP idle remasks and repolls too; its whole CPU integration remains unqualified.
Drain-until-quiescent can starve normal
scheduling under continuous producer load; no bounded fairness claim. Descriptor lifetime stays
the caller's obligation until completion.
Neither host fixture proves hardware delivery or concurrent
Exec. The old hart0 raw-counter FPU probe refuses SMP compilation;
a separate real concurrent Task/FPU harness is required. Public CPU
count publication must define the armed-before-online interval. Secondary arm/
launch is still absent; global softint gate/bindings now compile but await
qualification and full integration. Subsequent SMP
source-list selection now pairs the locked decision and SetTaskPri with a
P4 core_Schedule override; isolated objects compile and the actual-source
priority/decision fixture again passes2,359 sanitizer checks. P4 CPU paths
use their own switch/dispatch transactions; legacy generic entry points in
the override fail closed if reached. Actual mmake precedence/call-site audit
is pending, not full-link proof. Full ABI/link/configure acceptance remains
withheld. The local SDK
register proof and earlier E2 private IPI delivery are not production SMP proof.
SMP cpu_Dispatch now checks a still-current removed task through the separate
no-save p4_discard path. It validates Task removal state, bounded absence from
Ready/Wait/Running and a retired designated service reference with the executing
hart's sole Running owner. It clears only its own wait token, current_task and
FPU owner, unlocks the Task while protected, then releases Running by raw identity.
No ETask/context or switch callback is used. Isolated RV32 compilation passes;
the actual-source discard fixture passes 64 strict ASan/UBSan assertions
(author and root), with queue/registry operations mocked. Caller integration
is pending. The service-owner leaf proof
passes 38 sanitizer assertions; actual bounded list-absence checks pass 62.
The real gate binding now interposes the service-owner query and checks local
IRQ nesting restoration for both harts (80,985 total binding assertions,
including queued-only claim and live RUN-owner query).
This does not itself retire or queue a task.
This is not a usable full scheduler yet: removal caller integration is absent,
removal is unsafe, generic Wait/Signal still bypass the protocol, and
the second hart remains held. Configure refusal remains mandatory. Repeated
full-list validation has potentially quadratic scan cost; the count bound is
not a masked-IRQ latency qualification or a sustained-priority guarantee.

Creation integration is being wired through P4-only Exec platform hooks.
Independent registry storage is AllocMem-owned, not embedded in Task/ETask or
blindly attached to tc_MemEntry. Atomic registration plus a creator pin protects
all accesses through the end of NewAddTask; eventual detached-record freeing
belongs to the reclaimer. NewTaskReady holds Task plus Ready/Wait/Running locks,
then copies the destination for an unlocked asynchronous poke before its outer
Enable. This prevents caller preemption/retirement from suppressing a deferred
wake. The backend must latch without synchronous scheduling with IRQs masked.
The updated actual-source publication helper fixture passes332 sanitizer
checks and strict RV32 compilation; independent wake-order review pending.
Boot registration
reserves hart0 Running ownership and publishes runtime current_task before
unmasking; bootstrap ETask creation still follows this publication. These
helpers compile RV32. Shared NewAddTask/bootstrap hooks pass extracted-source
sanitizer fixtures in four configurations (42/42/42/58 assertions, author and
root); publication queues pass 766 assertions. The actual-source publication
helper fixture now passes 332 assertions with real registry/pin/queue sources
(author and root). Reclaim integration remains in progress.
Bootstrap registration explicitly requires hart0, one online CPU, empty
current_task and nonnegative boot nesting counts before its early publication.
A returned raw Task pointer is not permanent lifetime ownership;
subsequent cross-hart operations must pin before dereferencing it.

The removal/service design must transfer a designated lifetime reference before
publishing a Task as a ServicePort message. The service must retain it through
requeue and consume it atomically with registry detach only after all other
pins and Running owners drain. A separate unpin then ClaimReclaim is forbidden:
it creates an unowned interval. A private service_pending marker and atomic
retire-for-service/claim-service primitives now pass 370 sanitizer assertions
(author, root and independent reviewer) and have gated kernel bindings whose
actual-source fixture now passes 80,985 assertions, but are not yet wired
into RemTask/ServiceTask. Context deletion and CleanupETask must move after
owner drain; parent notification owns the separate ETask allocation and must
not be mistaken for the ServicePort's Task message. Claim occurs before
MemEntry freeing can destroy the Task; the detached record is freed outside
the gate, without subsequent Task access. External removal also needs this
deferred path instead of synchronous destruction of a remotely executing Task.
External RemTask must nevertheless preserve its return-time completion contract:
the caller may immediately free Task storage it allocated outside tc_MemEntry.
A registry pin cannot prevent such caller-owned FreeMem. Completion must be
communicated through separately owned storage after cleanup and RUN/pin drain,
not merely by returning after retirement. The new queue-only TaskRetireLocked
candidate validates exact Ready/Wait/Running membership under all three locks,
atomically acquires the service reference as its last fallible step, detaches
the scheduler node and marks TS_REMOVED. Live registry owner validation now
explicitly rejects a TS_RUN task without a RUN reference. Actual queue plus
registry/lifetime fixture passes 647 assertions (author and root), with
independent source review. It neither releases RUN nor performs
cleanup, queued-request cancellation, notification or completion; public caller
integration is pending. The private service_queued reservation
passes 531 actual-source sanitizer assertions (author and root): a drained
cleanup identity is taken once under the lifetime gate before any unlocked
ServicePort operation. A distinct queued-only claim and gated kernel binding
refuse even a fully drained record until reservation; its leaf fixture passes
48 assertions including the live-owner proof. Trusted service-owner claim remains separate and must not be
used by a GetMsg consumer. There is still no notification binding, actual
message delivery, completion acknowledgement or cleanup caller. The private
record grew by one word; creation's sizeof allocation consumer was freshly
compiled together with registry/pins. This does not change public Task ABI.

Next integration contract (design only, not implemented): use the registered
queued record as durable work, with a coalescing asynchronous mailbox wake,
rather than relying on a notifier resuming after Enable to publish a Task
message. Final unpin/RUN-leave must reserve and publish the wake before exposing
a local scheduling window, without Task/list/semaphore locks, allocation or
cleanup in the leaf gate. The permanently registered, non-removable housekeeper
drains queued work through queued-only claim before any Task access; the wake is
a hint, not ownership of the request. Clear/rescan ordering must prevent lost
wakes, and reserved work cannot depend on finite notification capacity.

The selected dispatch callback guard now snapshots live state/flags/launch
under the current Task lock while retaining RUN, calls callbacks outside locks,
then remasks MIE and checks retirement. CPU discards retired recipients and
restarts dispatch before final context/FPU access. Helper/CPU isolated RV32
compilation and callback225 sanitizer root repeat pass. This does not solve
public Exception's internal callback loop or self-RemTask nested-trap unwinding.
Subsequent review found generic Exception still selected. Paired P4 Exception
now replaces it with locked consume/merge, retirement exits and explicit raw
callback MIE handling. Strict isolated RV32 compile and actual-body327 sanitizer
root repeat pass. The latter includes the real Exception and protocol bodies,
but models RUN, adapter and retirement; it does not test self-RemTask unwind.
Callback helper225 sanitizer checks pass with root
repeat; this mocks Exception/RUN ownership, not the whole CPU return path.
Independent paired callback review accepts the local sequence only under RUN
ownership; generic RemTask still violates that lifetime assumption and generic
signal writers remain unpaired. These are activation blockers, not a safe
whole-system race claim. P4 SetSignal/SetExcept are now source-selected and
strict isolated-RV32 compiled. SetSignal masks through locked effect snapshot
and retains only its scalar after Enable; actual-body100 sanitizer root repeat
passes; independent review accepts local order under current RUN ownership,
not real adapter/lifetime integration. SetExcept67 actual-body wake checks pass
root repeat. P4 AllocSignal/FreeSignal now selected: mask updates share the
disabled canonical Task lock; only scalar/no Task access follows Enable.
NewCreateTaskA initializes its creator-owned unpublished Task lock before port
signal allocation. SMP and normal constructor isolated RV32 compilation passes;
allocation/free612 sanitizer root repeat and independent constructor review
pass under exclusive unpublished ownership. Actual constructor fixture exposed
an unnamed/no-port third-MemEntry overread; conditional count-checked lookup
now compiles SMP/normal and actual constructor SMP260/normal232 sanitizer root
repeats pass, including exact two-entry unnamed/no-port allocation and poisoned
successful NewAddTask return. Allocator/spinlock/publication are host stubs. This
does not qualify concurrent output-slot observation or whole construction.
Signal/Wait and public
retirement/self-RemTask unwind remain open; configure refusal stays intact.

ETask topology inventory confirms Task pinning does not retain independently
expunged ETask metadata. Generic Child* and parent/death-message accesses need
a coherent topology boundary and metadata ownership. SMP cleanup now detaches
exited messages before expunge, children before reparent and orphan nodes before
sentinel destruction; strict isolated RV32 compile and extracted actual-cleanup61
sanitizer root repeat pass. List/lock operations and ownership are host mocks.
These structural repairs do not fix parent lifetime, lock ordering, callbacks
and do not qualify the cleanup worker for concurrency. ChildWait separately
clears SIGF_CHILD before querying the durable message list, never after an empty
scan; SMP/normal isolated RV32 compile and actual-body193 sanitizer root repeat
pass. Independent local ordering review accepts the tested interleavings;
generic Wait, list/lock/signal behavior are mocked, not concurrent runtime.
This order repair is not a metadata lifetime/topology fix.

The source-selected p4_etask_lifetime leaf separates Task-owner metadata ownership,
dead-result queue ownership, child edges retaining parent metadata, and
temporary metadata readers. It requires already zeroed storage and one
caller-held topology gate. Closing prevents new active edges/readers; cleanup
requires all child edges detached, while existing readers can delay reclaim.
Result ownership can outlive Task storage. Reclaim transfers exactly once,
with freeing only after gate unlock. Strict isolated RV32 compilation and an
independent local transition review pass;282 strict sanitizer checks pass a
fresh root compile/run. The fixture mocks external Task RUN/pin ownership.
Private PID owner records now use this leaf: creation initializes a Task
metadata owner, actual CleanupETask closes it, retained dead-result ownership
transfers before PutMsg, and final expunge/affinity rollback drops ownership
before PID detach/free. Strict isolated builds and root owner-hook195/InitExpunge91
sanitizer repeats pass. Independent owner review accepts local ordering but
identified a prefix-lookup corruption path. Full-shape/exact-one lookup now
precedes every owner transition; root repeats1605 binding sanitizer checks,
including21 malformed-registry refusals without owner-state mutation. The
correction review accepts the ordering at source level. Direct-leaf194 root
sanitizer checks now cover null/absent/successful opaque lookup without
mutation; worst-case masked traversal duration is unmeasured. Binding counts predate
the next deferred-release API change. Release now distinguishes a unique free
claim from logical expunge retained by readers, and generic Expunge frees no
ETask or Result2 on deferral. Last ReadUnpin detaches PID/sidecar under the gate,
then frees sidecar and actual ETask storage outside it without recursively
dropping ownership. Strict isolated SMP/normal objects pass; root repeats2622
binding and196 actual Cleanup owner-hook sanitizer checks. Storage-free is
mocked there. Actual Init/Expunge126 and reader/reclaim227 sanitizer checks
also pass root repeats; the latter uses actual binding/leaves and extracted
actual Expunge/storage-free bodies, with gate/allocator/association mocks.
Independent source review accepts local reclaim ordering and the clarified
strong-owner admission precondition. Reader helpers are
selected but have no protected production discovery callers; parent edges
and actual RUN/pin-drained removal remain absent.
It cannot prove Task RUN/pin drain, authorize raw Task dereferences,
or correct publication before fallible NewAddTask setup. Those integration
preconditions remain hard activation gates.

SMP ChildFree now ignores active children and searches only durable exited
results under the message-port write lock. Unlink transfers queue ownership;
ExpungeETask follows port unlock and Permit, with no later result access.
Root repeats135 actual-body sanitizer checks and fresh isolated SMP/normal
RV32 objects pass. Current Task RUN/metadata ownership, expunge internals and
the shared topology protocol remain unproven by this local list correction.

The source-selected deferred operation has local actual-callsite host
qualification, not a concurrent Exec or hardware proof. Protected discovery
and reader callers still require real topology ownership binding. Reader
admission through the internally locking wrapper requires an existing strong
metadata owner, not a pointer discovered before dropping the gate. Future
topology discovery must acquire its lease in that same gate transaction.
Admission never accepts an arbitrary stale ETask
pointer; a metadata lease does not authorize Task access. No reader API may
be enabled before this free/lease contract has actual-callsite tests.

A raw-MIE SRAM metadata gate is now source-selected and prepared once from
Exec_PreparePlatform after bootstrap binding, before Exec vectors exist.
Strict isolated RV32 compilation passes after adding the missing P4_SRAMDATA
defining include. Independent review is pending. Allocation/free, callbacks,
Task pins and Task/queue/registry
locks must remain outside this nonrecursive gate. The bounded identity leaf
and wrapper are now selected: actual InitETask reserves before publication,
affinity rollback releases, and final ExpungeETask detaches the reservation.
Allocation/free are outside the gate; shared counter plus ETask PID publish
together inside it. Strict isolated SMP/normal builds pass, normal ExecUtil
remains byte-identical. Corrected actual PID leaf passes181 strict sanitizer
checks in fresh root repeats, with external gate/counter modeled; its zero-state
init precondition is now explicit. Actual binding262 sanitizer checks also pass
root repeat, with gate/allocator/ExecBase mocked. Actual InitETask/ExpungeETask
hook bodies91 and gate45 strict sanitizer checks also pass fresh root repeats.
These model CSR/runtime/Exec boundaries. Full final-expunge
metadata ownership remain open; this does not make Child* concurrent-safe.

The actual SMP CleanupETask now transfers all queued dead results to its
worker-owned local list while holding the message-port lock, then expunges
only after unlocking. PID gate acquisition and allocator callbacks therefore
no longer run under that port lock. Strict isolated SMP/normal builds pass;
normal bytes are unchanged. Corrected actual-source76 strict sanitizer checks
pass root repeat; first fixture compile lacked the new list mocks and failed.
Earlier cleanup61 evidence predates this change. Parent metadata/topology ownership
and concurrent ChildFree safety are still not implemented by this list transfer.

P4 SMP NewAddTask now stages ETask construction with a null parent until
PrepareContext and creator-pinned Task registry creation both succeed. Only
then does Exec_PublishConstructedETask attach the current caller as parent,
before prelaunch observers or Ready publication. Failed setup cannot publish
a parent edge or death result. The helper requires fresh unlinked metadata,
uses the existing parent iet lock with local interrupts disabled, and accesses
no Task after Enable. Strict isolated SMP/normal builds pass; actual-source
fixture is pending; independent review accepts local order under the stated
ownership assumptions. Both failed-construction paths now clear the freed
ETask pointer and TF_ETASK before returning caller-owned Task storage; fresh
SMP/normal isolated builds pass; independent follow-up review accepts the
rollback correction. Fresh root actual-source fixture passes212 staged and22
ordinary-path sanitizer checks; Init/Cleanup/context/registry/Ready are mocked.
This is a construction-order
repair, not a final topology transaction: external RemTask RUN protection,
metadata gate/lifetime binding and PID reservation still block activation.

The unselected p4_task_cleanup consumer claims a drained registry service
record before any Task access. It deletes context/metadata and moves every
MemList node to a worker-owned sentinel before freeing any entry; Task storage
and its embedded sentinel may disappear with the first entry. No Task access
follows the first FreeEntry. Strict isolated RV32 compilation passes;
independent local review and actual-source115 sanitizer checks with root repeat
pass. Registry and Exec cleanup APIs are modeled. This is not production worker
binding: metadata topology, wake delivery and completion acknowledgement are
absent. The trusted worker continuation must remain non-retirable throughout
detached-record ownership, or provide an explicit ownership-transfer protocol.

External removal needs a separately allocated two-reference completion ticket
(requester plus cleanup worker), allocated before retirement can succeed.
Cleanup completes context/ETask/owned-memory work before publishing acknowledgement
and never dereferences the target afterward. The requester waits with locks
released using a predicate/signal handshake, then returns; caller-owned Task
storage may be freed only after that return. A removed requester cannot be
responsible for releasing a reference from a continuation that never resumes:
its waiting-ticket ownership must be represented in reclaimable private metadata
and cancelled exactly once. Notification pins protect reply identity only until
the reply/drop; they must not keep a dead waiter or target alive indefinitely.
An unselected portable completion ticket now implements the two-reference state
protocol and passes255 strict sanitizer checks plus isolated RV32 compilation.
The prior237-check review found overlapping outputs could overwrite ticket
state; these now refuse before mutation with no end-address overflow. Refreshed
independent review accepts the correction and repeats255 checks.
This does not attach tickets, perform cancellation, notify pinned recipients,
wait in public RemTask or run the cleanup worker. Those integration mechanisms
remain missing, not qualified by these leaf tests. The source/design review and exact fixture recommendations
are in build evidence smp-deferred-service-design-2026-10-01.md.

Forced outer-trap dispatch is also required: generic core_Schedule returns false
for TS_REMOVED and generic core_ExitInterrupt skips scheduling under Forbid.
An actual remote-retirement interrupt must take the validated no-save path and
install another task instead of returning the old frame, even when normal
priority/preemption checks would decline. Combined Switch/discard fixtures alone
cannot qualify this trap-level reachability. Public Wait/Exception and queued
semaphore cancellation must reconcile retirement at their callback/arming
boundaries rather than returning a removed Task to user code or freeing a
stack-linked request. Second-hart release remains prohibited until these paths,
other public writers, timer/IPI and full matched ABI are integrated and tested.

2026-10-01: P4 SMP dispatch now has one launch-hook owner: the platform
mapping suppresses generic core_Dispatch's callback and leaves P4's call
after context/FPU restoration and exceptions. Extracted callback-site tests
pass in both selections, including repeated dispatch. Normal P4 behavior
is deliberately unchanged and its pre-existing duplicate remains separate.
The new actual Task-pin binding uses a private prepared public spinlock,
balanced Disable/Enable and bounded fail-closed acquisition around the
caller-serialized registry. It defines pin/unpin, record publication,
retirement and running-owner/reclamation APIs; source selection is now SMP-only,
but creation, scheduler and removal/service remain unwired. Standalone RV32
object and host binding tests pass; independent review is in progress.

2026-10-01: corrected signal core now explicitly receives an armed-wait
snapshot. Ordinary signals only poke a RUNNING owner for a matching live
wait token; pending exceptions remain independent. The adapter reads the
target owner's token under its Task lock. Root repeats 181 ASan/UBSan checks,
including stale masks, repeated arm and repeated committed switch refusal.
P4 Wait/Exception source candidates now fail closed on invariant failures;
queue binding validates runtime before list mutation. Wrapper compilation
and independent post-correction review remain in progress; none is selected.

2026-10-01: shared task creation now explicitly initializes tc_SpinLock when
both SMP ABI defines are selected. This includes the bootstrap task, which
bypasses NewAddTask. An extracted sanitizer fixture verifies initialization
before ETask/current-task publication in all four define combinations;
TaskLaunch still passes 90 checks per variant. Standalone SMP NewAddTask
compilation remains rejected because the real P4 krnSysCallReschedTask binding
is missing (plus existing strict-warning issues); no successful SMP build is
claimed from these prefix tests.

2026-10-01: the candidate signal queue adapter now uses actual Exec lists,
ordered Ready/Wait/Running locks and validated whole-list membership before
mutation. It rejects wrong task state, invalid affinity and broken list
sentinels/backlinks. RV32 compilation and 53 actual-helper ASan/UBSan queue
checks pass, including cycle and scan-limit refusal. It remains unselected:
scheduler context handoff, other list
writers and task retirement must adopt the same protocol first. The lifetime
model passes 56 independently repeated sanitizer checks and RV32 compilation;
it is not a Task registry or a reclamation implementation. Independent review
also exposed needless RUNNING reschedules from stale tc_SigWait; correction
must distinguish an actual armed per-hart wait token from the stored mask.

2026-10-01: portable signal/wait protocol passes 146 sanitized host checks
after review corrected a SetSignal corner case. Only matching wait bits or
the raised exception flag prevent sleeping; SetSignal's exception-matching
bits alone must not cause a READY/Wait busy loop. Exception helpers leave
removed/invalid views unchanged. The protocol returns queue effects; callers
must still commit them under task/queue locks. It is not linked into Exec.
The public atomic cold/hot lookup fixture passes 194 sanitized checks across
12 isolated scenarios, including mocked concurrent lookup and reentry.
Actual OpenResource interrupt-lock safety remains unqualified.

2026-10-01: per-hart runtime now reserves a wait_pending task token while
retaining its 64-byte slot size/alignment on host and RV32. Bootstrap rejects
nonempty tokens instead of erasing them; Finalize preserves an armed token.
155 production-runtime ASan/UBSan checks and standalone RV32 compilation
pass. The host fixture uses checked-in declaration stubs for unused public
spin/hook types; it does not validate those ABIs or scheduler transitions.
Independent atomic bridge review reproduces 77 checks and all three RV32
objects without AMO/LRSC. Cold resource lookup from interrupts/reentry and
matched whole-system selection remain unqualified; configure must set both
the Exec SMP and platform ABI defines when eventually activated.

2026-10-01: the task-state protocol uses the documented Task.tc_SpinLock,
not IntETask.iet_SpinLock (a nullable pointer tracking a lock being spun on)
or the separate ETask child/message lock. P4's matched SMP ABI must provide
the platform task-lock field and initialize it before task publication.
Signal/Wait audit confirms that queue transitions, removal/lifetime,
SetTaskPri and AllocTaskSignal must join the same protocol. A pure protocol
candidate is being tested; no scheduler or second Exec hart is activated.
The shared SMP TaskLaunch now runs its pre-launch hook exactly once before
Ready-list publication. Extracted production-code tests simulate immediate
remote observation and pass 90 sanitized checks in each build variant;
task-creation failure tests still pass 117 checks. This is not a matched
whole-system build or hardware result.

2026-10-01: public-spin adapter independent review passes under its stated
contract; actual concurrent hardware use is unqualified. Semaphore audit
requires cross-hart protection beyond Forbid and notification outside the
semaphore lock. Stack requests must not be accessed after notification, and
foreign SIGF_SINGLE must not be mistaken for ownership. Candidate integration
and deterministic tests are in progress.
Avoiding post-notification stack-request access is insufficient alone:
the captured Task* also needs lifetime protection until notification ends,
because a foreign signal can let a granted waiter return and exit first.
Task removal/service reclamation must join the same protocol before activation.
The unsafe partial Semaphore Obtain/Attempt changes were reverted; current
work is the separately tested lifetime protocol before restarting integration.
Fresh invalidated P4/shared/generic normal Exec compilation/linking passes
without warnings; the new 225,048-byte Exec object has not been packaged or
deployed. This does not qualify SMP or alter the on-board baseline.

2026-10-01: public atomics need an exported service, not undefined core symbols
in package-loaded ELF modules. The optional KATTR_AtomicOps versioned immutable
table follows the existing platform-operation-table pattern without changing
LVO numbering. Its P4 SMP-only checked backend passes 77 sanitized host checks
and standalone RV32 compilation (2,268 bytes; SHA-256
dc50ebf724c3b0769365bf13caef8d26db2726a7994d19ae0f5344630b823ee6).
The P4-only platform_atomic header now selects this checked service for SMP
public INC/DEC/AND/OR macros; core code calls directly before resource-vector
initialization. Loaded modules use existing resource/query LVOs, avoiding ELF
undefined core symbols. All 12 width/operation macro combinations compile in
both modes, without AMO/LRSC, and include-copy updated both header trees.
Plain task-field conversion, whole-build invalidation, independent review and
matched-system qualification remain incomplete. Configure refusal remains.

2026-10-01: full SMP completion explicitly requested. Real runtime integration
uses a staged bootstrap: prepare CPU-local slots with hart1 reset-held, bind the
new SysBase before PrepareExecBase uses SMP platform macros, and attach the
KernelBase later without resetting task/nesting/scheduler state. Private diagnostic
qualification remains distinct from actual concurrent Exec task acceptance.

2026-10-01: the parent/child callsite map confirms the next integration must
cover Exec publication/cleanup and every Child API as one topology protocol,
not just add reader calls after releasing discovery locks. ChildOrphan's SMP
active-only correction preserves retained dead results; ChildStatus's active
READ-lock correction passes root actual-source352 SMP/205 normal sanitizer
checks; ChildOrphan passes the corrected321-check root repeat. Both compile
as strict isolated SMP/normal RV32 objects and pass independent local source
review. Dead-only IDs now return CHILD_NOTFOUND from the SMP ChildOrphan LVO
and remain available for ChildWait/ChildFree. These local list corrections do not
provide an atomic active/dead snapshot, parent Task pin, or real RUN drain.
Cross-module getppid, task.resource parent-slot reads, DOS notifications and
waitpid's unlocked result-list count require a compatible public operation;
private core symbols cannot be assumed available in package-loaded modules.
Generic RemTask must stop deleting context/ETask before remote RUN/pins drain.
Integrate the topology and removal contracts before admitting hart1 to Exec;
keep configure refusal, the single-hart baseline and separate full ABI/build
and concurrent task/FPU/contention acceptance gates intact.

2026-10-01: actual P4 ServiceTask requests permanent protected registry
ownership at its first RUN entry; RemTask checks identity admission before
target fields/logging. Private worker_protected records cannot retire or drop
their permanent pin, but transient references and RUN handoff remain allowed.
Root leaf494 and actual pins-binding81,346 sanitizer repeats and strict
isolated SMP/normal caller builds pass; no actual caller runtime qualification.
The first-entry startup window requires a worker-ready acknowledgement before
secondary Exec admission; priority127 is not proof. Deferred wake and drained
consumer are now experimentally source-selected, not runtime-qualified.
Either hart's safe outer trap/dispatcher/idle now delivers protected worker snapshots
outside outer Task/registry locks; ServiceTask clears before durable drain/Wait.
The actual caller fixture and independent review are in progress. Completion,
metadata topology and startup admission remain open. Secondary IPI arm now
requires worker-ready ACK plus retained outer Disable
before taking the IPI gate (local mailbox763/route2,720 checks); actual secondary
entry and public CPU-count publication remain absent. Generic
external RemTask still lacks full RUN/pin drain. P4 self-RemTask now intercepts
before generic context/ETask cleanup: ordinary pin, locked retirement,
unlock/unpin, no-save dispatch. Review found callback depth2 dispatch cannot
handoff after retirement. Private scoped unwind now wraps dispatch and switch
hooks; self helper checks context first and handles remote-retire winners by
service/RUN proof. Strict isolated RV32 compilation passes; new fixtures and
independent correction review accepts local ordering under its RUN/service
preconditions, not the unsafe generic external removal path. Root strict ASan/UBSan host
repeats escape95/self175/dispatch258/switch2,049 pass with mocked boundaries
and native host jump substitution. Actual RV32 escape passes generic
QEMU virt O0/O2 with s1/s11/fs0/fs11 sentinels; runtime/ownership are mocked and
hart slots exercised sequentially, not actual P4 trap/concurrency proof.
Independent emulator rerun/review accepts only this bounded ABI/unwind result.
Independent host review exposes cross-fixture inference for actual switch/self
escape composition; a new actual-source composed fixture is requested.
Do not admit hart1 from this local proof.
Independent worker review found generic Signal outside the selected P4 predicate
lock. P4 Signal is now SMP-source-selected with registered-task pin binding;
actual-body67 sanitizer checks and strict isolated RV32 compile pass. Independent
selection/order review accepts local scope; full lifecycle/concurrency remains
unqualified.
P4's locked wait-token implementation is now SMP-source-selected so the worker
does not use the generic unlocked Wait path. Corrected actual-body fixture407
passes root ASan/UBSan with independent IDNest/TDNest models; the old217 mock
nesting proof is withdrawn. Context-switch token commit and all lifecycle gates still need full
configured/concurrent qualification.
External completion callsite audit is read. A private-record-indexed heap ticket
table with cancellation and poll/yield instead of retained waiter Task pins is
not selected: read feasibility/yield audits identify setup escrow, deferred
garbage outside outer locks, retire/claim closure, remote RUN-owner poke and
formerly primary-only cleanup wake as concrete blockers. Both-hart publisher
and safe-boundary delivery correction is now source-selected, with new
strict isolated RV32 publisher/CPU/trap builds passing. Updated actual mailbox
1,004 and route2,720 pass root strict ASan/UBSan repeats; boundaries pending.
Independent transport ordering review now accepts local source scope; new
executable callsite fixtures remain pending and no real secondary launch exists.
Caller yielding cannot force
another masked hart to progress. No external RemTask/ACK binding is selected.

2026-10-01: staged investigation/implementation begun at user's request;
no claim of accepted architecture or working second hart.

### External removal: next construction to audit (not selected)

2026-10-01: selected cleanup consumer is being changed to a retained service
claim/finish pair. During cleanup the bounded registry keeps an opaque identity
with exclusive claimed in-flight state, preventing duplicate publication or
claim until the last target/context/entry free. Finish validates membership
before reading the raw record, detaches only afterward, and record FreeMem is
outside the gate. Root registry478/gate binding82,107/composed consumer171 pass
strict sanitizer repeats; legacy80/370, queue711 and creator332 regressions pass.
Fresh isolated RV32 registry/pins/creator/consumer compile with matched defines.
Independent narrow lifecycle review accepted; no full matched image or concurrency.
This is not an external requester ACK: waiter references, cancellation and
callback-safe synchronous completion are still unbound. Configure refusal stays.

The read-only inline-progress audit also identifies stack-local semaphore
requests as separate reclamation obligations: ordinary pin/RUN drain alone
does not unlink a blocked Task from `ss_WaitQueue`. External removal must
cancel the request under the same arbitration used by all semaphore APIs
before reclaiming its stack. The creator pin spans the prelaunch hook;
synchronous pin-drain waiting inside that hook has a same-stack cycle, but
the audit does not establish that prelaunch removal is a supported API contract.
Raw-MIE spinning is therefore not selected as a general completion protocol.
Allocator backend reentrancy remains a conditional risk, not an observed P4
failure. Next: inventory the complete semaphore mutation surface before
selecting the gate/cancellation protocol. No new hardware run follows from
the display being reconnected; interactive tests still require readiness.

### Semaphore binding construction (2026-10-01; activation remains refused)

Root selects the already-initialized `ss_MultipleLink.sr_SpinLock` as the
per-semaphore arbitration word, without changing the public structure. The
private P4 binding passes root132 host checks and isolated RV32 compilation:
Disable before bounded write-acquire,
release before the matching Enable; no Forbid ownership in that leaf. API
callers retain responsibility for task-disable nesting and object lifetime.
The gate must not span Wait, Signal, ReplyMsg, Alert, allocation or application
callbacks, and must not nest another semaphore gate. The SMP platform now
maps the paired lock/unlock hooks for the nonblocking InternalAttemptSemaphore
transition. This is an isolated construction step, not deployable mixed API
routing: blocking Obtain and Release remain unbound and full SMP activation
is still refused. Full public API routing must be selected together.
The actual Attempt body passes root116 no-hook and149 hook host checks,
including12 simultaneous exclusive attempts with exactly one winner. The
fixture substitutes a pthread mutex and CheckSemaphore/Forbid/Permit mocks;
it does not qualify the actual P4 spin backend or unbound Release. Fresh
strict RV32 SMP/normal objects confirm the hook pair is imported only by SMP.

Blocking-operation storage construction uses one generation-stamped scope in
the independently allocated Task registry record, published before any waiter
queue insertion. SINGLE indexes its semaphore/request; LIST indexes the
caller-arbitrated list. These are opaque identities, not Task dereferences.
An active scope owns a designated pin; generation exhaustion is refused.
Readiness requires closed construction and zero queued requests, not a signal
bit. A grant must snapshot/dequeue request fields before publishing readiness
and acquire a separate ordinary notification pin first. After Signal it may
only drop that ordinary pin, not access the old scope or stack request.
Retirement cancellation must be claimed and woken before the scope pin drains;
final cancellation requires zero queued requests and no RUN owner. A claimed
scope is exclusive, retry leaves cancellation sticky, and a zero-queue scope
with a RUN owner must not cause a busy drain loop. The registry packet passes
root171 actual-source host sanitizer checks and isolated strict RV32 compile;
narrow independent leaf review is accepted after171/80/370/478 repeats. It is
not bound to public semaphore APIs and does
not represent asynchronous Procure messages or external RemTask completion.
Root now binds the scope transitions through the existing IRQ-masked registry
gate. Cleanup wake reservation also recognizes eligible wait cancellation,
independently of final service readiness: the designated wait pin must not
prevent its own cancellation from being scheduled. Final RUN release retries
that reservation; FinishCancel drops the scope pin and reserves any newly
eligible service wake. These bindings compile in an isolated strict RV32
object. Root82,554 actual-source binding checks pass with all eleven registry
helpers interposed to assert the IRQ-masked canonical gate; public-spin and
cleanup-poke services remain mocks. Independent binding review accepts only
these registry/gate transitions, not real semaphore queue integration.
That review identified two additional lost-wake orderings: generic registry
retirement did not schedule an eligible wait cancellation, and RUN could
drain while cancellation was claimed, leaving claim release without a retry
wake. Both paths now reserve durable cleanup wake under the registry gate
and poke only after unlocking. The corrected binding has a fresh strict
RV32 object; expanded opposite-order host tests pass root/independent82,772
checks and narrow independent review. The earlier82,554 result remains
historical and does not qualify the new paths by itself.
No cancellation consumer or public Obtain/Release routing is selected.

Before that routing is accepted, cancellation must distinguish queued nodes
from already-granted ownership, including a partially constructed semaphore
LIST. The existing queued count alone does not identify how far the caller
mutated the list or which acquisition must be rolled back. A Task pin prevents
storage reclamation but does not encode those ownership obligations. Resolve
that index/publication protocol before presenting the scope leaf as a complete
blocking-semaphore implementation.

Root selects a private boot-reserved operation arena for that accounting.
Caller-supplied operation/entry arrays are prepared before Exec and hart1
publication; runtime reserve/reclaim use only the registry leaf gate, not
AllocMem, FreeMem, callbacks or list-sized stack allocation. Array capacities
and byte size must be reported by bootstrap; exhaustion must fail closed
before semaphore mutation, never return apparent acquisition success. The
arena preparation is now bound before krnPrepareExecBase in the gated SMP
bootstrap, but has not run in a matched image and is not routed through the
public APIs. Default capacities are1024 operations and4096 entries, with
real request-node storage independent of Task stacks. The isolated RV32 ABI
reserves217,088 PSRAM payload bytes plus alignment padding and40 SRAM ledger
bytes. Reservation removes a prefix from the single unpublished free chunk,
leaves mh_Lower/mh_Upper intact, and preserves the remainder for TLSF.
Root857 sanitizer checks cover the reservation body. The composed actual
reservation/ledger/boot fixture now passes72 strict sanitizer checks, including
one-shot failure, withheld storage after initialization failure, zeroed real
request storage and a complete reserve/acquire/commit/reclaim cycle. Its host
Exec structures are shims, not a target boot. Frozen RV32 boot code compiles;
arena preparation and whole bootstrap still need independent review and
integrated runtime tests. Root review corrected an unreachable cancelled
withdraw transition and removed a post-mutation failure path in reclamation.
This changes no public Task, semaphore or message layout.

Private fused admission now connects the arena to the registry gate:
krnP4SemaphoreOperationReserve reserves the entire operation and publishes
generation plus designated lifetime pin atomically, before semaphore mutation.
Admission rejects a ledger object overlapping registry/record storage before
reading its typed metadata; root corrected the initial argument-check order.
OPERATION scope kind uses list_index as an opaque arena operation, not an Exec
List; generic Begin does not admit that kind. Legacy queue/readiness/finish and
retirement reject it. Dedicated fused SetEntry/Close and terminal helpers now
couple ledger completion/reclaim with scope clearing and pin discharge; both
selected retirement bindings cancel ledger and retire Task atomically.
Cancelled terminal finish requires exclusive claim, RUN0, queued0 and all
owned/queued obligations discharged. New cancellation snapshots are checked
outside the ledger object and its operation/entry arrays; the separate real
request pool is a caller-storage exclusion, not checked by this leaf. The
future cancellation consumer must use independent local snapshot storage.
Registry-owned operations must use fused Complete, not raw ledger Commit
followed by Complete. Malformed negative binding results are fatal.
Root actual composed completion208 checks at O1/O2 and fresh RV32 registry/
pins/ledger objects pass. Final admission221 passes author and fresh root
O1/O2 sanitizer repeats. Independent final lifecycle source review is accepted
only for this private contract and its stated caller preconditions; a separate
reviewer repeats root's retained completion208 at O1/O2. Legacy
pin-binding82772 and runtime atomic-policy36 regressions pass; neither
qualifies the new real queue path. The independent boot-arena
review (857 reserve/72 composed checks) is accepted for unpublished storage and
bootstrap ordering only; no matched image or TLSF execution was verified.
Public APIs and real node/count transitions remain unbound. Caller node-unlink,
rollback and notification-independence proofs are still preconditions, not
validated real queue behavior. These private checks do not satisfy acceptance.

E3-SM-N now supplies real reserved-node transactions for Configure, immediate
Acquire or Queue, cancelled Withdraw and owned-unit Rollback. The caller
holds semaphore then registry gates; the internal validated resolver exposes
borrowed operation/record pointers only during that interval. Absolute ledger
slot selects the pool node, including fragmented reuse. Live RUN/closed scope,
actual reciprocal queue/count shape, pool-domain separation, signed-WORD
overflow and pending-owned-unit floors precede the infallible commit. Real
queue insertion/unlink, NestCount/QueueCount and scope queued-count now move
with the ledger in these helpers. Isolated production-header RV32 objects
compile; actual-node fixture/review are pending. Grant, shared-batch/message
notifications, Release/Procure/Vacate, public routing and cleanup consumption
are not installed, so this does not establish public semaphore correctness.

Grant construction must preserve the full shared cohort from the original
queue, including shared waiters behind exclusive nodes. Reserve all selected
grants and stable notification storage before the first callback; a bare
grant/unlock/Signal/live-rescan loop can violate exclusion. Procure/Vacate need
separate exactly-once reply-publication metadata because queue absence after
grant is not evidence that a delayed reply has completed. PA_CALL can reenter
and cancel later cohort members. Neither a Task pin nor a stack worklist
retains reusable request-node storage. These are next binding obligations,
not properties established by Configure/Acquire/Withdraw/Rollback.

Each scoped entry records opaque semaphore/request identities and an explicit
UNVISITED, QUEUED, OWNED, RELEASED or WITHDRAWN phase. The complete operation
and reserved entries are indexed before the first semaphore mutation. Real
queue/count changes and ledger transitions must be atomic under semaphore
gate then registry gate. Aggregate nesting includes both completed anonymous
units and scoped OWNED units: cross-Task Release consumes completed units
first, otherwise marks exactly one scoped unit RELEASED. Cancellation can
therefore roll back only the remaining uncommitted OWNED units, not previous
recursive acquisitions or a unit another Task already released.

The frozen actual ledger fixture passes280 checks at O1/O2; root fresh strict
O2 ASan/UBSan repeat also passes280. Independent production-leaf review is
pending. These are caller-serialized host transitions with opaque identities,
not real queue mutation, notification lifetime, concurrent Exec or hardware.

Completion linearizes by committing the whole closed, nonqueued operation
under the registry gate. This converts its remaining units to completed
ownership without semaphore count changes; later retirement must remove
operation storage but not roll back committed units. Reclaim requires every
request detached and no notifier retaining an arena entry. A notification
batch must copy all destinations and reserve its references before publishing
readiness; notification pins alone do not retain entry storage. Async Procure
reply batches and external synchronous RemTask completion remain additional
obligations. Implementation, actual-node tests and independent review are
pending; this selected protocol is not concurrent hardware evidence.

Confirmed callout: ReplyMsg reaches InternalPutMsg and PA_CALL can invoke the
reply-port function synchronously. Dequeue/owner/count updates must therefore
complete under arbitration before that reply, with no request access after
ownership is transferred. A stack-node notification list alone is not safe:
a Task lifetime pin prevents stack reclamation, not reuse of a local request
after a foreign SIGF_SINGLE lets the waiter return. Blocking Obtain must check
its operation's completion predicate, not treat any wake signal as a grant.
Cancellation must cover queued AND granted-but-not-notified requests, retain
the target through final notification, and trigger before ordinary pin drain
makes cleanup claimable. Current generic semaphore code does not meet these
binding requirements.

Implementation order: private arbitration binding; complete mutation-surface
and callback-context audits; prepublished Task-independent wait-operation
ownership and explicit wake predicate; coordinated Obtain/Attempt/List and
Release/Procure/Vacate routing plus retirement cancellation; actual-source
concurrent/reentrancy tests and independent review; matched full SMP build and
headless D1001 qualification. A gate-only passing fixture does not close the
semaphore or E3 acceptance gate.

Actual selected P4 Wait now rejects supervisor trap depth after Disable and
before Task lock/wait-token mutation, including immediate-signal returns.
Root and independent actual-source437 sanitizer checks and fresh RV32
compilation pass; narrow valid-runtime guard review accepted. Generic
ObtainSemaphoreList still needs an
entry restriction before queue mutations. No callback is made sleep-capable
by this guard, and no public semaphore lifecycle acceptance follows from it.

Retired RUN-owner notification is now source-selected: RetireForService copies
the live owner under the registry gate before retirement and publishes an
asynchronous destination-only schedule poke after releasing that gate. It
does not dereference the retiring Task. Caller outer Disable/Task/list-lock
contracts still apply. Root endpoint1,249/binding82,107 host checks and isolated
RV32 compilation pass; the independent narrow owner-ordering review is accepted.
This cannot force a MIE-masked hart to drain and does not complete
external synchronous removal.

Root repeats now pass composed actual switch/self/unwind622 at O1/O2 under
ASan/UBSan with final RUN-release poison, and extracted dispatcher/trap C49.
These are host control-flow/snippet evidence with explicit mocks, not a full
SMP runtime. Fresh strict isolated RV32 owner-publisher/binding objects compile;
binding V1 passes81,952, extended V2 passes82,010 on unchanged root repeat and
independent timeout-bounded run. Earlier interrupted V2 run is unexplained and
retained in the evidence log, not a confirmed production/fixture defect.
Endpoint1,249 passes root strict ASan/UBSan repeat; owner ordering review and
external completion construction remain pending. Configure refusal stays intact.

The heap-ticket proposal has a pre-publication escape window. The next
candidate uses a completion slot allocated with each private registry record,
before Task publication, rather than AllocMem in the removing continuation.
One non-reentrant external removal per executing Task would use that slot;
whether supported callback contexts need more slots must be resolved before
selection, not silently refused as a final implementation.

Under the single registry gate, acquisition must resolve the current live RUN
requester and live target, acquire the target setup pin, initialize completion,
and publish both indexes atomically. No unindexed allocation or target pin may
exist on the caller's stack. Task/list locks are acquired only after this gate
is released. The setup pin must stay cancellation-visible until final target
Task unlock. Requester retirement marks cancellation but must not drop this
pin while that requester RUN frame might still be using the target; its final
RUN discard/release must discharge any abandoned setup resource under the same
registry protocol. A normal caller explicitly discharges it before yield.

The slot remains Task-independent even if its requester Task is cleaned first:
the detached requester record must outlive a target-side service reference.
Target ACK runs after every target/context/ETask/memory cleanup access and
before target-record reclamation. It clears target links under the gate and
queues newly ownerless records for deferred freeing outside every outer lock.
Polling validates the current requester record/index before reading slot state;
no ticket handle is dereferenced after unlocking. Completion consumes/reset
the live requester's slot; cancellation cannot report a normal successful
return into a retired continuation. All generic retire/claim paths must join
this protocol or reject outstanding references.

This is a design candidate, not executable code or an accepted contract.
Required next audit: record retention/reuse, cancellation/ACK cycles, setup
pin release on every RUN exit, callback reentrancy, bounded validation, deferred
garbage wake/drain, target-owner copied poke and caller nesting around yield.
Shared RemTask explicitly requires cleanup before external return because the
caller may free Task storage afterward. Async retirement is not compatible.
Normal calls inside Disable/Forbid must preserve caller nesting while yielding.
At callback depth1 a scheduling ecall would be nested and cannot run a new
dispatcher; a TF_SWITCH hook can also reenter removal while a normal requester
slot is occupied. These obligations invalidate accepting a one-slot/poll-loop
implementation without a separate callback/reentrancy solution. The next
feasibility audit must address them rather than refuse supported contexts.
Do not activate external RemTask or remove configure refusal from this sketch.
