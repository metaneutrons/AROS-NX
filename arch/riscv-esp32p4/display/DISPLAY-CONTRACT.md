# D1001 display contract

The single place this port keeps display facts.  Everything here was read out
of a named source or measured on the D1001 and carries an evidence class; where
sources disagree, both values are recorded and the disagreement is the
finding.  Entries promoted to `verified` cite the matching dated roadmap
evidence.

Anything about the display in `ROADMAP.md` or `README.md` that contradicts
this file is stale and this file wins.

## Evidence classes

| Class | Meaning |
|---|---|
| `reference` | read from a reference implementation that demonstrably drives this panel |
| `derived` | computed from `reference` values, arithmetic shown |
| `hypothesis` | plausible and unproven; needs a hardware measurement to become a fact |
| `unresolved` | the sources disagree and no measurement settles it |
| `verified` | measured on this port, with a dated roadmap evidence entry |

## Sources

| Tag | What it is | Licence |
|---|---|---|
| `V-board` | `Vellum/firmware/components-lcd/d1001_board/` | AGPL-3.0, Fabian Schmieder; relicensed for AROS by the copyright holder 2026-08-23 |
| `V-panel` | `Vellum/firmware/components-lcd/lcd_jd9365/` | same |
| `V-lvgl` | `Vellum/firmware/components/vellum_display/panel_lcd.c` | same |
| `E-jd9365` | `reTerminal-D1001/components/esp_lcd_jd9365_8/` | Apache-2.0, Espressif |
| `S-bsp` | `reTerminal-D1001/components/esp32_p4_re_terminal_d1001/` | Apache-2.0, Seeed |
| `IDF` | `/Volumes/Dev/esp-idf/v6.0/esp-idf`, v6.0 | Apache-2.0, Espressif |

`V-board`, `V-panel` and `V-lvgl` are the decisive references: they are a
product that runs this panel on this board.  `S-bsp` is a vendor BSP whose
timing fields, see below, this board has never actually run.

## Panel and surface

| Item | Value | Class | Source |
|---|---|---|---|
| Controller | JD9365, 8-lane variant driver | `reference` | `E-jd9365` |
| Native resolution | 800 x 1280, portrait scan | `reference` | `V-board:64,65` |
| Pixel format | RGB565, RGB element order | `reference` | `V-panel:49`, `S-bsp` display.h:35 |
| Native framebuffer | 2,048,000 bytes | `derived` | 800 x 1280 x 2 |
| Product mounting | landscape by default | `reference` | `V-lvgl:150` |
| Logical surface | 1280 x 800 landscape, 800 x 1280 portrait | `reference` | `V-lvgl:164,165` |

## DSI bus

| Item | Value | Class | Source |
|---|---|---|---|
| Bus id | 0 | `reference` | `V-panel:28` |
| Data lanes | 2 | `reference` | `V-board:59` |
| Lane bit rate | 1000 Mbit/s per lane | `reference` | `V-board:58` |
| Virtual channel | 0, both DBI and DPI | `reference` | `V-panel:38,46` |
| DBI command width | 8 bit | `reference` | `V-panel:40` |
| DBI parameter width | 8 bit | `reference` | `V-panel:39` |
| PHY supply | LDO channel 3 at 2500 mV | `reference` | `V-board:60,61` |
| Link payload budget | 250 MB/s against 80 MB/s used | `derived` | 2 x 1000 Mbit/s; 40 MHz x 2 B |

The margin in the last row is the reason no lane-rate change is planned.

## Timing, and the disagreement

Two sets exist and they are not close.  Both are recorded because the wrong
one is in the more official-looking place.

| Field | Set A | Set B |
|---|---|---|
| hsync pulse | 20 | 40 |
| hsync back porch | 20 | 140 |
| hsync front porch | 40 | 40 |
| vsync pulse | 4 | 4 |
| vsync back porch | 30 | 16 |
| vsync front porch | 30 | 16 |
| DPI clock | 40 MHz | 40 MHz |
| H total | 880 | 1020 |
| V total | 1344 | 1316 |
| Frame rate | 33.82 Hz | 29.80 Hz |

- Set A is `reference`, from `V-panel:55-60`, the literals inside
  `esp_lcd_dpi_panel_config_t`.
- Set B is `unresolved`, from `V-board:66-71` and identically from `S-bsp`
  display.h:55-60.
- Frame rates are `derived`: 40e6 / (880 x 1344) = 33.82; 40e6 / (1020 x 1316)
  = 29.80.

