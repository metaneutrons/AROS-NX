# AROS on the Espressif ESP32-P4 (riscv-esp32p4)

Native AROS platform port for the ESP32-P4 SoC. The target name follows the
AROS convention `$(AROS_TARGET_CPU)-$(AROS_TARGET_ARCH)`, so the platform is
configured with

    ./configure --target=esp32p4-riscv

and shares the CPU layer in `arch/riscv-all` with any other 32-bit RISC-V
platform. Note the two orders: an AROS target string is `<arch>-<cpu>`, built
in `configure.in` as `$target_os-$target_cpu`, while the directory it selects
is `<cpu>-<arch>`. Hence `--target=esp32p4-riscv` for `arch/riscv-esp32p4`,
the same way `--target=opensbi-riscv64` selects `arch/riscv64-opensbi`.

## Status

Nothing in the table below is claimed to work until it has been observed
working on hardware. `stub` means the file exists and compiles; `works`
means the behaviour was verified on a board and how it was verified is
recorded in the milestone notes.

| Area | State | Notes |
| :--- | :--- | :--- |
| configure target | done | `--target=esp32p4-riscv`, configure completes |
| crosstools | done | binutils 2.47 and gcc 16.2.0 for riscv-aros, link libraries built |
| rv32 CPU layer | done | M-mode CSR names, FLEN-aware FPU context, cache clears, backtrace, single-precision fenv, ABI-aware stub frames |
| kernel arch layer | compiles | `gmake kernel-kernel-esp32p4-riscv` builds all nine objects |
| exec arch layer | compiles | `gmake kernel-exec-esp32p4-riscv` builds all seven |
| trap entry | compiles | frame offsets asserted against the struct |
| link script | written | test-linked, both SRAM windows asserted |
| bring-up report | works | hart, misa, ids, section extents, then a repeating heartbeat |
| kickstart link | done | 132160 bytes of the 167 KB window, one segment at 0x4FF00000 |
| flashable image | done | `gmake kernel-esp32p4-riscv`, 114640 bytes |
| runs on hardware | **yes** | ROM loads it from 0x2000 and it reports; see below |
| watchdogs | done | timer groups and LP off, super watchdog self-feeding |
| CLIC interrupts | done | a raised line reaches the trap handler |
| SYSTIMER tick | done | 100 Hz sustained, 1201 ticks over 12 heartbeats, 16000000 counts per second |
| context switch | done | tasks are entered in M-mode and their syscalls dispatch |
| preemptive multitasking | done | two equal-priority tasks share the CPU within a few hundred counts of each other over 11 heartbeats |
| memory list | done | 520096 bytes in two regions, every byte of SRAM accounted for |
| kernel.resource | done | initialises, KernelBase built, context size 283 |
| exec.library | runs | SysBase, both InitCode passes, AvailMem and AllocMem answer |
| serial debug console | not started | UART0 |
| PSRAM bring-up | not started | prerequisite for anything beyond exec |
| timer.device | not started | |
| SD/MMC block device | not started | |
| MIPI-DSI framebuffer HIDD | not started | |
| touch HIDD | not started | |
| second core | not started | single hart until the rest works |

## The board this is being brought up on

Read off the hardware with esptool 5.3, not taken from a datasheet.

| | |
| :--- | :--- |
| Chip | ESP32-P4 revision **v1.3**, dual core plus LP core, 400 MHz |
| Crystal | 40 MHz |
| Console | USB-Serial/JTAG, Espressif 303a:1001, enumerates without a bridge chip |
| Flash | 32 MB, Winbond (manufacturer 0xef, device 0x4019) |
| Flash layout | standard ESP-IDF: partition table at 0x8000, nvs 0x9000, nvs_key 0xf000, otadata 0x10000, phy_init 0x12000, ota_0 0x20000 (8 MB), ota_1 0x820000 (8 MB), FAT storage 0x1020000 (15.9 MB) |
| Currently flashed | the owner's own `vellum-d1001` 1.9.0, built with IDF v6.0, entry point 0x4ff009d2, image header declaring min/max chip revision v1.00 to v1.99 |

