# Pinned ESP-Hosted reference for JC1060

Source audit, 2026-10-10. This is not a build or hardware acceptance.

## Provenance

Use the complete official host at
`espressif/esp-hosted@d5184217f710d54c01f932e75b4c55e3f4933fc9`
as the next comparison candidate, rather than current main or registry version
0.0.6. The board source is
`cheops/JC1060P470C_I_W@6304eddef32a33aa9d9a85e971e39dfb24c3cca3`,
under `1-Demo/Demo_IDF/examples/slave/main/common/`.

Primary comparison against the official archive's root `common/` confirms
byte-identical schema and generated protobuf-C files:

| File under common/proto | SHA-256, both sources |
| --- | --- |
| esp_hosted_rpc.proto | bbdbc9bb69c2b1793de6441ea55051c3ceb3f7b30994227b8ff1e995b0137dec |
| esp_hosted_rpc.pb-c.c | 693efbce5db14bf6f2416f5fcf746049a459764ec9f8571a45f2b6a7bdffcc69 |
| esp_hosted_rpc.pb-c.h | ad4f03ade1ca38d88354cc3104f1f4838f7c25004197e19a8fac395c1ad05e85 |

`common/include/adapter.h` differs only by an HCI comment and vendor UART
extended-capability bits 8/9; its SDIO payload-header layout is unchanged.
The earlier interpretation that the vendor removed the newer RPC IDs was
reversed: the vendor matches d518, which adds them relative to registry 0.0.6.
Preserve this correction; do not infer compatibility from the version label.
The installed C6's `54aafd0-dirty` identity remains unresolved. Source lineage
does not establish the identity or behavior of its installed binary.

Official archive download:
`https://codeload.github.com/espressif/esp-hosted/tar.gz/d5184217f710d54c01f932e75b4c55e3f4933fc9`
SHA-256 `1310dc05283b6adefa3ec217c017c7a8836801e263115c7192f9d8ef7d435c12`.
Local inspection workspace: `/Volumes/Dev/Build/esp-hosted-reference.RzWnFu`.
Vendor comparison snapshots: `/tmp/esp-hosted-audit.QD6qYL` (temporary;
re-fetch pinned source paths to reproduce).

## Build and deployment gates

### Native serial fragmentation contract

Pinned `slave/main/app_main.c:63,606..650` defines ETH_DATA_LEN1500 and
increments a static uint16 sequence before each serial fragment. MORE is set
on nonfinal fragments; the sequence increments within a message, including
65535-to-0 wrap. The entire pserial TLV stream, not independently decodable
TLVs, is split into chunks. `slave/main/protocomm_pserial.c:76` composes a
six-byte endpoint TLV followed by a DATA TLV with uint16 length.

`hosted_epoch_reassembly.c/h` is now linked into native control exchange.
It owns a4108-byte buffer, validates the final serial TLV and
allows up to4096 protobuf bytes and16 fragments. These are local defensive
caps, not negotiated slave limits. Within a partial message it requires the
next sequence; between complete messages it does not assert continuity.
The synchronous diagnostic accepts SERIAL/0 only and fails on other interfaces;
the future mixed data/control dispatcher must route them separately.
The owner serializes all access, captures
the transport generation and supplies one aggregate deadline. The helper has
no clock, I/O, allocation or transport-counter reset. Consumed malformed
input wipes/poisons its assembly; the owner must also invalidate the matching
transport and abort partial assembly on timeout/cancellation/failure before
any subsequent RPC. Stale feed/view/release/abort preserve replacement state.
Only the exclusive fresh-epoch owner may start a different generation; this
operation cannot order arbitrary generation numbers or identify stale owners.

