#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AROS_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BIN_DIR="$AROS_DIR/bin/esp32p4"

mkdir -p "$BIN_DIR"

echo "=== Building AROS for ESP32-P4 inside Colima/Docker Container ==="

docker run --rm -v "$AROS_DIR:/aros" aros-esp32p4-builder:latest bash -c '
set -e
cd /aros/arch/riscv32-esp32p4
OUT="/aros/bin/esp32p4"
FLAGS="-march=rv32imafdc -mabi=ilp32d -mcmodel=medany -O2 -ffreestanding -I/aros/arch/riscv32-esp32p4/include -I/aros/arch/riscv32-esp32p4/boot -c"

echo "[1/4] Compiling Low-Level Kernel & Drivers..."
riscv64-unknown-elf-gcc $FLAGS boot/startup.S -o $OUT/startup.o
riscv64-unknown-elf-gcc $FLAGS boot/uart.c -o $OUT/uart.o
riscv64-unknown-elf-gcc $FLAGS boot/boot.c -o $OUT/boot.o
riscv64-unknown-elf-gcc $FLAGS kernel/platform_p4.c -o $OUT/platform_p4.o
riscv64-unknown-elf-gcc $FLAGS kernel/cpu_switch.S -o $OUT/cpu_switch.o
riscv64-unknown-elf-gcc $FLAGS kernel/traps.S -o $OUT/traps.o
riscv64-unknown-elf-gcc $FLAGS boards/d1001/board_d1001.c -o $OUT/board_d1001.o
riscv64-unknown-elf-gcc $FLAGS hidd/p4gfx/p4gfx_hiddclass.c -o $OUT/p4gfx.o
riscv64-unknown-elf-gcc $FLAGS hidd/p4touch/p4touch_gt911.c -o $OUT/p4touch.o
riscv64-unknown-elf-gcc $FLAGS battclock/battclock_pcf8563.c -o $OUT/pcf8563.o
riscv64-unknown-elf-gcc $FLAGS audio/p4audio_es8311.c -o $OUT/p4audio.o
riscv64-unknown-elf-gcc $FLAGS audio/p4audio_i2s.c -o $OUT/p4audio_i2s.o
riscv64-unknown-elf-gcc $FLAGS camera/p4camera_sc2356.c -o $OUT/p4camera.o
riscv64-unknown-elf-gcc $FLAGS usb/p4usb_dwc2.c -o $OUT/p4usb.o
riscv64-unknown-elf-gcc $FLAGS usb/dwc2_hcd.c -o $OUT/dwc2_hcd.o
riscv64-unknown-elf-gcc $FLAGS sdcard/p4sdcard_init.c -o $OUT/p4sdcard.o
riscv64-unknown-elf-gcc $FLAGS sdcard/sdmmc_host.c -o $OUT/sdmmc_host.o
riscv64-unknown-elf-gcc $FLAGS radio/p4radio_c6.c -o $OUT/p4radio.o
riscv64-unknown-elf-gcc $FLAGS radio/sixlowpan.c -o $OUT/sixlowpan.o
riscv64-unknown-elf-gcc $FLAGS radio/ezsp_ash.c -o $OUT/ezsp_ash.o
riscv64-unknown-elf-gcc $FLAGS radio/spinel.c -o $OUT/spinel.o
riscv64-unknown-elf-gcc $FLAGS ethernet/emac_p4.c -o $OUT/emac_p4.o
riscv64-unknown-elf-gcc $FLAGS ppa/ppa_p4.c -o $OUT/ppa_p4.o

echo "[2/4] Linking AROS Standalone Kernel (ELF)..."
riscv64-unknown-elf-gcc -march=rv32imafdc -mabi=ilp32d -nostdlib -Wl,--gc-sections \
  -T/aros/arch/riscv32-esp32p4/config/linker.ld \
  $OUT/startup.o $OUT/uart.o $OUT/boot.o $OUT/platform_p4.o $OUT/cpu_switch.o $OUT/traps.o \
  $OUT/board_d1001.o $OUT/p4gfx.o $OUT/p4touch.o $OUT/pcf8563.o $OUT/p4audio.o $OUT/p4audio_i2s.o \
  $OUT/p4camera.o $OUT/p4usb.o $OUT/dwc2_hcd.o $OUT/p4sdcard.o $OUT/sdmmc_host.o \
  $OUT/p4radio.o $OUT/sixlowpan.o $OUT/ezsp_ash.o $OUT/spinel.o $OUT/emac_p4.o $OUT/ppa_p4.o \
  -o $OUT/aros_esp32p4.elf

echo "[3/4] Generating Genuine ESP32-P4 Application Image: aros_a.bin..."
esptool.py --chip esp32p4 elf2image --flash-mode dio --flash-freq 80m --flash-size 32MB \
  -o $OUT/aros_a.bin $OUT/aros_esp32p4.elf

echo "[3b/4] Compiling Partition Table: partitions.bin..."
python3 /aros/arch/riscv32-esp32p4/flash/compile_partitions.py \
  /aros/arch/riscv32-esp32p4/flash/partitions.csv \
  $OUT/partitions.bin

echo "[4/4] Creating 32MB Merged Dual-Bank Flash Image..."
python3 /aros/arch/riscv32-esp32p4/flash/merge_image.py \
  $OUT/aros-esp32p4-merged-32mb.bin \
  "" $OUT/partitions.bin "" $OUT/aros_a.bin
'

echo ""
echo "=== Build Complete! ==="
ls -lh "$BIN_DIR/aros-esp32p4-merged-32mb.bin" "$BIN_DIR/aros_a.bin" "$BIN_DIR/partitions.bin"
