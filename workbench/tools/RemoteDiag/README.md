# RemoteDiag: offline-prepared USB diagnostic slice

Status: implementation and host/native builds only. No installed service,
native USB enumeration, screenshot, input or file-transfer acceptance yet.
Fabian explicitly requested preparing these backends ahead of transport
qualification on2026-10-14; this does not waive the hardware gate or close
[RC-v5](https://github.com/metaneutrons/AROS-NX/issues/77).

The sources here contain no ESP32 registers. `protocol.c`, `service.c` and
`path.c` are portable; the AROS backends use ordinary Intuition/CyberGfx,
input.device and DOS interfaces. `bridge_aros.c` runs jobs in a separate DOS
process. Only the transport in `arch/riscv-esp32p4/usb` knows P4 hardware.
This is not a general daemon, public ABI, MCP implementation or final RC-v5
protocol. No mmake target, BSP inclusion, network listener or autostart is added.

## Implemented operations

- Full/cropped RGB888 public-screen copy; host saves P6 PPM, exclusively to a
  new file. The public screen must be explicitly named; no default fallback.
  `LockPubScreen` pins disposal, not redraw: the image can contain different
  redraw moments. Cursor inclusion is not promised. Pixel count is checked.
- Absolute screen-bound pointer movement and completed left/right/middle
  clicks. No keyboard, touch injection, held-button drag or persistent menu
  lease. Each owned event remains alive until `IND_WRITEEVENT` completes.
  Down/up errors return I/O failure and attempt release; repeated native
  release failure leaves input state uncertain, not a proved clean release.
- Root-relative directory listing, stat and exact-range/full download.
  Snapshot bytes are immutable once copied. Paths do not expose shell,
  device/handler syntax, absolute volumes, parent traversal, links or writes.
  A trusted configured root is resolved and its actual object lock pinned.
  Walks compare canonical metadata/parent identity and revalidate ancestry
  (64 levels). Native handler lookup decides ASCII case variants; ambiguous
  case-fold collisions fail closed. Listing/path searches scan at most128
  entries per directory; large directories can return LIMIT.
- Full32-bit native protection mask, comment and raw DateStamp on stat/list.
  Owner R/W/E/D are active-low; owner no-read is explicitly enforced. Group,
  other and unknown mask bits are not truncated or reinterpreted. Bit7 is
  AROS Hold/H, not universally Hidden; no UTC/date conversion is invented.
  Native names/comments are exposed as Latin-1 bytes, not Unicode-normalized.

File revalidation detects observable replacements, moved ancestors and
metadata changes before/after copying, not every same-size/same-date rewrite
or a fully atomic namespace/content snapshot. A pinned root continues to
refer to that configured object if the root itself is renamed. DOS does not
offer a universal atomic no-follow/openat transaction; do not advertise a
strong source-version or concurrent-writer exclusion guarantee. Chunk/resume
within one snapshot never rereads the source; a new session cannot resume an
old snapshot.

## Bounds, ownership and failure behavior

Request bodies <=1024 bytes, response data <=4096, one operation outstanding,
one cached snapshot <=4MiB. Replacing a snapshot can temporarily hold old and
new allocations (up to8MiB plus protocol/native overhead); release after use.
File offset+length <=INT32_MAX; larger files/ranges are explicitly unsupported.
CRC32 validates frames and complete copied payloads; it is not authentication
or a collision-resistant source version. Host download additionally reports
SHA-256, without dumping file or screen contents into diagnostic logs.

The USB owner alone touches the byte stream/TinyUSB. It submits copied jobs
to a worker through Exec messages; screen, DOS and blocking input calls never
run in endpoint/IRQ callbacks. The worker owns snapshots/handles. Reset and
new idle sessions invalidate outstanding reply tickets and queue snapshot
clear. An already executing native operation can still finish after USB
reset; a completed click is not undone. Its delayed reply is discarded. This
is not transactional cancellation or physical-input arbitration.

Shutdown keeps code and storage alive until actual worker process death via
ChildWait/ChildFree, not just an early reply. Native DOS/graphics/input calls
are not forcibly cancellable and a broken handler can delay shutdown
indefinitely. Use trusted local filesystem roots only. The USB lifetime and
controller polling budgets are not hard bounds on a stuck native backend.
Native SMP/message/scheduler behavior remains a hardware acceptance item.

Mouse access requires server `ALLOWMOUSE UNATTENDED` and host `--allow-input`.
UNATTENDED acknowledges that nobody will concurrently use local touch/mouse.
Held physical buttons are checked before movement/click and immediately
before movement injection, but PeekQualifier+DoIO is not atomic: physical
press/release racing a synthetic action can still drag or alter button state.
Generic input arbitration remains open. Input setup failure preserves capture
with the input capability off. Never interpret request success as visible
interaction acceptance; inspect the screen later.

## Build and later activation

From the repository root, using the archive/SDK described in the USB README:

```sh
sh arch/riscv-esp32p4/usb/build-prototype.sh AROS_BUILD PINNED_ARCHIVE NEW_OUTPUT diag
sh workbench/tools/RemoteDiag/run-tests.sh NEW_HOST_OUTPUT
```

This produces standalone `USBDiag`, not a new kernel/image. Original `USBEcho`
build/test mode remains available. The shared experimental USB product string
is still `P4 USB echo prototype`; the host additionally validates RDG1 HELLO.
Do not deploy until connector/VBUS/power and the echo transport are qualified.

Later, on AROS, with authorized decimal laboratory VID/PID and explicitly
reviewed root/screen choices:

```text
USBDiag ENABLE VID <VID> PID <PID> SECONDS 600 STEADYCLOCKS NONEAUTH SCREEN Workbench ROOT SYS:Prefs ALLOWMOUSE UNATTENDED
```

Omit ROOT for no file access; omit ALLOWMOUSE/UNATTENDED for screen-only input
policy. Backend availability is reported by STATUS. Invalid root or absent
screen is not silently replaced with a wider root/default screen. NONEAUTH is
mandatory: this local USB mode has no encryption/client authentication and
exposes the granted capabilities to whoever controls the USB host. It must
not be exported to a network. TLS/token/SSH and MCP are later deliveries.
Existing P4 steady-clock/IRQ ownership and fault-quarantine limits still apply.

After target qualification, the host CLI uses explicit inspected coordinates:

```sh
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> status
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> screenshot new.ppm --crop 0 0 800 600
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> --allow-input click 200 300
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> stat Network
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> list ''
python3 workbench/tools/RemoteDiag/client.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDR> download Network new.bin --offset 0 --length 128
```

No reset, driver detach, reconfiguration, first-match selection or replay of
an ambiguous command. A new invocation chooses a fresh random session cookie
and may replace an idle session without resetting USB; pending operations and
undrained replies prevent takeover. Corrupt/oversized framing poisons the link
until transport reset. Host outputs are exclusive; no implicit metadata
sidecars, protection changes or target mutations are performed.

## RDG1 diagnostic wire contract (not final RC protocol)

Little-endian32-byte header: magic `RDG1`, u16 version1, u16 opcode, u32 status,
u32 body length, u32 request ID, u64 session cookie, u32 CRC32. Requests have
status0; replies set opcode bit15. CRC covers first28 header bytes then body.
HELLO ID1/session0 carries a nonzero8-byte random cookie, echoed as the reply
session. Same-cookie active HELLO is rejected. Thereafter IDs strictly increase;
duplicate/stale requests fail without re-executing. Unique internal completion
tickets never repeat during a process lifetime, even across transport resets.

| Opcode | Request | Successful response |
| --- | --- | --- |
| 1 HELLO | u64 new cookie | four u32: version, chunk max, snapshot max, generation |
| 2 STATUS | empty | u32 capability bits(screen1/input2/files4), cached snapshot ID |
| 3 CAPTURE | four u32: x,y,w,h; all zero for full screen | snapshot metadata |
| 4 CHUNK | three u32: snapshot ID,offset,length | exact bytes |
| 5 RELEASE | u32 snapshot ID | empty |
| 6 MOVE | u32 x,y | empty |
| 7 CLICK | u32 x,y,button(1left/2right/3middle) | empty |
| 8 STAT | NUL-terminated relative path | native metadata |
| 9 LIST | u32 index then path | native metadata, or END |
| 10 READ | u32 offset,length then path | snapshot metadata |

Snapshot metadata: five u32 ID,size,width,height,CRC32; files have dimensions0.
Native metadata: u32 protection,size; i32 type,days,minutes,ticks;108-byte name
and80-byte comment, NUL-terminated. Status0..9: OK,BAD_REQUEST,DENIED,
UNAVAILABLE,IO,CHANGED,LIMIT,SESSION,BUSY,END. No path/content in error replies.

## Evidence limits

Host tests use actual parser/dispatch/backend source with fake AROS API layers;
a fragmented Python-to-C pipe also tests wire compatibility. They do not
prove real DOS handlers, Intuition effects, SMP scheduling, USB traffic or
electrical operation. Keep actual capture/input/DOS/transport comparisons,
physical touch coexistence and watchdog/debug/WLAN regression open. No merged
source or successful build closes RC-M1..M9 or the WLAN goal.