The pinned SDIO slave zeroes its payload header in
`slave/main/sdio_slave_api.c:431` and fills only interface/length/offset/
sequence/flags at434..439. Its SERIAL packet type and throttle field are zero.
SDIO flow control uses START/STOP_THROTTLE interrupts, handled in
`host/drivers/transport/sdio/sdio_drv.c:685..688`; SPI header throttling is not
this board's transport contract. Native reassembly enforces the SDIO source
profile, not an established identity of the installed C6 binary.

Control stores assembly and up to4096 response-body bytes in the caller-owned
result, allocated off the small runtime stack. Requests remain capped at256B;
each committed receive batch remains capped by the512..4096B transport buffer.
All frames in the committed batch are checked after a match, and a trailing
unfinished message fails rather than being carried into the next RPC. Every
terminal partial-message failure aborts/wipes its assembly and invalidates
only the matching FIFO generation. A local preflight refusal changes neither
output nor transport. No persistent UID allocator or retry was introduced.

The earlier timestamp-based `hosted_reassembly.c/h` prototype and its tests
remain unchanged and unlinked. The new helper addresses explicit generation
ownership, poisoned cancellation and freestanding no-libc requirements;
it does not silently replace the older API. Host integration tests remain
distinct from elapsed-deadline or hardware acceptance.
Host models validate fragment assembly, not protobuf semantic validity,
elapsed timeout behavior, installed C6 compatibility or WLAN operation.

1. Build the actual root component and its complete transport/bootstrap/RPC
   implementation. The old host example CMake files name stale directories;
   do not repair that by inventing a replacement protocol. A small application
   using the existing APIs is the intended reference front end.
2. Primary build target remains ESP-IDF 6.0.1. ESP-IDF v5.4.3 is downloaded with submodules, commit
   `ea1c174c1cbb7348bd8ba0ff1eb306246938dd80`; its fetch process exited 0.
   Its isolated tool installation exited 0; it is a comparison option only,
   not a port downgrade or a proven requirement. The official host manifest
   requires >=5.3. Its first actual build under 6.0.1 fails; see below.
3. `esp_wifi_remote` 0.1.12 requires exactly IDF 5.3. Candidate 0.2.3 supports
   IDF >=5.3, but declares an unconditional eppp_link dependency. Resolve and
   pin the dependency closure, then verify host API compatibility by build.
   No dependency compatibility is claimed from this audit alone.
4. Board adaptation must forbid reset GPIO access when reset is absent.
   Official `reset_slave()` unconditionally configures/writes its pin;
   `MAX_RETRY_TRANSPORT_ACTIVE` is 1000. Replace this policy with an explicit
   bounded, fail-stop reference gate and record the exact source patch.
   JC1060 has `WIFI_RESET_GPIO=-1`; GPIO54 is not established on this board.
5. Use verified JC1060 SDIO pins CLK18/CMD19/D0..D3=14..17. Audit all
   implicit constructor initialization and task/error paths before deployment.
   Preserve C6 firmware, SD media, bootloader and partition table. No OTA call.
6. A full host performs function/IRQ/bootstrap writes, including OPEN; this
   changes powered C6 state. P4 backup/rollback cannot undo those effects.
   Quiesce existing scanout before a reference handover; independently back up
   and read back the exact P4 write span and restore the normal baseline.
7. First reference acceptance is bounded startup, MAC and scan, followed by
   explicit association/frame/recovery gates. A working IDF reference is not
   an AROS WLAN driver. Port OS/bus glue and integrate SANA-II only after the
   wire contract and behavior are demonstrated.

No further hardware probe was started during this source audit.

## Actual IDF 6.0.1 build attempt

The build-only front end is saved in `tools/hosted-reference/`. It selects the
complete external host and remote component; it does not bypass RPC/transport.
It is not a scan/association test or an approved firmware image. The tested
copy is `/Volumes/Dev/Build/esp-hosted-reference.RzWnFu/reference-app`.

