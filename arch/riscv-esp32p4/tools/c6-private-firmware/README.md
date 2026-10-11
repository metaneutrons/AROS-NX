# Private C6 firmware candidate

This recipe creates an undeployed candidate. It does not authorize flashing,
establish installed C6 identity, or prove recovery. Never use the generated
`flash_args` before the documented actual-device recovery gate passes.

Source: ESP-Hosted archive for d5184217f710d54c01f932e75b4c55e3f4933fc9,
archive SHA-256 `1310dc05283b6adefa3ec217c017c7a8836801e263115c7192f9d8ef7d435c12`.
Use the complete matching protobuf-c dependency, not the earlier empty
reproduction directory. Apply `privacy.patch` to a separate source copy using
`git apply --check` followed by `git apply`. Copy `sdkconfig.private` into its
`slave` directory. Preserve the original archive and source hashes.

The patch removes Hosted SSID/password/current-AP logs and SDIO, serial,
Ethernet and HCI payload dumps regardless of later DEBUG/VERBOSE settings.
It bounds the response-string length read to the field width. Valid terminated
and full-width fields retain the previous intended length convention. Generic
Wi-Fi MAC/status logs and IDF/vendor-library logs are not all suppressed.
Do not claim whole-binary or hardware privacy certification from this patch.

## C6 vendor logging extension (build verified, undeployed)

`idf-wifi-privacy.patch` applies to the pinned IDF 5.4.3 commit
`ea1c174c1cbb7348bd8ba0ff1eb306246938dd80`, specifically its C6 Wi-Fi OS
adapter. Apply it with `git apply --check` and `git apply` only to a separate
complete IDF source copy, then select that copy through `IDF_PATH`. Preserve
the reference IDF and the previous candidate/evidence unchanged.

Both vendor `_log_write`/`_log_writev` callbacks become unconditional sinks:
they neither format arguments nor forward to ESP logging. This deliberately
disables callback-routed vendor Wi-Fi diagnostics at every level, while ordinary Hosted
status/error logs remain available. A runtime tag/INFO setting alone is not
the privacy policy. The IDF constructor sets Wi-Fi tags to the default level,
and `esp_wifi_init()` sets the vendor level from the configured maximum.

Run `python3 test-vendor-log.py PRIVATE_IDF` to exercise the exact patched
callback bodies at O0/O2 with ASan/UBSan. Each run checks18 cases; the original
IDF must fail the sink-call assertion as a negative control. Additional source
guards require the reviewed no-op bodies and expected table-binding text, so
a future direct printf/ROM-output replacement cannot silently pass this test.
The fixture mocks the downstream logger and does not execute the adapter
table, vendor blobs, IDF startup or a radio. Review actual table bindings separately.

Apply `hosted-metadata-privacy.patch` to the Hosted tree after `privacy.patch`.
It removes the remaining Wi-Fi/Bluetooth MAC and direct country/HE AP diagnostic
outputs without changing RPC payloads or Wi-Fi event forwarding. Verify patch
application and retain all three patches with the build inputs.

A fresh complete build with both extensions now generates a1105760-byte app,
SHA-256 `d4a992b9aa806d61b0e406b848dfc4e449f3a06eab102120b84453c7ae7c358e`.
The previous1106624-byte candidate is unchanged. The new ELF's two logging
callbacks have no outgoing calls; the variadic callback still has ordinary
ABI register spills. Vendor format strings may remain despite those sinks.
Direct stdio/ROM and PHY diagnostics require link-map/reachability review;
callback suppression does not prove whole-firmware UART privacy. Actual UART
qualification after secured recovery remains required before credentials.

## C6 legacy formatter extension (build verified, undeployed)

Apply `idf-legacy-log-privacy.patch` with `git apply --check` and `git apply`
to the separate pinned IDF tree as well. Wi-Fi, coexistence and PHY legacy
shared formatters otherwise bypass the OS callback sinks through ESP_LOGI.
For C6 only, they ignore all arguments and return zero printed characters;
public wrappers retain ownership of their va_list. Other targets retain the
existing body. This deliberately loses these vendor diagnostics at all levels.
Direct static error-message puts branches remain outside the helpers.

Run `python3 test-legacy-log.py PRIVATE_IDF`: actual extracted bodies are
compiled at O0/O2 with ASan/UBSan, six checks per helper, and exact no-op source
guards. The original Wi-Fi helper fails the negative control. Fixtures mock
the output sink; neither this test nor the build proves runtime UART privacy.
The fresh extended app is1105312B, SHA-256
`149ec760ce40bbf159d1d780f001f44de19477b5076e40a91a15764410ce0b86`.
Keep all four patches and actual build inputs with the evidence. Earlier
candidates are preserved. Do not enable credentials before the hardware gates.

