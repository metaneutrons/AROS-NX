# ESP32 firmware release contract v1

Decision date: 2026-10-11. Owner: AROS-NX. Normative design for roadmap N2M
and M4-AB; not an implemented publisher, signature verifier or update admission.
The checked-in CI currently has no ESP32 release/signing job. No release
environment, production key or published asset has been provisioned by this change.

## R1 - Release identity and asset inventory

One release manifest authenticates the complete inventory, including the
[C6 application](esp32-c6-firmware-contract.md) and/or
[P4 kernel/BSP pairs](esp32-p4-firmware-contract.md). Binary payloads remain in
their native format. A detached signature over the manifest binds every
listed asset through its exact byte length and SHA-256: this is asset
authentication, not an unsigned checksum list. Unlisted files cannot be installed.

Asset names are `esp32-firmware-manifest.json`,
`esp32-firmware-manifest.<keyId>.sig`, and the payload basenames listed inside.
Do not sign the signature file or put its digest into the signed manifest.
Archive extraction, full-flash images, arbitrary offsets and executable
installation instructions are outside v1.

The manifest is UTF-8 JSON without BOM, at most 65536 bytes, maximum nesting
depth 8 and at most 32 payloads. Reject duplicate keys, unknown fields,
non-integer numbers, embedded NULs, unsupported versions and duplicate asset
names/IDs. JSON parsing is bounded even after signature verification. Signing
covers the exact stored bytes; consumers never reserialize JSON to verify it.

All fields below are required. Object field sets are closed. Identifiers and
versions are ASCII strings of 1..128 bytes; names use only letters, digits,
dot, underscore and hyphen, with no leading dot or path components. SHA-256
values are 64 lowercase hexadecimal characters; Git commits are full 40-character
lowercase SHA-1 IDs for the current repositories. Arrays are nonempty, unique
and bounded by 32 elements. Integer fields are nonnegative JSON integers.

| Object | Fields and meanings |
| --- | --- |
| Root | `format="aros-nx-esp32-release"`, `formatVersion=1`, `repository="metaneutrons/AROS-NX"`, `tag`, `channel` (`stable` or `preview`), `releaseSequence` (1..4294967295), `sourceCommit`, `assets`, `updates` |
| Asset | `id`, `name`, `bytes` (1..33554432), `sha256`, `kind` (`c6-app`, `p4-core`, `p4-bsp-ab`) |
| C6 update | `id`, `target="esp32c6"`, `profile="c6-app-v1"`, `assets` (exactly one c6-app ID), `boards`, `build`, `hostedProtocol`, `discoveryVersion` |
| C6 build | `sourceCommit`, `idfVersion`, `idfCommit`, `hostedCommit`, `configSha256`, `projectName`, `appVersion` |
| P4 update | `id`, `target="esp32p4"`, `profile="p4-ab-pair-v1"`, `assets` (one p4-core and one p4-bsp-ab ID), `boards`, `build`, `layout`, `abi` |
| P4 build | `sourceCommit`, `configSha256`, `sdkId` |
| P4 layout | `id="p4-ab-256k-v1"`, `pairEnvelopeVersion=1`, `minimumFlashBytes` (16777216 or 33554432, matching the board build) |
| P4 ABI | `cpu="rv32"`, `smp` (boolean), `sdkId`, `privateSdmmcVersion`, `privateWifiVersion` (integers 1..65535) |

Every asset ID must resolve once; no unreferenced payloads, mixed kinds or
ambiguous updates for the same board/profile are allowed. IDs select records,
not filesystem paths. Board names must come from supported build profiles.
`hostedProtocol` names an explicitly qualified host/peer compatibility profile,
not an invented semver range. `discoveryVersion` is a positive integer, not a
claim that an installed peer supports it. Descriptor strings have additional
binary-format bounds and must match the inspected C6 descriptor. Build facts
come from the actual build outputs/configuration, never user-entered guesses.
Build configuration must contain no WLAN credentials or signing secrets;
record its exact byte digest, not secret-bearing configuration contents.

Firmware display versions, manifest format, release sequence, binary envelope,
Hosted wire/discovery versions and private AROS ABI numbers are independent.
No reader guesses a newer format or treats a larger ABI number as compatible.
An incompatible contract change requires a new format/profile version.

## R2 - Signature and key custody

Use standard pure Ed25519, not Ed25519ph or a home-grown algorithm.
The signed message is the ASCII bytes `AROS-NX-ESP32-RELEASE-V1` followed by
one NUL byte and the exact manifest bytes. Each `.sig` contains precisely
64 raw signature bytes. `keyId` is the full lowercase SHA-256 of the 32 raw
public-key bytes. It selects only an already trusted embedded key; neither
the manifest, signature filename nor a download can introduce a key.

The private key is an environment secret named
`ESP32_RELEASE_SIGNING_KEY_B64` in GitHub environment `release`:
base64 of an Ed25519 PKCS#8 DER private key. Store it nowhere in source,
artifacts, caches, logs, command arguments or updater images. The signing job
decodes it into a restricted temporary file, masks derived representations,
uses no shell tracing and removes that file on every exit. Secret absence,
wrong algorithm and public/private key mismatch fail the release; no unsigned
fallback. Base64 is transport encoding, not encryption.