Build environment: local IDF commit
`8c19b156084a0753687347cca1f5355782893533`, version 6.0.1, with pre-existing
bootloader and esp_hw_support changes left untouched. Python environment
`/Users/fabian/.espressif/tools/python/v6.0.1/venv`, GCC
`esp-15.2.0_20251204`. Explicit `IDF_PYTHON_ENV_PATH` is required; otherwise
idf.py looks for a nonexistent standard Python environment. Set IDF_PATH,
HOSTED_REFERENCE_ROOT and ESP_IDF_VERSION=6.0.1; run `idf.py -B build-v6 build`.
Component manager is disabled (`IDF_COMPONENT_MANAGER=0`) for the explicit
local component closure. Remote's EPPP sources are excluded by HOSTED selection;
no EPPP component is used in this build. This does not validate EPPP dependencies.

The official gitlink for `common/protobuf-c` is
`abc67a11c6db271bedbb9f58be85d6f4e2ea8389`. The codeload archive omits submodule
content; supply that exact checked-out revision, not the latest protobuf-c.

Actual attempts, each terminal process exit 2 after environment setup:

- `build-v6.log`: missing declared `esp_driver_sdmmc` dependency.
- `build-v6-dependencies.log`: additional DMA dependency and libc endian-macro
  redefinitions. External CMake now declares esp_driver_sdmmc, esp_driver_dma,
  esp_wifi_remote; common.h guards htole16/le16toh instead of suppressing warnings.
- `build-v6-dma.log`: missing GPIO dependency, removed public
  esp_dma_mem_info_t/esp_dma_capable_malloc, removed wifi_sta_config_t
  reserved/he_reserved fields, and two new HTTP event cases. This is concrete
  API incompatibility, not a working reference build. No image accepted/flashed.

The compatibility bridge is now retained as `tools/hosted-reference/idf6-compat.patch`,
SHA-256 `0232d1d573929724a076677cd4e521f0cbf5ebf382be5e0f324a7426131426cb`.
It applies cleanly to a freshly extracted pinned archive (`git apply --check`,
exit0). No global compiler-warning suppression or wire-schema change.
DMA uses overflow-checked 64-byte rounding/alignment and deliberately stricter
DMA|INTERNAL|8BIT placement, with PSRAM builds rejected. Reserved STA/HE wire
bits are zero on request and ignored on response, not remapped into IDF6's new
feature positions. Two new HTTP event variants are explicitly handled.

Full build session64334 exits0 under IDF6.0.1. Its first image379040B
SHA-256 `4c60061ab7c44db14f4bc645468cea65154fad80994e030e39b131c1e01dcdb9`
targets revisions3.1..3.99 and is rejected for the JC1060. IDF6 requires
ESP32P4_SELECTS_REV_LESS_V3=y in addition to REV_MIN_100. Corrected full
build session85767 exits0;377856B SHA-256
`8c6d4b967adf9820714b0beb1200c3a1320e337758aea65bba4b6220482b34bd`.
Image-info validates P4, revision1.0..1.99,16MB DIO80MHz. Build log
`build-v6-rev1.log` SHA-256
`6ab1bd3d9055a999bbf1efd12cf89727a327fff301ddc772b2a2d6825a093a60`.
The normal generated flash commands would overwrite the AROS layout; forbidden.

Independent actual-source allocator tests and primary reruns pass strict C11
O0/O2 ASan/UBSan,10 cases/41 checks each, exit0. Test extracts the external
function verbatim and checks overflow, rounding, exact allocation arguments,
failure and pointer forwarding with a heap stub. It does not test actual P4
memory placement or DMA. Retained test script SHA-256
`c13855a062f517c30de329de12580ecaee6503c60f5260a741bce9d4c44275b4`;
`allocator-tests-primary.log` SHA-256
`5786a8b111f0febef67f2be1aa240cc84223ec95aeb60c5701b16c8274b327da`.

This is build verification only. No serial/device activity. The reset/timeout,
constructor and task failure policies still require adaptation before a run.
The front end at that stage initializes Wi-Fi only; MAC/scan/association/frame
and recovery acceptance remain open. Do not claim that 5.4.3 is mandatory.