Revision v1.3 means the pre-v3 memory layout applies. Nothing on this board
has been written to; the readings above reset it into download mode and back
a few times, which restarted the firmware that is on it.

## Silicon revisions

Supporting both revision families costs less than the 190 differing register
headers in ESP-IDF suggest, because the peripheral base addresses are not
among the differences: `register/hw_ver1/soc/reg_base.h` and its hw_ver3
counterpart differ by exactly one added define, `DR_REG_LP_TRNG_BASE`. What
differs is inside peripherals, and only for the ones a driver touches.

What does differ structurally is where the L2 cache is carved out of the
768 KB of internal SRAM. From `components/esp_system/ld/esp32p4/memory.ld.in`:

| | before v3 | v3 and later |
| :--- | :--- | :--- |
| L2 cache | top of SRAM | bottom of SRAM, from 0x4FF00000 |
| Code and data | 0x4FF00000 to 0x4FF2CBD0, 179 KB | 0x4FF00000 + L2 size to 0x4FFAEFC0 |
| Data only | 0x4FF40000, 512 KB minus L2 size | - |
| Second stage and ROM working set | 0x4FF2CBD0 to 0x4FF40000, 77 KB | none |

The L2 cache is 128, 256 or 512 KB, chosen at startup. On this board with
256 KB, which is what the resident firmware selects, that leaves 179 KB of
executable SRAM plus 256 KB of data SRAM; choosing 128 KB instead would give
384 KB of data SRAM, and for a system that is not yet scanning a framebuffer
out of PSRAM that is the better trade.

That last row is not memory the ROM keeps from us, which is what it looked
like from the app linker script alone. Reading the second stage bootloader out
of the flash backup settles it: it loads into three segments spanning
0x4FF29ED0 to 0x4FF354C4, and the ROM's own variables sit at the top of the
window from about 0x4FF3FFC8 up (`rom_spiflash_legacy_data`, `ets_ops_table_ptr`,
`uart_acm_dev` and the rest, in `components/esp_rom/esp32p4/ld/esp32p4.rom.ld`).
The gap between the two is where the ROM loader's stack must live, which is
also why a working second stage stops at 0x4FF354C4 instead of filling
upwards. An image the ROM loads itself has all of it free, because then there
is no second stage.

The RAM inventory can therefore be decided at runtime, and cheaply: exec
takes any number of disjoint memory regions, so the revision is read and the
matching region table is added. The link address cannot be. The overlap of
the executable windows across both families is 0x4FF20000 to 0x4FF2CBD0,
about 51 KB, which will not hold kernel.resource, exec.library and
task.resource together.

So this port builds one image per revision family, which is also how ESP-IDF
treats the question: the image header carries min_chip_rev_full and
max_chip_rev_full, and the firmware on this board declares v1.00 to v1.99.
The v1 numbers are the ones being used, since that is the silicon in hand;
the v3 numbers belong in the same header next to them, marked as untested
until someone runs them.

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

## The reTerminal D1001

There is a complete, working board definition to refer to rather than guess
from: the owner's `vellum-d1001` firmware, at `/Volumes/Dev/Source/Vellum/firmware`,
runs on this exact board. `components-lcd/d1001_board/include/d1001_board.h`
carries the pin map and `components-lcd/lcd_jd9365/` the panel driver.

Worth reading before writing any driver here, because several things the
earlier attempt assumed are wrong:

| | earlier attempt | the working firmware |
| :--- | :--- | :--- |
| Panel | "8 inch 1280x800 MIPI-DSI" | JD9365, natively 800x1280 portrait, 2 lanes at 1000 Mbps, DPI clock 40 MHz, HSYNC 40 / HBP 140 / HFP 40, VSYNC 4 / VBP 16 / VFP 16 |
| DSI PHY | not addressed | needs the internal LDO on channel 3 at 2500 mV |
| Backlight | GPIO 26 | GPIO 14, with LCD_PWR_EN and LCD_BL_EN on an I2C port expander |
| Panel reset | not addressed | port expander bit, not a GPIO |
| Button | GPIO 35 | GPIO 3 |
| Port expander | not addressed | PCA9535, holding power, panel power, panel reset, backlight enable, battery read and charge enable, amplifier enable |
| ESP32-C6 radio | UART at 460800 with a hand-written EZSP/Spinel stack | SDIO (CLK 11, CMD 6, D0-D3 on 7-10), reset on GPIO 13, ESP-Hosted protocol |
| I2C buses | one assumed | I2C0 on SCL 38 / SDA 37, I2C1 on SCL 21 / SDA 20 |
| Console | UART0 | native USB-Serial/JTAG |

