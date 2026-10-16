# C6 first installation: independent ROM recovery

Status, 2026-10-11: procedure planned, not executed. The actual JC1060 CN5
connector, UART adapter, ROM access, installed security/layout and restore
remain unverified. This plan does not enable UPDATE-C6 or authorize a write
before Fabian's existing recovery condition is satisfied.

## Separate bootstrap from the product updater

The installed Hosted peer does not provide our new discovery extension.
That extension cannot supply evidence for its own first installation.
Qualify an independent CN5 UART/ROM path first. Prefer an app-only direct
installation if the observed layout and security policy permit it. Do not
require the unfinished AROS OTA frontend or its full-transfer timing gate for
this separate ROM operation. Conversely, ROM installation does not qualify
UPDATE-C6, credential privacy or WLAN operation.

The product CLI/MUI updater remains governed by
[the board-tool plan](esp32-board-tool.md). Its private engine exists, but
public dispatch and device-specific admission producers are incomplete.

## Execution checkpoints

1. **Physical identification, no connection yet.** Obtain a sharp photograph
   of CN5 and the actual board revision. Establish connector family, pitch,
   pin 1 and orientation; verify nets before connection. The pinned older
   schematic labels CN5 as 3.3V, GND, TX, RX, IO9 and CHIP_PU. These labels
   are not verified wiring instructions for this board. Use a 3.3-V logic
   adapter; do not supply adapter VCC to an already-powered board. Arrange
   fresh physical readiness before any cable/button/power operation.
2. **Independent ROM entry and identity.** Establish controlled C6 ROM entry
   and repeat entry independently of its application and the P4. Record the
   actual UART device, C6 identity/MAC/revision, flash capacity and a non-secret
   security summary. Historical C6 MAC is `10:bd:a3:9b:bc:34`; P4 MAC
   `80:f1:b2:d3:3b:a6` identifies a different chip. A USB port listing alone
   identifies neither. The P4 profile has no defined C6 reset GPIO; do not
   infer a working P4-controlled recovery path.
3. **Installed layout, selection and permitted backups.** Establish actual
   partition-table location/bytes, bootloader identity and boot selection;
   candidate CSV/sdkconfig do not establish these. Read only approved,
   non-secret bootloader/table/otadata/app ranges where security permits.
   Read each backup twice into separate newly created files, compare exact
   lengths and SHA-256, and record offset/extent/device/procedure. Exclude NVS,
   keys and credential-bearing data: no whole-flash dump or indiscriminate
   flash scanning. If security prevents usable readback/restoration, stop.
4. **Recovery decision before candidate write.** Demonstrate repeatable ROM
   re-entry and establish a device-specific usable restoration procedure with
   the retained app and relevant boot metadata. Distinguish readback/hash
   evidence from an actually exercised restore/boot verification. If a
   controlled restoration roundtrip itself needs additional write scope,
   resolve that explicitly; do not claim it happened or use the new candidate
   as a substitute for independent recovery. No C6 write proceeds while the
   existing recovery authorization condition remains unmet.
5. **Choose the exact app-only operation.** Check the inspected chip/revision,
   installed bootloader loading/rollback policy, image format, flash mode/rate,
   slot fit and signature/encryption requirements against the pinned candidate.
   Select the app offset from observed boot/layout facts, not generated
   `flash_args`. Preserve bootloader, partition table, otadata, NVS and eFuses;
   if boot selection must change, stop and resolve that distinct operation.
   Account for every affected 4-KiB erase sector, including rounding beyond
   image length; backup and restore scope must cover the complete footprint.
   Tool compatibility warnings are not approval. No `--force`, whole-chip
   erase, security bypass or eFuse provisioning.
6. **Controlled install and verification.** Only after the preceding evidence
   permits the write, record the exact app hash, range, command and actual
   exit status. Independently read back the written bytes and verify identity.
   Then verify normal boot and fresh Hosted MAC/scan without credentials.
   A boot failure requires the established independent recovery procedure,
   not repeated speculative flashing. Record both success and failed attempts.
7. **Runtime qualification before network credentials.** Qualify actual C6
   UART privacy, stack margin, discovery-v2 framing and matched ABI11 P4
   consumers. Then complete native association/DHCP, bidirectional traffic,
   disconnect/reconnect and the 1,800-second combined load gate. Separately
   finish authenticated local/HTTPS sources, live target/policy/recovery
   producers, transfer timing and CLI/MUI updater acceptance. Do not turn
   compile-time release assertions into assumed device facts.

## Retained candidate, not installed-device evidence

Source/build: `/Volumes/Dev/Build/c6-discovery-v2.3RE1Sf`;
app: `build/network_adapter.bin`, 1,110,928 bytes, SHA-256
`3583e341ea108a087a924c4320833f3cad3c3be436070c1ebf5d80f14292b2f4`.
The build's bootloader differs from the previous candidate and is not approved
for flashing. Its partition table matches the previous candidate, not the
actual device. Full build evidence is recorded in roadmap N2M; runtime privacy
and recovery are not established by that build.

## Primary references and limits

- [Pinned board schematic](https://github.com/cheops/JC1060P470C_I_W/tree/6304eddef32a33aa9d9a85e971e39dfb24c3cca3/5-Schematic): logical CN5 route only.
- [Espressif C6 ROM boot selection](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/advanced-topics/boot-mode-selection.html): GPIO9/8 strap requirements; verify actual nets before applying them.
- [Espressif esptool commands](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/esptool/basic-commands.html): 4-KiB erase footprint, readback and security restrictions. Incompatible app images at non-bootloader offsets can produce a warning and still be written; do not rely on esptool as the compatibility gate. Inspect the pinned local tool's behavior/options before execution.

Next required user input: CN5 photograph and subsequent verified adapter
availability. No serial connection, reset, backup or installation was performed
when this runbook was written. Execution results belong in ROADMAP.md, not in
an inferred pass beside these checkpoints.