## Bounded MAC/scan candidate

Apply `idf6-compat.patch`, `bounded-reference.patch`, then `scan-safety.patch`
to the pinned archive, checking each before application. Primary reproduction
in `/Volumes/Dev/Build/hosted-reproduce.6qcg7E` exits0 and matches the actual
external host tree byte-for-byte. All three pinned protobuf hashes remain
unchanged. No replacement transport or RPC implementation.

The bounded patch removes implicit initialization/destruction, forbids reset
GPIO access for -1, uses a ten-second connection deadline and propagates
connection failure. Card-init, OPEN and worker-creation failures abort under
PRINT_HALT. The app starts its forty-second timer before explicit init, checks
each API before/after deadline, validates a unicast MAC, performs one blocking
scan and retrieves at most eight records before stopping Wi-Fi. Cooperative
timer dispatch cannot guarantee a hard deadline if scheduling/hardware hangs.
Persistent workers are not automatically torn down after application success.

The old scan wrapper copied the slave count without capacity validation and
never published that count. The parser also trusted repeated counts, pointers
and byte lengths and could report malformed replies as successful because
calloc initialized status to zero. The scan patch corrects these concrete
paths, validates MAC length and defaults decode failures to a nonzero error.
The pinned slave rejects empty get_records; the app therefore skips that API
after a successful zero-AP scan, preserving C6 firmware. This is an explicitly
named empty-scan compatibility workaround, not a slave API root fix.

Actual-source startup models pass strict O0/O2 ASan/UBSan:6 cases/49 checks
each. The final build process30271 exits0, image380304B SHA-256
`12383bd32aee7c2f4ad40dae48f3a58aa141a9cf9ba67c7f4190ca7237c5aa27`;
`build-v6-scan-safe-final.log` SHA-256
`5ca0d68d912fd73b1b9e46cb7fab9ef8aa2e27311a88c136c28e627e69da77bf`.
P4 revision1.0..1.99,16MB DIO80, PRINT_HALT, PSRAMoff, reset=-1,
slot1/1bit/400kHz unchanged. Scan/application tests and independent runtime
review are pending. No hardware deployed or accepted. Association, Ethernet
frames, native AROS/SANA-II integration, coexistence and recovery remain open.

A subsequent source correction removes the second mutex creation after worker
launch: replacing a live global lock could split bus ownership and leaked the
initial mutex. Session49737 exits0; final380240B image SHA-256
`a765b9f65617dddd52e3b670dc09310e4c4a64209313809a7923c8fc8d3e3a5a`;
`build-v6-lock-safe.log` SHA-256
`64d66f375af9c121ce4928822e31929b844c3c98407a6ad23f4f5ba21421ff4a`.
The earlier image above is superseded, not deployed. This correction is retained
in bounded-reference.patch; rebuild and review remain separate from hardware.

## First bounded hardware attempt: startup failure, recovery pending

Final actual-source startup/scan/app models and independent review pass (see
ROADMAP dated evidence). The380256-byte 2e92b5e6... image was explicitly
written/readback verified at0x20000 after quiescence. It fails before app entry
at esp_clk_init: the generated400MHz setting is unsupported on revision1.3.
No Hosted initialization or MAC/scan acceptance follows. Defaults now select
360MHz and check-config.py enforces21 required generated settings. Corrected
build380256B SHA-256349e8c588870d98e938055ac008dd3d5e4de5b430b298233c7bd7c94ad6ba900
passes, not deployed.