**Set A is what the panel actually runs.**  `lcd_jd9365_config_t` in
`V-panel/include/lcd_jd9365.h:17,18` declares `hsync, hbp, hfp, vsync, vbp, vfp`;
`V-lvgl:184` fills them from `V-board`, that is Set B; and `V-panel:55-60`
then ignores the whole struct and writes Set A literally into
`esp_lcd_dpi_panel_config_t`.  So Set B has never driven this panel and
changing it in `V-board` changes nothing.  That is a latent defect in the
reference, reported to its author, and it is why this file does not simply
copy the header.

Both `V-board:63` and `S-bsp` display.h call the mode 60 Hz.  Neither set produces it:
60 Hz at Set A's totals would need 70.96 MHz, not 40.  The 60 Hz label is
`unresolved` and must not be used as an expectation.

AROS starts from Set A because it is the set with evidence behind it.  B4
measures VSYNC and either promotes 33.82 Hz to `verified` or replaces it.  A
timing change is a separate, later experiment, not part of bring-up.

## Power, reset and backlight

I2C bus 1 on GPIO21 SCL and GPIO20 SDA, PCA9535 at 7-bit address 0x20
(`ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_000`, A0..A2 to ground).  All
`reference`, `V-board:18,19,22` and `V-board/d1001_board.c:165`.

| Expander bit | Signal | Polarity |
|---|---|---|
| 0 | LCD_PWR_EN | active high |
| 2 | LCD_RST | active low |
| 7 | LCD_BL_EN | active high |
| 8 | PWR_HOLD | high keeps the board alive |
| 6 | BAT_READ_EN | active high |
| 10 | BAT_CHARGE_EN | 0 enables charging |
| 11 | AMP_EN | active high |

Reset sequence, `reference` from `V-panel:81-87`: RST high, 5 ms, RST low,
10 ms, RST high, 120 ms.  Then the JD9365 command sequence over DBI.

Backlight is LEDC PWM on GPIO14, 10-bit resolution at 5 kHz, duty
`1023 * percent / 100`, channel 0 on timer 0, low-speed mode.  `reference`,
`V-board/d1001_board.c:202-224`.  The channel is created with duty 0, so no
light appears until a percentage is set.

**One deliberate deviation from the reference.**  `V-board/d1001_board.c:165`
sets the whole expander to output with `set_dir(0xffff, OUTPUT)` and only then
writes the individual levels.  The PCA9535 output register powers up all-ones,
so that order briefly drives LCD_BL_EN and AMP_EN high before they are
corrected.  This port writes the output latch first and changes direction
afterwards, so nothing is ever driven to a state it was not asked for.  The
reference's order is not a defect in a product that wants the backlight on
anyway; it is one here, where B2 through B4 must keep the panel dark.

## Rotation

The panel scans portrait and the product is mounted landscape, so a rotation
is mandatory rather than a preference.

Direction: **90 degrees clockwise** from the logical landscape surface to the
physical portrait buffer.  `reference`, and two independent paths in the
reference agree, which is why this is no longer the open hypothesis
`ROADMAP.md` B0 recorded:

- `V-lvgl:172` selects `ESP_LV_ADAPTER_ROTATE_270` for landscape mounting,
  counter-clockwise by that API's convention, which is 90 clockwise;
- `V-lvgl/include/lcd_rotation.h` maps the raw draw path with
  `(logical_width - 1 - logical_x) * physical_width + logical_y`, which for
  `physical_width` 800 and `logical_width` 1280 is the same transform.

What remains `hypothesis` is only which physical corner the logical origin
lands in on *this* board, since a 180-degree mounting difference would not
show up in either reference path.  B6's asymmetric pattern settles it.

**The reference does the rotation on the CPU, deliberately.**
`V-lvgl:214` sets `enable_ppa_accel = false` with the reason that IDF 6.0
needs an out-of-tree workaround for rotated `TRIPLE_PARTIAL`, and that its
own updates are small dirty regions where a cache-friendly CPU rotation is
enough.  That reason does not transfer: AROS uses no LVGL adapter, so the
adapter's workaround problem does not apply, but neither does the reference's
evidence.  So:

| Path | Class | Note |
|---|---|---|
| CPU rotation over dirty rectangles | `reference` | what the product runs |
| PPA hardware rotation | `hypothesis` | `SOC_PPA_SUPPORTED` is 1 and `IDF` has `esp_driver_ppa` with `ppa_srm_rotation_angle_t`; unproven on this board |

Bring-up uses the CPU path because it has evidence.  The PPA is a measured
experiment afterwards, not an assumption in the design.  A full-frame CPU
rotation is 4 MB of PSRAM traffic per frame, 135 MB/s at 33.82 Hz, which is
above what B1 aims to establish; that is the number that decides whether the
PPA is needed or merely nice.