Other pins from the same header: battery ADC 18, charge state 15, VSYS power
good 4, USB insertion 17, LEDs on 22 (red), 36 (green) and 23 (blue), touch
interrupt 16.

## Where the image goes

One image, two places it can be written, and no rebuild between them.

    0x2000    the second stage bootloader offset, where the first stage ROM
              loader picks it up directly
    0x20000   the ota_0 app partition, loaded by whichever second stage
              bootloader the device already carries

The first is the one this port is built around. An app partition brings three
mechanisms with it that all assume an ESP-IDF application is sitting there:
the partition table, the OTA data with its state field, and anti-rollback. An
image that is not such an application cannot hold up its end - it has no
`esp_ota_mark_app_valid_cancel_rollback()` to call - so the bootloader does
the right thing for the firmware it was built with and reverts. Observed
exactly that on this board: written to ota_0, the next boot rolled back to the
other slot, and the firmware there repaired ota_0 over the network.

Everything is linked into one run of SRAM from 0x4FF00000, nothing into the
high window at 0x4FF40000, for two reasons. The high window only exists as
RAM while the L2 cache is smaller than 512 KB, and on this silicon the cache
size is set by software that has not run yet when the ROM loader starts an
image - so linking a boot stack there is a bet on a setting we do not yet
control. And the high window and PSRAM are better given to exec as heap at
runtime than spent on link-time sections.

There is a second ceiling, and it is on the image rather than on the
addresses: the first stage ROM loader stops loading a second stage somewhere
between 120544 bytes, which it takes, and 142064 bytes, which it silently
refuses - the flash is written and verified and not one byte comes out. 128 KB
sits in that window and matches how the loader works, mapping flash in 64 KB
pages, but the exact number has not been pinned down. Adding debug.library to
the kickstart was enough to cross it.

The room came from the unwind tables. .eh_frame was 25164 bytes of a 120544
byte image, a fifth of it, and nothing reads it: the backtrace walks the frame
pointer chain (UnwindFrame in arch/riscv-all/exec/alert_cpu.c) and there are no
C++ exceptions in a kickstart. Discarding it in the link script leaves 95376
bytes, and debug.library then fits at 114576 with about 14 KB still to spare.

Which is worth stating as a measurement rather than a rule of thumb, because it
inverts the obvious conclusion. debug.library looked like the thing that did
not fit, and it was not; the tables nobody asked for were twice its weight.

The link script's ceiling is the lower of the two bounds, 0x4FF29ED0, which
costs 12032 bytes of headroom and is what makes one image valid on both
paths. Overflowing it is a linker error naming the script, not an image that
loads over whatever put it there; that assertion has been made to fire on
purpose to check it works.

## Milestones

Each milestone names what has to be true before it counts as done. No
milestone is marked done on the strength of the code reading correctly.

**M0 - build chain.** Done. `configure --target=esp32p4-riscv` succeeds and
`gmake crosstools` produces binutils 2.47 and gcc 16.2.0 for
`rv32imafc_zicsr_zifencei_zaamo_zalrsc/ilp32f`, plus the AROS link
libraries. Four things had to be fixed on the way, none of them in this
platform: two host-side (see above) and two in the shared rv32 layer, where
`fenv.h` rejected the single precision ABI outright and the library stub
frames in `genmodule.h` were hardcoded to ILP32D.

**M1 - CPU layer.** Done. `gmake kernel-kernel-esp32p4-riscv` and
`gmake kernel-exec-esp32p4-riscv` both build. Note they cannot share one
gmake invocation under `-j`: genmf regenerates the same mmakefile from both
and the loser of the race finds the temporary file already renamed.

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

## First boot