The entire380928-byte original core span was restored and independently
verified, SHA-2561880a2f41f07fe14a32df7dde2dd4a09119248e6b67442967b49b8b95ce3e62e.
Its return capture instead faults in ROM at store address0x50091000 and
watchdog-resets. Byte-identical flash restoration is not boot recovery.
Board identity is verified and the board is left in download loader; retained
state/root cause remains open. No more reference deployment before baseline
recovery. Bootloader/partition/BSP/SD/C6 firmware were not written. Empty-scan
workaround and its separate N1E uncompensated removal test remain open.
Subsequent current-state reading finds the240MHz parent gate off while Crypto
selects that parent (mux2, established by esp_crypto_clk.h). Exactly one masked
bit30 enable at0x500e6028 changes0x00170503 to0x40170503 and restores a complete
30.948s baseline return. This is a manual recovery workaround, not a permanent
fix or rail-off gate; N1R tracks warm/early-failure return without compensation.
Board now runs baseline AROS. Corrected360MHz reference is still not deployed.

## Corrected360MHz reference hardware gate

The subsequent exact349e8c58... candidate passes one full MAC/scan run under
IDF6.0.1. C6 INIT chip13 (0x0d)/capabilities0x0d includes WLAN; MAC10:bd:a3:9b:bc:34,
13 APs/eight records, result=ok at4.085s. Complete45.822s capture has no
additional reset/fault. Entire380928-byte baseline span restored/readback
matched; complete30.925s AROS return passes without another parent-clock write.
See ROADMAP dated evidence for complete hashes, sessions and procedure.

Correction (2026-10-10): the existing capture prints `ESP board type is : 13`.
The later `Slave chip Id[12]` prints the hexadecimal TLV tag constant0x12,
not its chip-ID value: pinned `transport_drv.c`, `send_slave_config()` logs
ESP_PRIV_FIRMWARE_CHIP_ID. The earlier chip12 interpretation is withdrawn;
the capture itself is unchanged. Chip ID and capability bits are distinct
fields even though both observed values are0x0d.

This is a measured complete official-host reference, not a native AROS Wi-Fi
driver or an association/frame/coexistence/recovery pass. It proves installed
protocol compatibility for these calls, not installed binary/source lineage.
The empty-scan workaround was not exercised (13 APs); N1E remains open.
N1R still needs uncompensated early-failure return. Reference function/OPEN
mutations persist in powered C6 state; P4 flash restoration does not undo them.
Use the exact working card/bootstrap path for native comparison rather than
infer firmware incompatibility from prior timeouts. C6 firmware unchanged.

## Recovered working host WiFiInit profile (2026-10-10)

Use the actual reference-app/build-v6 configuration, not the separate
idf_sdio_reference project or a new SDK's unexpanded defaults. Its built app
and CPU360 readback are byte-identical SHA-256349e8c588870d98e938055ac008dd3d5e4de5b430b298233c7bd7c94ad6ba900.
The generated sdkconfig.h is SHA-256555142211028a03b54d50b339ebb09660793ddf2257fdf905d47b8199048c5cf.
Expanding IDF6.0.1 WIFI_INIT_CONFIG_DEFAULT() with that build's reference.c
compile command, followed by its explicit nvs_enable=0 override, yields:

| RPC field | Name | Value |
| --- | --- | --- |
| 1 | static_rx_buf_num | 10 |
| 2 | dynamic_rx_buf_num | 32 |
| 3 | tx_buf_type | 0 |
| 4 | static_tx_buf_num | 16 |
| 5 | dynamic_tx_buf_num | 0 |
| 6 | cache_tx_buf_num | 0 |
| 7 | csi_enable | 0 |
| 8 | ampdu_rx_enable | 1 |
| 9 | ampdu_tx_enable | 1 |
| 10 | amsdu_tx_enable | 0 |
| 11 | nvs_enable | 0 (explicit app override) |
| 12 | nano_enable | 0 |
| 13 | rx_ba_win | 6 |
| 14 | wifi_task_core_id | 0 |
| 15 | beacon_max_len | 752 |
| 16 | mgmt_sbuf_num | 32 |
| 17 | feature_caps (uint64) | 129 /0x81 |
| 18 | sta_disconnected_pm (bool) | true |
| 19 | espnow_max_encrypt_num | 7 |
| 20 | magic | 0x1f2f3f4f |

