# AROS on the Espressif ESP32-P4 (riscv-esp32p4)

Native AROS platform port for the ESP32-P4 SoC. The target name follows the
AROS convention `$(AROS_TARGET_CPU)-$(AROS_TARGET_ARCH)`, so the platform is
configured with

    ./configure --target=riscv-esp32p4

and shares the CPU layer in `arch/riscv-all` with any other 32-bit RISC-V
platform.

## Status

Nothing in the table below is claimed to work until it has been observed
working on hardware. `stub` means the file exists and compiles; `works`
means the behaviour was verified on a board and how it was verified is
recorded in the milestone notes.

| Area | State | Notes |
| :--- | :--- | :--- |
| configure target | done | `--target=riscv-esp32p4` recognised |
| rv32 CPU layer gaps | in progress | M-mode CSR names, cache clears, backtrace done |
| crosstools (riscv-aros gcc) | not started | needs gcc 16.2.0 / binutils 2.47 |
| kernel.resource | not started | |
| exec.library | not started | |
| M-mode trap and CLIC interrupts | not started | |
| SYSTIMER tick | not started | |
| serial debug console | not started | UART0 |
| PSRAM bring-up | not started | prerequisite for anything beyond exec |
| timer.device | not started | |
| SD/MMC block device | not started | |
| MIPI-DSI framebuffer HIDD | not started | |
| touch HIDD | not started | |
| second core | not started | single hart until the rest works |

## Hardware facts this port is built on

Taken from ESP-IDF v6.0 (`components/soc/esp32p4`) unless noted. Anything
marked *unverified* still needs a look at the ESP32-P4 TRM.

| Property | Value | Source |
| :--- | :--- | :--- |
| ISA | RV32IMAFC, `zicsr zifencei zaamo zalrsc` | `components/soc/project_include.cmake` |
| ABI | `ilp32f`, single precision floats only, FLEN=32 | same |
| ISA string accepted | yes | `riscv32-esp-elf-gcc 15.2.0` compiles with it and reports `__riscv_flen 32`, `__riscv_atomic 1`, `__riscv_float_abi_single 1` |
| Privilege modes | M-mode; no S-mode, no Sv32 MMU | *unverified* |
| Interrupt controller | CLIC, not PLIC | `soc_caps.h: SOC_INT_CLIC_SUPPORTED` |
| HP cores | 2 | `soc_caps.h: SOC_CPU_CORES_NUM` |
| FPU | present, `EXT_ILL` CSR errata on FLW/FSW | `soc_caps.h: SOC_CPU_HAS_FPU*` |
| Internal SRAM | `0x4FF00000`-`0x4FFC0000`, 768 KB | `soc.h: SOC_DRAM_LOW/HIGH` |
| PSRAM window | `0x48000000`-`0x4C000000`, 64 MB window | `soc.h: SOC_EXTRAM_LOW/HIGH` |
| LP SRAM | `0x50108000`-`0x50110000`, 32 KB | `soc.h: SOC_RTC_DRAM_LOW/HIGH` |
| Register layout revisions | `hw_ver1` and `hw_ver3` differ | two `register/hw_ver*/soc/reg_base.h` trees |

Two consequences worth stating early. First, the D extension is absent, so
the `ilp32d` default in `configure.in` for 32-bit RISC-V does not hold here
and `struct FpuContext` in `arch/riscv-all/include/aros/cpucontext.h`, which
declares `UQUAD f[32]` for FLEN=64, is wrong for this platform. Second,
768 KB of internal SRAM is not enough to reach a Workbench, so PSRAM
bring-up gates every milestone above the exec layer.

## Relationship to the existing RISC-V code

- `arch/riscv-all` is the shared 32-bit CPU layer and is used as is where
  possible. It currently lacks the S-mode/M-mode-independent pieces that
  `arch/riscv64-all` has (`cli`, `sti`, `issuper`, `backtracefromframe`,
  `cachecleare`, `cacheclearu`, `processor/`), and its `asm/cpu.h` defines
  only the `sstatus`/`sie` names. M-mode equivalents have to be added.
