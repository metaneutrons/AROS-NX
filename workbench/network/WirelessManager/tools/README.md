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
