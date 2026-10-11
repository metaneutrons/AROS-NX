# C6 application firmware contract v1

Decision date: 2026-10-11. Candidate profile `c6-app-v1`, carried by the
[signed release contract](esp32-firmware-release-contract.md).
Design only: no JSON loader, publisher authentication or UPDATE-C6 admission
is implemented by this document. Existing CHECK-C6 with a user-supplied SHA
remains a read-only diagnostic, not production release verification.

## C1 - Payload

One unchanged, unsigned, unencrypted ESP-IDF ESP32-C6 application `.bin`.
The manifest/signature are sidecars and are never sent as OTA image bytes.
No ZIP/container, merged full-flash image, bootloader, partition table, NVS,
PHY data, signature trailer or encrypted payload is accepted by this profile.
Release signatures are independent of ESP Secure Boot image signatures.
Support for the latter requires a separately qualified profile/parser/backend.

Current `workbench/tools/ESP32Board/image.c` checks 288..1572864 bytes,
ESP magic 0xE9, 1..16 bounded segments, chip ID 13, an app descriptor in the
first segment, XOR checksum and the mandatory appended simple SHA-256.
It rejects trailing bytes and compares SHA-256 over the entire input.
The 1.5-MiB ceiling is a local memory bound, not discovered slot capacity.
The descriptor provides secure version, wafer-revision limits, separate
eFuse-block-revision limits and MMU page-size exponent. Descriptor project/app
version comparison against release metadata is still to be implemented.

The producer records actual IDF/Hosted/source commits, configuration digest,
descriptor version/name and an explicitly qualified Hosted compatibility
profile. No firmware version number alone proves protocol compatibility.
Manifest-provided facts cannot replace binary inspection: repeated facts must
agree exactly. Candidate table/build options do not describe the installed peer.

## C2 - Live admission, independent of the release

Before BEGIN, require independently observed actual C6 identity/revision,
security/installed anti-rollback policy, flash/layout and running/boot/update
slot roles. The selected inactive OTA slot must fit the complete image and
its erase footprint. Never take offsets, partition names, boot selection,
recovery flags or device facts from the candidate manifest. Do not write
bootloader, partition table or user data. Standard OTA may change `otadata`
as part of qualified boot selection; that is not a bootloader replacement.

Authenticate origin, inspect and retain immutable bytes, quiesce the existing
SDIO owner and qualify the original full-transfer deadline. Unknown policy,
layout provenance or recovery refuses admission. Existing discovery-v2 and
COMPARE-C6 do not supply complete proofs; full physical secure-bit count is
not the installed bootloader's effective anti-rollback floor.

## C3 - Transfer and completion

Reuse the existing Hosted protocol/serialized owner. Pinned Hosted commit
`d5184217f710d54c01f932e75b4c55e3f4933fc9` has no write offsets, replay
protection or transactional abort. Never retry ambiguous BEGIN/WRITE/END,
and never send END as cancellation cleanup. BEGIN can destroy the inactive
fallback. Failed/lost END can leave boot selection changed: commit-unknown.
An acknowledged END is waiting-for-reboot, not installation success.

Installation requires fresh post-reboot identity tied to the expected image,
host protocol/health and boot confirmation under the actual rollback policy.
All failure dispositions preserve the maintenance fence described in the
[board-tool plan](esp32-board-tool.md). First installation remains governed
by the [independent CN5 recovery runbook](esp32-c6-first-install.md), not by a
future discovery extension qualifying itself.

## C4 - User interface and qualification

CLI and GUI show installed/candidate version and identity, signature/key ID,
compatibility/unknowns, recovery availability, progress and terminal result.
No mandatory C6 partition graphic or user-selectable flash offset. Optional
read-only partition diagnostics remain provenance-labelled; internal layout
admission is still mandatory. The C6 is a firmware peripheral, not a second
user-managed P4 storage layout.

Acceptance requires the release tests R4, existing real image parser tests,
negative chip/descriptor/checksum/hash/capacity/revision/security checks,
actual installed discovery and independently secured restore, full-image
transport/failure qualification and post-reboot identity/health. No candidate
build or manifest satisfies these hardware gates. UPDATE-C6 remains disabled.

Primary reference: [ESP-IDF C6 OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/ota.html).
Current compatibility is against pinned sources; this reference is not an
automatic migration to the current IDF release.