- `arch/riscv64-opensbi` is the reference implementation to port down from.
  It is the only complete and recently maintained RISC-V platform in the
  tree (traps, CLIC's PLIC counterpart, Sv39 MMU, SBI timer, ELF loader,
  lazy FPU switching, per-hart data).
- `arch/riscv64-opensbi` takes a lot of what it knows from the flattened
  device tree the firmware hands it: `kernel_fdt.c` parses it, and
  `arch/riscv64-all/processor/processor_init.c` derives the whole hart
  description from `/cpus`. The ESP32-P4 boots with no device tree at all.
  Supplying a small built-in DTB describing the harts, RAM, UART0, the CLIC
  and the timer would let those paths be reused rather than reimplemented,
  and `arch/riscv-all/linklibs/fdt` is already there to read it. Worth
  deciding before the kernel starts growing platform-specific variants of
  code that already exists.
- `arch/riscv-native` and its `sifive_u` board are 2023 scaffolding that was
  never built: no configure target exists for them, `kernel/intr.c` ends in a
  truncated inline-assembly handler with the call commented out, and
  `cpu_Init()` has an empty body. This port does not build on that code.

## Milestones

Each milestone names what has to be true before it counts as done. No
milestone is marked done on the strength of the code reading correctly.

**M0 - build chain.** `configure --target=riscv-esp32p4` succeeds and
`make crosstools` produces a working `riscv-aros-gcc` for
`rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f`.
Done when: the compiler builds a trivial object for the target ISA.

**M1 - CPU layer.** `arch/riscv-all` gains the missing generic files and the
M-mode CSR names; `arch/riscv-esp32p4` gains its skeleton and link script.
Done when: `make kernel-kernel-esp32p4-riscv` and
`make kernel-exec-esp32p4-riscv` compile and link.

**M2 - first output.** M-mode startup, `mtvec` trap entry, UART0 debug
console, kernel.resource and exec.library reaching `KrnBug()`.
Done when: the board prints an AROS banner over USB-serial.

**M3 - flash image.** Own image at the ROM bootloader offset, or a
documented ESP-IDF bootloader dependency with its licence resolved.
Done when: the image flashes with esptool and reaches M2's banner from a
cold boot.

**M4 - multitasking.** CLIC interrupt handling, SYSTIMER tick, context
switch, lazy FPU state, the idle task, `timer.device`.
Done when: two tasks alternate and a `Delay()` returns after the right time.

**M5 - memory.** PSRAM MSPI bring-up, PSRAM added to the exec memory list.
Done when: `AvailMem(MEMF_ANY)` reports the external RAM and a multi-MB
allocation survives a write-read cycle.

**M6 - storage and package loading.** SDMMC host, block device, the BSP
package loaded and relocated by an ELF loader, resident scan.
Done when: modules outside the kickstart start from flash or MicroSD.

**M7 - display and input.** MIPI-DSI framebuffer HIDD, touch HIDD, Intuition.
Done when: a Workbench screen appears and the pointer follows a touch.

## Verification

There is no emulator. Espressif's QEMU build in `~/.espressif/tools/qemu-riscv32`
supports the machine `esp32c3` only, and upstream QEMU has no ESP32-P4
model. Every milestone from M2 on is verified on a Seeed Studio reTerminal
D1001 over its USB-C serial console.

## Host prerequisites on macOS

Beyond what `configure` finds by default, this host needed `gawk`, netpbm
(for `pngtopnm` and `ppmtoilbm`) and the Python `mako` module. `mako` is
checked for in `configure.in:677` but nothing in this tree imports it, so
it is a configure-time gate only.

## What was taken from the earlier attempt

The `feat/riscv32-esp32p4` branch was a freestanding bare-metal sketch that
defined its own `ULONG`/`APTR`/`struct Resident`, was built by a shell script
of 26 hand-written gcc calls rather than by mmake, and was never registered
in `configure`. Its structure is not reusable. What carries over is the
research: the SRAM and PSRAM addresses, the D1001 peripheral inventory
(GSL3670 touch, PCF8563 RTC, ES8311 audio, SC2356 camera, ESP32-C6 radio),
the dual-bank OTA flash layout, and the 6LoWPAN engine in
`radio/sixlowpan.c`, which is the one part written as real code and which
can be revisited once a SANA-II driver has something to sit on.