These are recovered semantic host values, not a captured RPC packet or proof
of installed C6 source/binary identity. The pinned host maps these20 fields
individually; the slave replaces all20 defaults with decoded values. IDF6's
additional struct members/pointers are not part of the legacy schema and must
not be serialized by copying its raw layout. Native hosted_wifi.c now encodes
the typed body but deliberately provides no default profile. The caller must
explicitly select and validate its compatibility profile. Native runtime
initialization/cleanup is now integrated as a bounded fresh-epoch diagnostic;
hardware success remains a separate open gate. Main independently reproduces
the expanded initializer from the exact recorded compile command (-c replaced
by -E -P, -o/object removed, build cwd and other flags preserved). IDF_VER is
v6.0.1-dirty; no clean-IDF provenance is claimed. Evidence
reference-config-preprocessor.log SHA-256
d6b25bcfbf3f276425cdcf6a6a6eac0fa50f771d6ffd56e74469345d036f2941
is retained under native-control-exchange.KnVrtu. The new pinned generated
protobuf-c interoperability oracle passes810 O0/O2 ASan/UBSan checks each;
all20 Init values agree semantically, including explicit native zeros versus
proto3 omitted defaults. This does not prove installed slave firmware identity.

## Native association prerequisites (source audit, 2026-10-10)

Pinned common/proto/esp_hosted_rpc.proto defines SetConfig request284 /
response540, Connect282/538 and Disconnect283/539. SetConfig body has iface
field1 and wifi_config field2; STA is wifi_config oneof field2. Connect and
Disconnect request bodies are empty; response status is int32 field1 with
proto3 default0. Success of Connect's RPC is not an association event or IP
connectivity proof. Existing native control does not yet allow these calls.

The STA schema uses bounded byte strings: SSID32, password64, BSSID6. Do not
copy an IDF wifi_config_t ABI to the wire. Mandatory nested threshold field9
and pmf_cfg field10 must be emitted even when empty: pinned
slave/main/slave_control.c:829-834 dereferences them without null checks.
The generic request limit remains256 bytes; maximum-value configurations
must be sized before writing and refused unchanged if they exceed it, not
silently truncated. No real credentials may appear in test fixtures or logs.

Typed connected/disconnected events use IDs775/776 with outer status field1
and nested record field2 (proto:1525-1532). Their SSID byte field and explicit
ssid_len must be checked against each other under the actual slave encoding;
do not assume byte-field length always equals SSID length. Records must be
owned beyond RX buffer reuse and tied to the live generation/state machine.
Association, data-frame traffic, DHCP, cancellation and reconnect are separate
acceptance gates; none is implemented or accepted by this source audit.
The pinned slave emits a fixed32-byte SSID array plus its meaningful length,
not a variable byte field of that same length (slave_control.c:2449-2461,
2481-2492). Decode the wrapper for the actual event: the pinned host wrapper
incorrectly reads disconnected status through event_sta_connected
(host/drivers/rpc/core/rpc_evt.c:153); do not reproduce that defect.

Credential privacy requires a separate check before transmission: the pinned
slave handler logs SSID at ESP_LOGI and password at ESP_LOGD
(slave_control.c:815-819). Installed54aafd0-dirty lineage/log level is unresolved,
so suppression cannot be assumed. No credentials have been supplied or sent in
this audit. Do not collect secret-bearing slave debug output as evidence.
The same handler calls strlen on fixed arrays after bounded memcpy; full-width
non-NUL SSID32/password64 are not proven safe even though schema-valid.
Codec support must not be confused with safe installed-slave execution.
Any shorter-string development restriction must remain a named workaround,
not a permanent replacement for correct32-byte SSID/64-byte key support.