## Chip revision and register set

The board is ESP32-P4 revision 1.3.  `IDF` splits the ESP32-P4 register
definitions by hardware version and picks `hw_ver1` when
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, `hw_ver3` otherwise
(`components/soc/CMakeLists.txt:36-40`).  So this port uses **`hw_ver1`**.

The difference matters and is not symmetric:

- `mipi_dsi_host_reg.h` is byte-identical between the two apart from a
  copyright year.  That is the Synopsys DesignWare host core, unchanged.
- `mipi_dsi_bridge_reg.h` is not: 73 lines exist only in `hw_ver3`, adding
  `DSI_BRG_DPI_TYPE`, `DSI_BRG_DPI_DBG_EN`, `DSI_BRG_DSI_BRIG_RST`,
  `DSI_BRG_VSYNC_INT_CLR` and a version-date register.

Those bridge registers must not be touched on this board.  Both statements
are `derived` from a diff of the two header sets and are cheap to re-check.

## Framebuffer and memory

| Item | Value | Class | Note |
|---|---|---|---|
| Native buffer | 2,048,000 bytes | `verified` | one 800 x 1280 RGB565 frame; B5 D1001 evidence 2026-08-25 through 2026-08-27 |
| Reference buffer count | 3 | `reference` | `V-lvgl:224`, triple partial for tear avoidance |
| Reference total | 6,144,000 bytes plus a 2 MB decode buffer | `derived` | `V-lvgl:242` |
| AROS start | exactly one buffer | `verified` | B5; buffer count is still a B6 decision with a measured reason |

Descriptors, ISR data and controller state stay in internal SRAM.  CPU writes
are not coherent with display DMA, so each dirty region is cleaned
CPU-to-memory before presentation.

### Verified native scanout mapping and B6 consequence

B5 hardware on 2026-08-27 fixes the native scanout's constant cyclic X phase
at exactly **525 pixels**.  A raw-source ruler first measured it; the repeated
compensated visual gate then placed full quadrants, four asymmetric corners and
two horizontal plus two vertical one-pixel lines correctly.  This does not make
the test-pattern remapper a framebuffer API.  Moving the DMA base would cross
linear row boundaries, so the correction belongs in B6's logical-to-physical
write transform.

That transform is deliberately fused: for each changed logical pixel, rotate
90 degrees clockwise into the physical portrait coordinates and add the
525-pixel cyclic physical-row phase before storing RGB565.  It therefore costs
no separate phase-correction pass.  AROS must expose a conventional linear
`1280 x 800` logical bitmap; the fixed `800 x 1280` PSRAM buffer remains the
DMA-facing surface.

The bandwidth decision is also now measured rather than assumed.  One native
frame is 2,048,000 bytes and Set A is 33.82 Hz, so the handshake-paced display
DMA can request about 69.3 MB/s; the D1001 repeatedly measured 71 MB/s and held
that rate through the 30-minute concurrent gate.  Such a DMA cannot report the
old 100 MB/s floor because the panel clock itself prevents it from requesting
that much.  The useful negative result is architectural: rotating every pixel
every refresh would itself create about 138.5 MB/s of CPU read-plus-write
traffic before the display's own DMA traffic.  B6 therefore follows the
working reference's dirty-rectangle CPU approach and must not depend on a
full-frame CPU transform at each VSYNC.  Buffer count remains a B6 measurement.

## B3's register sequence, derived

Everything below was derived from `IDF` and is `reference`: it is what the
working driver does, expressed as register writes rather than as calls, with
the values this board's parameters produce already substituted.  Nothing here
has been executed by this port yet.  The point of writing it down is that the
derivation is the hard part and the code is then mechanical; the point of
marking it `reference` is that a value being correctly derived from a working
driver is not the same as a value this port has seen work.

### The PHY supply

`BSP_MIPI_DSI_PHY_PWR_LDO_CHAN` is 3, and `IDF`'s channel-to-unit mapping is
`unit = chan - 1` followed by `index_array[4] = {0, 3, 1, 4}`, so channel 3 is
unit 2 is index 1, which is `PMU_EXT_LDO_P0_0P2A_REG` at PMU base + 0x1c0 with
its analogue half at + 0x1c4.  Fields: `FORCE_TIEH_SEL` bit 7, `XPD` bit 8,
`TIEH_SEL` bits 10:9, `TIEH` bit 14; `MUL` bits 25:23 and `DREF` bits 31:28 in
the analogue register.