Build environment used: IDF 5.4.3, Python 3.12.13 in a private uv venv,
IDF core requirements constrained by Espressif's `espidf.constraints.v5.4.txt`,
CMake 3.30.2 and official Ninja 1.12.1, RISC-V GCC 14.2.0_20250730.
Provisioning downloaded dependencies; the subsequent build used local files.
Archive exact resolved Python versions, constraints and IDF/tool identities.

Set `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` to the isolated locations;
prepend venv, Ninja and compiler bin directories to PATH. No global IDF install
or source replacement is required. Disable component resolution with
`IDF_COMPONENT_MANAGER=0` for this dependency-complete pinned tree.

```sh
cd PRIVATE_SOURCE/slave
python "$IDF_PATH/tools/idf.py" -B PRIVATE_BUILD \
  -D IDF_TARGET=esp32c6 \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.private' build
```

IDF loads `sdkconfig.defaults.esp32c6` automatically. Verify the resulting
sdkconfig selects SDIO, C6, maximum INFO, Wi-Fi NVS disabled, and the expected
partition CSV. Archive sdkconfig, ELF/BIN/map, bootloader, partition table,
build logs, source patch, manifest and hashes. A successful partition-size
check applies to this source layout only, not the installed device.

Run `sh test-string-response.sh PRIVATE_SOURCE` at O0/O2 with ASan/UBSan.
This extracts the actual response macro and exercises exact-width fields;
it does not test the full RPC handler, allocation failure, wire/radio behavior,
or compiled IDF library logging. Installed identity, security/backup/restore,
actual UART privacy, native association and data-path acceptance remain open.

## Read-only discovery producer (candidate integrated, not installed)

Current source is discovery version2 with40-byte header/1064-byte maximum and
matched native ABI11 consumers. It detects C6 chip ID and MXX revision, plus
the complete physical16-bit SECURE_VERSION field's burned-bit count through
status-checked esp_efuse_read_field_cnt. Read errors/out-of-range counts leave
that fact unknown while preserving independently valid chip/revision facts.
The app-config-sized esp_efuse_read_secure_version helper is not used. Neither
the full counter nor the app configuration attests installed bootloader policy.
Virtual-eFuse builds publish none of these physical facts. IDF retries reads
concurrent with an eFuse burn internally without a deadline; exclude concurrent
burns and qualify runtime timing rather than claiming a bounded whole RPC.
The previous complete candidate and counts below are historical version1.
A fresh full version2 build now completes941 steps and links the collector,
handler, codec and full-field eFuse counter. App1110928B SHA256
3583e341ea108a087a924c4320833f3cad3c3be436070c1ebf5d80f14292b2f4;
source/build `/Volumes/Dev/Build/c6-discovery-v2.3RE1Sf` and evidence
`/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/wifi-2026-10-11/c6-discovery-v2.dLbUla/`.
The complete archive includes ELF/map/config/generated sources/recipe and logs.
The new table matches the prior candidate; the bootloader differs and must not
be flashed from generated flash_args. Neither is installed-device evidence.
Fresh RPC146/10, dispatcher54/9 and string490 checks pass each O0/O2, alongside
vendor/legacy source-body privacy checks and exact-app ESP32Board preflight.
Static C6 GCC stack reports: collector2192B + parser1120B + RPC48B =3360B,
before outer IDF/library frames in the5120B worker. Runtime stack high-water,
UART privacy, installed-table provenance and recovery remain unqualified.
No C6 write or release gate was enabled.

`discovery.c/.h` collect a bounded normalized internal IDF partition inventory
using the pinned IDF5.4.3 APIs, with shared `boardtool/discovery.c/.h` validation
and the ESP32Board layout parser. This is a producer for the future negotiated
extension, not an existing query on stock Hosted. The candidate recipe below
adds a dispatcher hook, startup capability and generated schema; native P4
negotiation, GUI binding and update permission remain absent.
Do not send speculative discovery requests to the installed C6.

Use `esp_flash_get_physical_size` for detected physical capacity and
`esp_flash_get_size` separately for the usable image-header/driver limit.
The collector records actual API-resolved running, next-boot and next-update
roles after sorting, including unknown or aliased roles on limited layouts.
Boot's API can resolve a fallback when otadata is invalid; it is not an
attestation that raw boot-selection metadata is healthy. A next-update role
equal to running is an inventory fact, never a permissible update target.
Flags normalize IDF encryption/readonly booleans; original raw table flags
and bytes are not preserved. Internal registry enumeration can include runtime
registrations even on the default flash chip; installed-table correspondence
needs separate qualification before the write gate can consume this inventory.

