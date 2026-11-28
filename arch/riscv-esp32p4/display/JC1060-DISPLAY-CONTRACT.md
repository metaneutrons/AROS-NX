# JC1060P470C display contract

The display facts for the Guition JC1060P470C (`P4_BOARD=jc1060p470c-v2`, `-v1`), kept
the way [DISPLAY-CONTRACT.md](DISPLAY-CONTRACT.md) keeps the D1001's: every
entry names its source and an evidence class from that file. Entries marked
`verified` were measured on board `80:f1:b2:d3:3b:a6` (ROADMAP D2).

## Sources

All in the vendor package (`JC1060WP470C/`), as received 2026-10-03.

| Tag | What it is | Licence |
|---|---|---|
| `G-sch` | `5-Schematic/JC1060P470C_I_W_Y-V1.0.pdf` | vendor document |
| `G-idf-old` | `1-Demo/Demo_IDF/ESP-IDF_5.5.4/JC1060P470C_I_W_Y_Old_Panel/common_components/esp32_p4_function_ev_board/` | Apache-2.0, Espressif notice |
| `G-idf-new` | `1-Demo/Demo_IDF/ESP-IDF_5.5.4/JC1060P470C_I_W_Y_New_Panel/common_components/espressif__esp32_p4_function_ev_board/` | Apache-2.0, Espressif notice |
| `G-ard` | `1-Demo/Demo_Arduino/JC1060P470C_I_W_Y_{Old,New}_Panel/lvgl_demo_v8/` | as `G-idf-*` |
| `G-dtsi` | `4-Driver_IC_Data_Sheet/MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_..._2lane.dtsi.txt` | vendor document |
| `G-ds` | `4-Driver_IC_Data_Sheet/JC1060M070N_I.pdf` (panel) | vendor document |

`kernel/jd9165_init.c` carries the two command tables from `G-idf-old` and
`G-idf-new` byte for byte under their original Apache-2.0 notice, as
`jd9365_init.c` does for the D1001.

## Panel and surface

| Item | Value | Class | Source |
|---|---|---|---|
| controller | JD9165BA | `reference` | `G-ds`, `G-dtsi`, demos |
| native size | 1024 x 600, landscape, no rotation | `reference` | `G-ds` p.2; FPC straps UPDN high, SHLR low (`G-sch`) |
| MADCTL | 0x00, sent before the table | `reference` | `G-ard` `esp_lcd_jd9165.c` 205-212 |
| pixel format | RGB565 (COLMOD 0x55, last table entries) | `reference` | `G-idf-*` |
| lanes | 2 (table entry 0x0B 0x11) | `reference` | `G-dtsi` 23, `G-idf-*` |
| lane rate | 750 Mbit/s | `reference` | `G-idf-old` 598, `G-idf-new` 480 |
| PLL | N 4, M 150, range 0x19 against the 20 MHz PLL_F20M reference | `verified` | factory and AROS registers (JTAG); ESP-IDF search; ROADMAP 2026-10-04 |
| DPI clock | 48 MHz real, host timed against 52 MHz nominal, bridge line 1241 | `verified` | factory registers HSA 43, HBP 245, HLINE 2423, bridge total 1241; ROADMAP 2026-10-04 |
| video mode | burst with sync pulses; non-burst displaces the image | `verified` | ROADMAP 2026-10-04 |
| bus clocks | CPU 360, mem/sys 180, APB 90 MHz needed for burst scanout | `verified` | factory registers; ROADMAP 2026-10-04 |
| H sync / back / front | 24 / 136 / 160 (new panel) | `reference` | `G-dtsi` 3-8, `G-idf-new` 505-515, `G-ds` p.8 |
| H back porch, old panel | 160 (IDF) versus 160 with HS 20 (Arduino) | `unresolved` | `G-idf-old` 629-634, `G-ard` old `.h` 110-115 |
| V sync / back / front | 2 / 21 / 12 | `reference` | as H; Arduino old panel says 10 / 23 / 12 (`unresolved` for that batch) |
| init tables | old 51, new 60 commands; board IDs `jc1060p470c-old` and `jc1060p470c` | `reference` | `G-idf-*` |
| fitted batch | new: the factory firmware on board `80:f1:b2:d3:3b:a6` holds the new table at the offsets of vendor image V3.7_New_Panel; V2.x images hold neither long array | `reference` | factory backup 2026-10-03 |
| "V2" label | vendor note says it marks the new panel; that board has no label, so the label is not a reliable test | `unresolved` | burn notes; factory backup |
| reset | GPIO0, active low | `unresolved` | `G-sch` FPC1 pin 4 and the BSP header say GPIO0; `G-ard` says GPIO5, two other demos GPIO27 |
| backlight | GPIO23 to the MP3202 boost enable, 10 k pull-down, active high, PWM | `reference` | `G-sch` "Blacklighting"; LEDC 5-20 kHz in the demos |
| LCD power | none to switch; bias boost enabled from VDDA | `reference` | `G-sch` "LCD_DC-DC" |
| DSI PHY supply | VO3, 2.5 V | `reference` | BSP `display.h` 69-70 |

## Open for J1

J1's test card passed on 2026-10-04 with the settings above. Still open:
whether reset really depends on GPIO0 (the panel was initialised by our
table, but a pulse on a wrong pin would go unnoticed), and the old batch. A CPU reset does not stop the GDMA (D1001 B5 history), so
display runs keep `P4_SCANOUT_SECS` bounded until the path is proven.