Written to 0x2000 and started by the first stage ROM loader:

    ESP-ROM:esp32p4-eco2-20240710
    rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)
    SPI mode:DIO, clock div:1
    load:0x4ff00000,len:0x1b248
    entry 0x4ff00000

    AROS/esp32p4-riscv
    [kernel] hart   0
    [kernel] misa   0x40901125  rv32acfimux
    [kernel] vendor 0x00000612  arch 0x80000003  impl 0x00000001
    [kernel] text   0x4ff00000 - 0x4ff10044  65604 bytes
    [kernel] rodata 0x4ff10044 - 0x4ff1b230  45548 bytes
    [kernel] data   0x4ff1b234 - 0x4ff1b248  20 bytes
    [kernel] bss    0x4ff1b250 - 0x4ff1f680  17456 bytes
    [kernel] sram   0x4ff00000 - 0x4ffc0000
    [kernel] psram  0x48000000 - 0x4c000000  (not brought up)
    [kernel] no memory list, no KernelBase, no exec yet - stopping here.

Three things that were assumptions before and are facts now.

`misa` reads 0x40901125: A, C, F, I, M, U and X, with MXL saying 32-bit. **No
D**, so the single precision ABI is confirmed by the silicon rather than
inferred from ESP-IDF's build files, and the two rv32 fixes it needed were a
precondition rather than a precaution. **No S, but U is there**: there is no
supervisor mode, as assumed, but user mode exists and will be available when
tasks want separating.

The ROM loader accepts an image at the bootloader offset and loads it to
0x4FF00000, so the reasoning about the low window being free while it runs
holds.

And `rst:0x7 (HP_SYS_HP_WDT_RESET)`: the ROM leaves a watchdog armed, so a
kernel that stops in wfi is restarted about once a second. Convenient for now,
because a single banner cannot reliably be caught over USB-Serial/JTAG at all
- the peripheral only takes data once a host has attached and is reading, and
krnP4PutC drops bytes when it has not - but it has to be fed or disabled in
platform_init before anything runs for longer than that.

## Verification

There is no emulator. Espressif's QEMU build in `~/.espressif/tools/qemu-riscv32`
supports the machine `esp32c3` only, and upstream QEMU has no ESP32-P4
model. Every milestone from M2 on is verified on a Seeed Studio reTerminal
D1001 over its USB-C serial console.

## Building

    ./configure --target=esp32p4-riscv --enable-ccache
    gmake crosstools

Out of tree, from a build directory of its own. Three things about this host
are worth writing down, because none of them is guessable:

  - **`gmake`, not `make`.** The system make is GNU Make 3.81, which is old
    enough to matter; Homebrew's is 4.4.1.
  - **`PYTHONPATH` for the mako check.** `configure.in:677` requires the
    Python mako module, though nothing in the tree imports it. It does not
    have to be installed: the copy the Google Cloud SDK vendors satisfies
    the check via
    `PYTHONPATH=/opt/homebrew/share/google-cloud-sdk/lib/third_party`.
    Note that Homebrew's `mako` formula is an unrelated JavaScript bundler.
  - **A worktree of its own.** The build reads SRCDIR live, so anything that
    changes the branch under a running build breaks it. If the main checkout
    is also being worked in, put this branch in `git worktree add`.

## Host prerequisites on macOS

Beyond what `configure` finds by default, this host needed `gawk`, netpbm
(for `pngtopnm` and `ppmtoilbm`) and the Python `mako` module. `mako` is
checked for in `configure.in:677` but nothing in this tree imports it, so
it is a configure-time gate only.

Two things in the build system had to be fixed before it would get as far as
a compiler, both of them upstream problems rather than anything to do with
this target, and both reproducible wherever the host compiler is clang:

  - The host binutils names were built from the compiler's command line
    rather than its name, giving `llvm-argcc`, `llvm-ranlibgcc` and
    `llvm-ldgcc`. configure reported success anyway and `HOST_AR` reached
    `host.cfg` as a bare `cr`.
  - `CXXCPP` was set to the C preprocessor complete with `-std=gnu23`, which
    gmp's `AC_PROG_CXXCPP` sanity check rejects.

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