The dispatcher must serialize against OTA/slot selection and all partition
registry mutation, including deregistration/unload. Re-querying the roles does
not replace that exclusion or a peer-epoch check. Both rollback enforcement
policies stay unknown; app sdkconfig/otadata state cannot attest the installed
bootloader. Virtual-eFuse builds also leave hardware security facts unknown.
Only NOT_SUPPORTED/NOT_FOUND state queries become unknown; unexpected API errors
refuse the snapshot and leave caller output untouched.

Host tests mock IDF metadata APIs, not flash/radio behavior. Native object builds
use real C6 headers/config/compiler and include `workbench/tools/ESP32Board` for
layout.h; isolated object tests do not produce integrated firmware. Runtime stack
margin, live protobuf/framing behavior, native capability/version negotiation,
freshness/correlation and live owner dispatch remain open. The first installation
cannot rely on discovery code not yet installed and still requires independently
secured recovery. Candidate privacy/recovery/credential gates remain unchanged.

Run `sh arch/riscv-esp32p4/tools/run-c6-discovery-tests.sh` from the repository.
The actual collector, shared validator and layout parser pass35 cases per build
under ASan/UBSan at O0/O2:205 checks with normal security APIs,204 with virtual
eFuses. Fixtures exercise capacity errors, iterator exhaustion/release, role
drift, slot aliasing, unknown states, malformed labels and output preservation.
Mutation/allocation sentinels cover the production translation units, not the
actual IDF iterator implementation, allocator or synchronization.

The proposed wire contract is in [discovery-protocol.md](discovery-protocol.md).
`discovery-schema.patch` applies cleanly to the pinned Hosted schema and is
generated in the separate candidate below. Shared discovery_wire.c/.h encode/decode a
canonical1064-byte maximum payload with echoed nonzero nonce and normalized
records. Run `sh arch/riscv-esp32p4/tools/run-c6-discovery-wire-tests.sh`:
45 cases/136 assertions pass each O0/O2 with ASan/UBSan. These byte tests do not
exercise Protobuf framing, epoch/UID correlation, handlers or live transport.

## Serialized discovery RPC integration (build verified, undeployed)

Start with a separate complete Hosted tree carrying both Hosted privacy patches
above and a private IDF tree carrying both IDF privacy patches. Preserve the
baseline. From PRIVATE_SOURCE apply each of `discovery-schema.patch` and
`discovery-integration.patch` using `git apply --check` followed by `git apply`.
Run the repository's `stage-discovery.sh PRIVATE_SOURCE` with `sh`; it copies
the collector, handler, shared validator/codec and layout parser into a new
`aros_extension` subtree and refuses to replace an existing subtree. Regenerate
the schema from PRIVATE_SOURCE/common/proto:

```sh
protoc-c --c_out=. esp_hosted_rpc.proto
```

The verified generator is protobuf-c1.5.2 with libprotoc36.2. Use the isolated
IDF build procedure above and a fresh build directory. The C6-only CMake branch
links request350/response606 and defines the version1 startup TLV0x17 only with
the handler present; other targets do not advertise it. The worker serializes
collection with its existing OTA handlers. External registry mutations must
also obey the collector exclusion contract. Unknown policies stay unknown.

Before unpacking, the candidate checks positive input length against the shared
4096-byte serial reassembly capacity, not a single SDIO/SPI frame. Dispatcher
failure releases the unpacked request; normal release clears the pointer before
later error cleanup. Reply/snapshot allocation failure and cleanup are bounded.
The full candidate rebuild passes and produces a1110496-byte app. Neither the
newly generated bootloader nor any app has been installed. The bootloader differs
from the prior candidate; its difference is not qualified or authorized to flash.

Run `sh arch/riscv-esp32p4/tools/run-c6-discovery-rpc-tests.sh PRIVATE_SOURCE`:
actual handler, codec/validator, generated protobuf and vendored runtime pass
145 checks/10 cases at both O0/O2 with ASan/UBSan;18 handler allocations and
18 frees. The collector is stubbed. Native serial/frame encoders are exercised,
not the actual IDF serial dispatcher/task/radio. The maximum response fits one
1536-byte SDIO frame (1098 bytes including both wrappers). This does not prove
runtime stack margin: the5120-byte worker still has roughly3.2KB of nested
collector/parser scratch before RPC/IDF frames. Native negotiated owner discovery,
installed-table provenance, secured recovery and real OTA remain open. Keep
UPDATE-C6 disabled and stock peers query-free.

Run `sh arch/riscv-esp32p4/tools/run-c6-dispatch-cleanup-tests.sh PRIVATE_SOURCE`
for the extracted actual dispatcher body:54 checks/9 cases pass each O0/O2
ASan/UBSan with stub dispatcher/protobuf routines. Removing the common request
free fails the two expected ownership assertions. This tests request/response
cleanup and the4096-byte input boundary, not IDF scheduling or real transport.