Only a dedicated signing job references `environment: release`, after an exact
tag/commit check, qualified builds and a complete inventory. Use environment
review/tag restrictions, least-privilege permissions, pinned actions and an
isolated runner. PR/fork code never runs with this secret. A signing job must
not execute scripts from untrusted artifacts or accept arbitrary caller-supplied
artifact repositories/run IDs. It verifies the provenance, exact producer
commit/configuration and inventory before signing; signatures do not make an
unqualified build safe. Upload a draft, redownload and verify exact bytes/set,
then publish. Never overwrite assets under a published tag. GitHub provenance
attestations/SBOMs may supplement this offline-verifiable signature.

The user's explicit environment-key requirement replaces a keyless-only
signature policy for these embedded firmware assets. It does not authorize
creating/uploading a production key or bypassing GitHub protection rules.

## R3 - Updater trust configuration and release discovery

Build inputs (proposed names):

- `AROS_ESP32_RELEASE_REPOSITORY=metaneutrons/AROS-NX`
- `AROS_ESP32_RELEASE_API_URL=https://api.github.com/repos/metaneutrons/AROS-NX/releases`
- `AROS_ESP32_RELEASE_BASE_URL=https://github.com/metaneutrons/AROS-NX/releases/download`
- `AROS_ESP32_RELEASE_KEYS_FILE`: reviewed, non-secret file containing trusted
  key IDs and raw 32-byte public keys in lowercase hex.

The keys file is a JSON array of 1..4 closed objects with fields `keyId`
and `publicKeyHex`; reject duplicates and derive/check each key ID from its
public key. No URLs, private material or remotely resolved key IDs are allowed.

A build generator validates these inputs and emits a compiled configuration
used by the common CLI/MUI backend. Missing keys mean updates unavailable,
never trust-on-first-use. No private key is a build input. Log public key IDs
and configuration identity for reproducibility. Normal on-device `ENV:` or
`ENVARC:` values may choose an allowed channel/check interval, not replace the
repository, origin or keys. A separate custom-trust development build is visibly
labelled and cannot silently share the official production trust profile.

GitHub API metadata and `latest` are discovery hints, not authenticity.
Fetch the bounded manifest/signature, verify with embedded trust, validate its
repository/tag/channel/profile, then derive payload URLs from the configured
base, signed tag and signed basenames. Never use manifest-provided URLs.
Validate/encode tags as a single path segment; refuse traversal, controls and
query delimiters. HTTPS and TLS certificate verification are mandatory;
redirects must remain HTTPS on a reviewed GitHub asset-host allowlist, with no
credential forwarding. A CDN response is still verified against the signed
digest/size. Download completely before quiescing the C6 providing networking.

Signatures do not establish freshness. Persist the highest successfully
accepted `releaseSequence` per trust profile/channel/target in crash-consistent
updater-owned metadata; reject lower sequences and same-sequence different
manifest digests. No automatic install is permitted without that persistence.
The first trusted installation needs a provisioned floor; without it, an older
validly signed release may be replayed. Device anti-rollback remains a separate
gate. A downgrade/recovery procedure must be separately designed, not a hidden
`--force` switch. The current read-only storage/native flash backend supplies
none of this persistence yet.

Key rotation requires an updater already trusting the new key, delivered and
verified using an old trusted key before retiring it. Overlap permits multiple
detached signatures on the same exact manifest. Downloaded keys cannot expand
trust. Compromise/revocation and first trust provisioning require an explicit
recovery/security release procedure; an offline client cannot infer revocation.

## R4 - Acceptance before release/update enablement

Required producer/consumer tests: real key sign/verify interoperability; reject
mutated manifest, payload, length, signature and wrong key; reject duplicates,
oversize/deep JSON, traversal, unknown versions/fields and missing paired assets;
reject board/layout/ABI mismatch and unsigned fallback; prove CLI/MUI share
the verifier and immutable inspected bytes. Test replay-floor crash consistency,
key overlap and missing configuration. Verify draft round-trip and missing-secret
counter-probes without production credentials. Native RV32 verification,
bounded memory/time and real update/reboot/recovery acceptance are separate gates.

Next deliveries: schema/manifest generator and host signature tests; reviewed
native Ed25519/JSON backend and generated trust config; protected release build/
sign/publish workflow; actual installed-device admission and safe update backend.
No schema, signing pipeline, keys or native verifier are implemented by this
contract-only change.

Primary references: [Espressif image format](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/advanced-topics/firmware-image-format.html),
[Ed25519 specification](https://www.rfc-editor.org/rfc/rfc8032),
[GitHub environments](https://docs.github.com/en/actions/reference/workflows-and-actions/deployments-and-environments),
[GitHub secret handling](https://docs.github.com/en/actions/reference/security/secrets).
