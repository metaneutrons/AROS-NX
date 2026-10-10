# Security-summary host checks

Run `run-security-summary-tests.sh` to compile and execute the actual
`common/security_summary.h` helpers at `-O0` and `-O2` with AddressSanitizer
and UndefinedBehaviorSanitizer. Each reported scenario is an independent
summary/configuration input; predicate-check totals are reported separately.

These cases validate the `protected`, `open`, and `match` predicates. They do
not test construction or conversion of driver-reported summaries, or the
supplicant's selection flow; those require separate integration tests.

An RV32 public-header ABI syntax check is optional. Set both `RISCV_CC` and
`RISCV_GEN_INCLUDE` to enable it. If both are unset, the runner explicitly
reports the ABI check as skipped; that is not an ABI pass. Supplying only one
variable is an error. The optional check verifies the public structure sizes
and field offsets at compile time only; it does not execute target code or
claim device capabilities.

Run `run-security-summary-integration-tests.sh` for source-extracted driver
validation/conversion, the actual capability producer, and summary selection
helpers. The runner builds at O0/O2 with ASan/UBSan, including WPS/PMF refusal
branches. Separate cipher roles and swapped-role rejection are covered.
The fixture mirrors consumed structures and supplies Exec/tag fakes; numeric
constants are extracted from production headers and source, with missing or
duplicate definitions treated as errors. It requires the repository's public
headers, not just an isolated WirelessManager copy.
Capability state is already queried: it does not execute the device
capability query, complete selection/association flow, live SANA or hardware.
The separate RV32 ABI fixture remains required for public layout qualification.

Run `run-sana2-association-tests.sh` for source-extracted association, key and
cipher helpers. The fixture records synchronous borrowed options during DoIO;
it tests capability-gated passphrases and fixed 32-byte raw PSKs, passphrase
preference, errors and pointer cleanup. Raw PSK requires both FOURWAY and
RAW_PSK on the actual device. Exec, request structures and device I/O remain
host fakes: a passing model does not prove association, firmware privacy,
target scheduling or link-up.

WPAInfoLength conveys the caller's validated extent for one complete IE,
including its two-byte header. It is sent only for negotiated FOURWAY and
IE_EXTENT; old offload and legacy devices receive no effective additional tag.
Open association conveys a NULL IE with extent zero. Nonzero extents outside
2..257 or inconsistent embedded lengths are rejected before any device I/O.
The caller still guarantees readable memory throughout synchronous DoIO;
length validation cannot establish arbitrary pointer validity. Native devices
must copy borrowed data before acknowledging asynchronous association work.

CONNECT_FAILED is an optional per-device event capability, independent of
firmware handshake offload. The caller negotiates it before initial ONEVENT,
then sends a nonzero non-reused 32-bit AssociationCookie with each attempt.
An asynchronous failure returns that cookie in ONEVENT DataLength, with
io_Error zero and the standalone CONNECT_FAILED WireError bit. Mismatched,
zero, duplicate, unnegotiated or mixed-edge notifications are ignored. Counter
exhaustion is refused before device I/O. This event does not assert an AP
rejection, timeout or former-link loss; providers must scope retention and
supersession to the originating opener before advertising the capability.
The cause-free supplicant path cancels only the auth timer, resets a pending
attempt and schedules a five-second retry unless removed or user-disabled;
it neither emits a DISCONNECTED control event nor changes AP blacklist policy.
Host fixtures remain models, not native provider or radio qualification.

Negotiated CONNECT_FAILED devices also support standalone Disassociate=1.
Both local deauthentication and disassociation use this cancellation tag;
the caller retires its cookie before DoIO, including a refused leave request.
It does not manufacture a DISCONNECT edge or claim confirmed radio teardown.
Legacy devices still return unsupported without receiving new options.
The internal CANCEL_PENDING flag is granted only to negotiated FullMAC
devices. It lets the supplicant send cancellation during authentication or
association, before a confirmed BSSID exists (NULL if even the pending address
is unknown). The leave helper cancels its keepalive timer before I/O; a later
CONNECT without an active cookie is ignored on these devices.
`run-sana2-connect-failed-tests.sh` executes extracted negotiation/correlation
helpers and source-checks initialization/rearm ordering. The separate
`run-sana2-assoc-failed-state-tests.sh` executes extracted pending-state cleanup
and timeout helpers; scan scheduling and device operations remain mocked.

Run `run-sana2-close-tests.sh` for the extracted Amiga signal registry and
SANA-II deinit path at O0/O2 with ASan/UBSan. It exercises callback self/other
unregistration, same-bit handlers with distinct userdata, callback-time
re-registration, invalid/high-bit signals, allocation failure, keepalive
cancellation, request/port teardown order, and event-request freeing. Init
registration/error-cleanup ordering is also source-checked. These host fakes do
not execute Amiga Exec signal delivery, SANA-II I/O, or hardware behavior.

The cancellation state fixture also executes the real supplicant local
disassociate/deauthenticate paths, including confirmed-address precedence,
unknown pending address, legacy/MLME routing and cleanup. The lifecycle
regression passes macOS/Linux O0/O2 ASan/UBSan: association84 scenarios/555
checks, failure23/93, state554, security integration38/76, summary88/264.
The separate RV32 compile checks the new cookie tag, failure capability/event
bits and unchanged v1/32B layout. None of these is a full target or radio pass.
