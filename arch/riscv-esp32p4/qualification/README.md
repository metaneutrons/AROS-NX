# JC1060 production qualification

This optional Workbench tool tests the normal production core, not a diagnostic
kernel. It is not yet hardware-qualified. It never writes SD media.

Build with the existing SMP build's includes/libraries:

```sh
LC_ALL=C gmake kernel-production-qualify-esp32p4-riscv-quick \
  kernel-smoke-qualify-esp32p4-riscv-quick P4_BOARD=jc1060p470c-v2
```

After building both tools explicitly, `P4_PRODUCTION_QUALIFY=1` adds them and icons to a newly generated
development volume. Do not silently regenerate a provisioned volume: preserve
its existing manifest entries and verify an exact four-file addition. Core,
BSP, bootloader and SD fixture stay unchanged.

## Procedure

1. Get Fabian's fresh readiness before asking him to start a tool or observe
   the screen. Confirm no host reader owns the serial port during the load.
2. Start `SmokeQualify` from Workbench first. Its default is 30 seconds; the
   separate smoke executable cannot produce `PASS`, even with a duration
   override. Verify visible redraws and responsiveness; do not infer these
   from drawing counters.
3. After the load finishes, retrieve a 45-second passive capture using
   `tools/late-boot-log.py`. Opening the port may reset a board; continuity must
   be verified. Require actual successful process completion and validate the
   capture using `tools/check-production-qualification.py --smoke CAPTURE`.
4. Only after smoke acceptance, get fresh readiness for `ProductionQualify`.
   Its default load is 1,800 seconds. Keep the reader closed throughout that
   interval; retrieve within the following 300-second result window, then
   validate without `--smoke`. A reset, missing result, incomplete coverage,
   failure or aborted run is not a pass. Record the run ID and physical/user
   observations separately from machine results.
5. Confirm final desktop/touch behaviour and document exact artifact hashes,
   duration, counters, capture exit status and procedure in ROADMAP.md.

The result is held only in the current program's RAM and is repeatedly emitted
after workers stop. Each of the nine report records fits the 64-byte endpoint
budget, even with maximum 32-bit values. Close-window aborts the load; after
completion it closes the display window while the result retrieval window
continues. No serial output is intentionally emitted by this tool during load.
Other production components can still emit diagnostics; absence of a host
reader is a procedure gate, not something the tool can prove.

## Coverage and limits

Two affinity-pinned workers verify 16,777,216-step LCG batches against an
independent affine jump-ahead calculation. The main hart issues referenced
64-KiB SD READ64 requests and draws an ordinary Intuition window concurrently.
Each hart, SD and graphics count must reach at least ceil(actual duration/2).
All CPU errors, SD errors, mismatches and reason counters must be zero; the
fixture fingerprint must be verified. A five-second coverage grace has an
absolute EClock deadline. Worker startup/batches/stop and returned SD/drawing
operations have lateness gates. A blocking device or graphics call cannot be
cancelled safely by this user-space tool; a hang/reset loses acceptance rather
than producing a result. On the P4 port, remote `RemTask` has an off-CPU barrier
before freeing worker memory. That barrier deliberately waits without a hard
limit if a task cannot be rescheduled; a stop timeout cannot guarantee a report
under that kernel-level failure.

The fixture header is derived from the unchanged 64-MiB SYS image
`5f123f79d907f82dc46018d0a62934131d3883eb263cba4b736cee24a8eb397a`.
It checks MBR plus three varied low-LBA ranges. This does not test the READ64
4-GiB boundary, all physical SD sectors, raw B5 scanout bandwidth or all upstream
SMP tests. A graphics count proves completed API calls, not visible frames.
This JC1060 gate does not close D1001 acceptance.
