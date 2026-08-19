# AROS Research Operating System — RISC-V 32-bit (ESP32-P4)

Standalone bare-metal AROS architecture port for the **Espressif ESP32-P4 SoC** and the **Seeed Studio reTerminal D1001** intelligent HMI terminal.

---

## 📋 Hardware Architecture Overview

| Component | Specification |
| :--- | :--- |
| **SoC** | Espressif ESP32-P4 |
| **CPU** | Dual-Core RISC-V 32-bit (RV32IMAFDC) @ 400 MHz (ilp32d ABI, medany) |
| **Internal SRAM** | 768 KB L1/L2 SRAM (`0x4FF00000`) mapped to `MEMF_LOCAL` |
| **PSRAM** | 32 MB Octal SPI PSRAM (`0x48000000`) mapped to `MEMF_PUBLIC \| MEMF_FAST` |
| **Display** | 8.0-inch 1280x800 MIPI-DSI IPS LCD Panel (`p4gfx.hidd`) |
| **2D GPU Blitter** | Hardware Pixel Processing Accelerator (`ppa.resource`) with 90°/180°/270° rotation |
| **Touchscreen** | Capacitive Multi-Touch (GSL3670 / GT911 via I2C, `p4touch.hidd`) |
| **Audio DAC** | Everest ES8311 Hi-Fi DAC & ES7210 Mic via I2S DMA double-buffering (`p4audio.device`) |
| **Storage** | SDMMC Host Controller (SDHC/SDXC 4-bit 50 MHz DMA, `esp32p4_sd.device` for `DH0:`) |
| **USB Host** | Synopsys DWC2 High-Speed 480 Mbps USB-OTG Host Controller (`dwc2.hardware`) |
| **Ethernet** | Synopsys DWMAC 10/100 Mbps RMII Ethernet MAC (`emac.device`) |
| **Companion Radio** | ESP32-C6 (UART1 @ 460,800 baud) for Wi-Fi 6, 802.15.4 / 6LoWPAN, Thread, Zigbee 3.0 |
| **Real-Time Clock** | NXP PCF8563T I2C RTC with Amiga epoch leap-year calculation (`battclock.resource`) |
| **Flash** | 32 MB Dual-Bank OTA SPI NOR Flash with NVS Encryption & Rollback Protection |

---

## 💾 32MB Dual-Bank Flash Memory Map

```
0x00000000 ┌────────────────────────────────────────────────────────┐
           │ 2nd-Stage Bootloader (bootloader.bin @ 0x002000)       │
0x00010000 ├────────────────────────────────────────────────────────┤
           │ Partition Table (partitions.bin, 3 KB)                 │
0x00019000 ├────────────────────────────────────────────────────────┤
           │ OTA Data / Bank Selector (otadata, 8 KB with CRC32)    │
0x00020000 ├────────────────────────────────────────────────────────┤
           │ AROS Kernel Slot A (aros_a.bin, 8 MB active partition) │
0x00820000 ├────────────────────────────────────────────────────────┤
           │ AROS Kernel Slot B (aros_b.bin, 8 MB target partition) │
0x01020000 ├────────────────────────────────────────────────────────┤
           │ Base Workbench FAT16 Partition (workbench_flash.bin)   │
           │ (15.875 MB — Bootable out-of-the-box without SD-Card)  │
0x02000000 └────────────────────────────────────────────────────────┘
```

---

## 🏛️ ROM Resident Kickstart AutoInit Modules

All subsystems are registered as standard AmigaOS / AROS `struct Resident` tags with priority-ordered coldstart initialization:

| Priority | Module Name | Type | Description |
| :---: | :--- | :---: | :--- |
| **+85** | `ppa.resource` | `NT_RESOURCE` | 2D Hardware GPU Blitter, Alpha Blend & Screen Rotation |
| **+82** | `gpio.resource` | `NT_RESOURCE` | GPIO Matrix, D1001 Power Button & Display Backlight |
| **+80** | `p4gfx.hidd` | `NT_RESOURCE` | MIPI-DSI 1280x800 Framebuffer & Graphics Subsystem |
| **+70** | `p4touch.hidd` | `NT_RESOURCE` | Capacitive Touchscreen to Intuition Pointer Translator |
| **+60** | `esp32p4_sd.device`| `NT_DEVICE` | SDMMC Block Storage Driver (MicroSD `DH0:`) |
| **+50** | `dwc2.hardware` | `NT_DEVICE` | Synopsys DWC2 USB 2.0 High-Speed Host Controller |
| **+40** | `esp32c6.device` | `NT_DEVICE` | Clean-Room 6LoWPAN (RFC 6282/4944), Spinel & Zigbee 3.0 |
| **+35** | `emac.device` | `NT_DEVICE` | Synopsys DWMAC 10/100 Mbps Ethernet SANA-II Driver |
| **+30** | `p4audio.device` | `NT_DEVICE` | I2S Hi-Fi DAC Audio DMA Driver for AHI Subsystem |

---

## 🔄 Dual-Bank OTA Firmware Upgrade

AROS on ESP32-P4 includes full native support for live over-the-air firmware upgrades directly from within the running system:

### 1. AmigaDOS CLI Command: `C:OTAUpgrade`
```amiga
OTAUpgrade DH0:aros_update.bin REBOOT
```
* **Features:** Template `FILE/A,REBOOT/S,FORCE/S`, ESP32-P4 header magic (`0xE9`) verification, text progress bar, and automatic `otadata` slot activation.

### 2. World-Class Zune/MUI GUI: `System:OTAUpdater`
* **Features:** Native Amiga `PopaslObject` file browser, dynamic firmware validation badge, smooth non-blocking flash loop, live progress gauge, and one-click update with reboot.

---

## 🛠️ Building the Standalone Image

### 1. Lightning-Fast Docker Build (Recommended)
Builds the complete kernel, compiles partition tables, generates the Base Workbench partition, creates the 32MB Flash binary, and generates the 512MB MicroSD image in **~1.8 seconds**:

```bash
bash arch/riscv32-esp32p4/docker/build_image.sh
```

### 2. Standard AROS MetaMake (`mmake`)
```bash
# Build 32MB Flash Image (with Base Workbench):
make esp32p4-image

# Build Full 512MB MicroSD Workbench Image (DH0:):
make esp32p4-sdcard
```

---

## 🔌 Flashing & Booting

### A. Flashing the Onboard 32MB Flash (via USB-C):
Connect the Seeed D1001 / ESP32-P4 board via USB-C to your computer:

```bash
esptool.py --chip esp32p4 --port /dev/cu.usbmodem* --baud 921600 \
  write_flash 0x0 bin/esp32p4/aros-esp32p4-merged-32mb.bin
```

### B. Fast Incremental Kernel Flash (Slot A only):
```bash
esptool.py --chip esp32p4 --port /dev/cu.usbmodem* --baud 921600 \
  write_flash 0x20000 bin/esp32p4/aros_a.bin
```

### C. Writing the MicroSD Card Image (DH0:):
```bash
# On macOS (replace /dev/rdiskX with your target SD card device):
sudo dd if=bin/esp32p4/aros_sd_workbench.img of=/dev/rdiskX bs=1m status=progress
```

### D. Serial Debug Console:
The ESP32-P4 port features **Dual-Logging**: boot messages and kernel diagnostics are simultaneously mirrored to hardware **UART0** (115,200 baud, 8N1) and the native **USB-C CDC-ACM Serial/JTAG** port.

```bash
# Monitor via miniterm:
python3 -m serial.tools.miniterm /dev/cu.usbmodem* 115200 --raw
```

---

## 👤 Author & Copyright

* **Copyright:** (C) 2026, The AROS Development Team. All rights reserved.
* **Author:** Fabian Schmieder ([@metaneutrons](https://github.com/metaneutrons))
* **License:** AROS Public License (APL)
