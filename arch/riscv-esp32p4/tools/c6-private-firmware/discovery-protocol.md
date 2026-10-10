# Private C6 discovery protocol, version1

This is an undeployed extension, not an installed capability. The schema patch
targets pinned Hosted d5184217f710d54c01f932e75b4c55e3f4933fc9. Apply it only to
a separate candidate source tree and regenerate its protobuf-c sources before
building. It adds request350/response606, moving the respective sentinels to
351/607. Existing message IDs remain unchanged. Native RPC class ranges and
normal control allowlists remain unchanged until explicitly integrated.

Request nested fields are `uint32 version=1`, `uint32 nonce=2`. Both are
mandatory semantically: version must1 and nonce must nonzero. Response fields
are `int32 resp=1`, `bytes snapshot=2`. Success requires resp0 and exactly one
valid inventory. An error response must contain no snapshot. Handlers must
bound input before unpacking and output before transport publication.

The startup advertisement must explicitly identify this extension and
its version before a query is permitted. A stock peer receives no speculative
request. Advertisement, schema generation and a serialized handler are included
in the separate C6 candidate recipe. Native owner negotiation/binding remains
open; the schema patch or inventory codec alone does not provide these features.

## Canonical inventory payload

Shared `boardtool/discovery_wire.c/.h` implements the payload. All integers are
little-endian. Exact length is32+32*count, count1..32; maximum1056 bytes. It is a
single owned snapshot, not pages, a native C struct or a raw partition table.

| Byte offset | Field |
| --- | --- |
| 0..3 | ASCII C6DI |
| 4 | version1 |
| 5 | header length32 |
| 6 | record length32 |
| 7 | partition count |
| 8,9,10 | running, boot, update sorted indices;255 unknown |
| 11 | running state0unknown,1new,2pending,3valid,4invalid,5aborted,6undefined |
| 12,13 | security known/enabled bitmasks |
| 14..15 | zero reserved |
| 16..19 | detected physical capacity |
| 20..23 | usable image-header/driver limit |
| 24..27 | echoed nonzero request nonce |
| 28..31 | zero reserved |

Each32-byte record contains offset at0, size at4, normalized flags at8,
type/subtype at12/13, label length at14, reserved zero at15 and16 label bytes
at16. Length1..16; printable ASCII without embedded NUL; unused label bytes
must zero. Entries are sorted, nonoverlapping and structurally validated by the
same validator as the collector. Encryption flag1 and readonly flag2 are
normalized IDF metadata, not all original on-flash flags. Security known bits
are encryption1, secure boot2, anti-rollback4, rollback8; enabled is a subset.
Unknown is not disabled. A running/update alias is valid display information,
never a permissible write target.

Encoders reject invalid inventories, zero nonce, insufficient capacity and
overlapping input/output/written ranges before changing output. Decoders reject
noncanonical or invalid records, wrong nonce, surplus/truncated data and input/
output overlap without publishing partial output.

## Correlation and qualification still required

The nonce detects mismatched requests; it does not authenticate the C6 or prove
freshness across reset. The owner must also verify response ID, non-reused UID,
live peer/owner epoch, negotiated capability and exclusive maintenance lease.
The dispatcher must exclude OTA/slot selection and all partition registry
mutation during collection. No inventory authorizes writing or proves recovery.

1056 bytes leaves room within stock1536-byte SDIO buffers for the nested and
outer Protobuf envelopes, serial endpoint framing and transport header, but the
generated-message packer and native host frame encoders now pass a maximum-size
roundtrip (1098 bytes including both wrappers). Actual IDF serial/framing and
live transport remain unqualified. Request unpacking is bounded by the existing
4096-byte serial reassembly capacity, not an individual transport frame.
Source/object tests do not prove live transport,
stack budget, installed-table provenance or initial-install recovery.