Standalone native codec qualification (2026-10-10): hosted_association.c now
encodes all fifteen known STA fields, correlated284/540,282/538,283/539 and
owned775/776 events. Mandatory threshold/PMF exist even when empty. It rejects
alias/overflow/invalid lengths and stages writes; failed encoding leaves output
and written unchanged, transient credential copies are volatile-wiped. A
nonzero event status validates any nested record but publishes zero data fields.
The caller must protect and wipe successful credential-bearing output.
Independent generated protobuf-c confirms a full-width all-value envelope is
258 bytes (STA237, cfg240, request243, envelope258), so the256-byte cap refuses
it transactionally. Full-width byte strings with supported fitting values are
accepted; schema support is not proof of safe installed-slave strlen behavior.
Self-tests239/oracle603 checks each O0/O2 sanitized pass. The oracle runner
pins schema, generated files and protobuf-c hashes and uses a no-network Colima
snapshot with write bits removed; it does not claim root-proof immutability.
No runtime/control allowlist or installed firmware changes are part of this
qualification. Persistent generation-scoped event/UID handling remains needed.

## C6 firmware identity and recovery gate (2026-10-10)

The pinned slave's console version printer reports a semantic version, not a
whole-image digest. Its SDIO initialization event advertises chip/capabilities,
not a verified source commit or binary hash. Installed `54aafd0-dirty` therefore
does not identify exact bytes. An application descriptor/ELF hash is not the
full flashed `.bin` hash either. No credential transmission is authorized by
matching a protocol or successful MAC/scan alone.

Primary source audit in `hosted-reproduce.6qcg7E` confirms:

- `slave/main/slave_control.c:819` has plaintext password DEBUG logging;
  `slave/main/app_main.c:325` has a VERBOSE serial-payload hex dump. Compiling
  those paths out can protect a known artifact, but says nothing about the
  unknown installed binary. Do not collect secret-bearing output or NVS.
- OTA_BEGIN selects the next update partition and calls
  `esp_ota_begin(..., OTA_SIZE_UNKNOWN, ...)` (control:345-353); it can erase
  the inactive fallback. OTA_END validates then changes the boot selection
  before allocating/starting its reboot timer (428-455). A failure response
  after that change does not prove the previous boot selection remains intact.
- Two OTA slots and redundant otadata do not prove automatic rollback of a
  defective app. Rollback configuration and app confirmation must be verified.
  See [official C6 OTA documentation](https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32c6/api-reference/system/ota.html).
- The complete IDF host wrapper defaults to HTTP URL OTA (`rpc_wrap.c:45-47`),
  not the file helper. It is not a verified authenticated native AROS updater;
  native AROS currently has no OTA client. Do not use stock OTA as recovery.

The pinned board schematic offers a candidate direct C6 UART recovery route
through CN5: VCC3V3/GND/U0TXD/U0RXD/IO9/CHIP_PU. This is schematic evidence,
not confirmed connector population, orientation or continuity on the active
JC1060-v2. Before connecting anything, obtain fresh physical readiness and
verify the actual revision/pin-1 and nets. A powered board must not receive a
second supply from an adapter's VCC. See the
[pinned board schematic](https://github.com/cheops/JC1060P470C_I_W/tree/6304eddef32a33aa9d9a85e971e39dfb24c3cca3/5-Schematic)
and [official C6 boot-mode guidance](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/advanced-topics/boot-mode-selection.html).

Safe next gate, not an executed procedure: explicit C6 write authorization;
verified physical restore path; non-secret security summary and actual
partition/active-slot discovery; app/bootloader/table/otadata backups where
readback is permitted, excluding potentially credential-bearing NVS; pinned
privacy-safe source/configuration and image identity; fit/signature/encryption
checks; controlled write/readback and restore verification. Secure Download
or encryption may prevent useful readback; do not assume recoverability.
No C6 flash, serial interaction, backup or credential use occurred in this audit.