The voltage is not a register value but a search.  `Vout = (Vref*K + Vos) *
(1 + 0.25*mul*C)` with `Vref = dref < 9 ? 0.5 + dref*0.05 : 1 + (dref-9)*0.1`,
and K, Vos and C are per-part trim constants in eFuse.  Uncalibrated, K=1,
Vos=0, C=1, the exact solution for 2500 mV is **dref 9, mul 6**, and it is
exact rather than nearest: 1.0 V reference times 2.5.

The port now performs the same integer exhaustive search as ESP-IDF when an
eFuse block version is present, and uses 9/6 only as the old-silicon fallback.
On D1001 MAC `e8:f6:0a:e0:46:4c`, block version 0.3 carries K=4, Vos=32 and
C=39 in their signed encodings; those decode to K=0.979, Vos=-0.003 and
C=0.983 and select **dref 12, mul 4** (about 2.518 V under IDF's model).
`EN_VDET` bit 26 is also set to match `ldo_ll_enable_ripple_suppression()`.
USB-JTAG measured Vellum's live analogue register as `0xc6000000`, and the
AROS implementation now reaches the identical readback without inheriting
PMU state from another firmware.

### Clocks and resets

| Step | Register | Field |
|---|---|---|
| DSI system clock | `HP_SYS_CLKRST` + 0x18 | bit 12 |
| Bridge reset, assert then release | + 0xc0 | bit 26 |
| PHY config clock | + 0x3c | bit 0 |
| PHY PLL reference clock | + 0x3c | bit 1 |
| PHY reference source select | + 0x38 | bits 31:30 |
| DPI clock source | + 0x3c | bits 6:5 |
| DPI clock enable | + 0x3c | bit 7 |
| DPI clock divider | + 0x3c | bits 15:8, value less one |

The PHY reference is the 40 MHz crystal with no division, the same choice and
for the same reason as the I2C bus: it is the one clock no divider this port
sets can move.

### The PHY PLL, with this board's numbers

`f_vco = M/N * f_ref`, M even, and `5 <= f_ref/N <= 40` MHz.  For a 40 MHz
reference and 1000 Mbit/s per lane the search terminates immediately at
**N = 2, M = 50**, which is exact rather than nearest.  The frequency-range
selector for [1000, 1050) Mbit/s is **0x2A**.

The PLL is not memory mapped.  It is written through the DesignWare test
interface, one register at a time, address then value, each latched by a
falling and then a rising edge of the test clock.  The five writes are:

| PHY register | Value | Meaning |
|---|---|---|
| 0x44 | 0x54 | range selector, shifted left one |
| 0x19 | 0x30 | take M and N from 0x17 and 0x18 |
| 0x17 | 0x01 | N - 1 |
| 0x18 | 0x11 | low five bits of M - 1 |
| 0x18 | 0x81 | high bits of M - 1, with the load flag |

0x18 is written twice on purpose; the second write carries bit 7 to commit.

### The order the reference brings it up in

1. LDO on, PHY config and reference clocks on, bridge out of reset.
2. Write the five PLL registers.
3. Wait for PLL lock, then for every data lane to reach stop state.  Both are
   status bits and both must be bounded here, which is the half of B3's gate
   that is about not hanging.
4. Command mode: video mode off, clock lane in low power.
5. `set_switch_time(50, 104, 46, 128)`, the four undocumented constants.
6. Receive CRC and ECC on, end-of-transmission packet on.
7. Timeout and escape clock dividers from the lane rate: byte clock is the lane
   rate over eight, and each divider is that over the target frequency.
8. Timeout counts all zero, which disables the timeout mechanism.
9. `set_max_read_time(6000)` and `set_stop_wait_time(0x3F)`.

Only then does the panel's own command sequence go out over DBI, and only then
is the 120 ms of the reset pulse from B2 actually needed to have elapsed.

## Unresolved, carried forward

1. Which timing set is correct.  Set A runs; whether Set B would also run, or
   run better, is unmeasured.  B4.
2. The 60 Hz label in two sources that no timing set produces.
3. The PHY numbers `set_switch_time(50, 104, 46, 128)`,
   `set_max_read_time(6000)` and `set_stop_wait_time(0x3F)` in `IDF`
   `components/esp_lcd/dsi/esp_lcd_mipi_dsi_bus.c`.  No derivation is available
   in any local source and no ESP32-P4 technical reference manual is present on
   this machine.  They will be carried over as opaque constants with this note
   attached.  So will the PHY register 0x19 value 0x30, whose only
   documentation is a comment saying it makes the PLL use 0x17 and 0x18.
4. Whether the JD9365 answers a DCS `0x04` ID read, and with what.  No source
   states an expected value; inventing one is forbidden.  A stable
   non-degenerate response across resets becomes a board fact.
5. Which physical corner the logical origin occupies.  B6.
