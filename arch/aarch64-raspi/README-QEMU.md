# QEMU Testing for AROS AArch64

## Working Command

```bash
qemu-system-aarch64 -M raspi4b -m 2G \
  -serial file:/tmp/aros_serial.log \
  -display vnc=0.0.0.0:1 \
  -dtb bcm2711-rpi-4-b.dtb \
  -kernel build-fresh/bin/raspi-aarch64-smp/AROS/aros-aarch64-raspi.img \
  -drive file=aros_sd_full.img,format=raw,if=sd \
  -initrd build-fresh/bin/raspi-aarch64-smp/AROS/aros-aarch64-bsp.rom \
  -device usb-kbd -device usb-mouse \
  -no-reboot -no-shutdown \
  -daemonize
```

Connect via VNC on port 5901.

## Requirements

- **QEMU version**: 10.2.2 (tested). Version 11.0.0 has regressions.
- **Arch Linux packages**: `qemu-system-aarch64`, `qemu-common`, `qemu-hw-display-virtio-gpu`
- **DTB must have padding**: QEMU inserts `linux,initrd-start` and `linux,initrd-end`
  properties into the `/chosen` node at runtime. If the DTB has no slack space,
  QEMU cannot insert these properties and the BSP ROM will not be found.

  To add padding:
  ```bash
  dtc -I dtb -O dtb -p 4096 bcm2711-rpi-4-b.dtb -o bcm2711-rpi-4-b.dtb
  ```

## Known Issues / Pitfalls

### 1. DTB needs slack space (CRITICAL)

QEMU modifies the DTB in-place to add `linux,initrd-start/end`. Without padding
bytes in the DTB file, these properties are silently not added. The bootstrap
then cannot find the BSP ROM → "no COLDSTART residents" → system idle.

**Symptom**: Boot log shows `[BOOT] DTB at ...` but no `[BOOT] initrd-start:` line.

**Fix**: Recompile DTB with `-p 4096` padding (see above).

### 2. 40-bit MMU breaks QEMU

The BCM2712 (RPi5) needs 40-bit VA/PA to reach peripherals at 0x107C000000.
QEMU's raspi4b machine does not support 40-bit physical addresses — enabling
`TCR_T0SZ=24` + `IPS=2` causes immediate exceptions or hangs.

**Fix**: `mmu.c` now checks MIDR_EL1 for Cortex-A76 (RPi5) before enabling
40-bit. On Cortex-A72 (RPi4/QEMU) it stays at 36-bit.

### 3. QEMU raspi4b hardware limitations

QEMU does NOT emulate:
- VideoCore GPU (V3D) — no 3D acceleration
- PCIe Root Complex — no xHCI USB 3.0, no NVMe
- GENET Ethernet
- HDMI output
- RNG200, Thermal, I2C/SPI hardware

QEMU DOES emulate:
- ARM Cortex-A72 (4 cores, SMP)
- GIC-400 (interrupt controller)
- ARM Generic Timer (CNTP)
- PL011 UART
- SD card controller (SDHCI)
- USB DWC2 (OTG, not xHCI)
- VideoCore Mailbox (property tags for framebuffer allocation)

### 4. Framebuffer via Mailbox

The `vc4gfx.hidd` allocates a framebuffer via the VideoCore mailbox
(tag 0x00040001). QEMU supports this — it returns a framebuffer at
~0x3C100000. The `qemu-hw-display-virtio-gpu` package must be installed
for the VNC display to show the framebuffer content.

### 5. Serial output stops after SMP init

The bootstrap prints `[SMP] Core 0xN alive` then hands off to the kernel.
The kernel continues printing on the same UART (PL011 at 0xFE201000).
If you see boot messages stop after SMP, the kernel is running — check VNC.

### 6. `-daemonize` vs background

Use `-daemonize` for QEMU to fork properly. Using `&` with `setsid` can
cause signal propagation issues that kill QEMU unexpectedly.

## Build Directories

| Directory      | Date       | Type    | Status |
|---------------|------------|---------|--------|
| build         | Apr 22     | non-SMP | Boots (no COLDSTART residents in image) |
| build-smp     | Apr 23     | SMP     | Boots with DTB padding |
| build-verify  | Apr 24     | SMP     | Boots fully (pre-40bit MMU) |
| build-test    | May 5      | SMP     | Needs MMU fix (has 40-bit) |
| build-fresh   | May 6      | SMP     | Boots fully (MMU fix applied) |

## Building a QEMU-compatible BSP

The default BSP includes drivers (genet, pci-bcm2711, pcixhci) that access
hardware not emulated by QEMU, causing a bus fault after boot. To build a
BSP without these drivers:

```bash
make kernel-package-raspi-aarch64 \
  PKG_DEVS="input gameport keyboard console sdcard USBHardware/usb2otg" \
  PKG_HIDDS="gfx inputclass mouse keyboard hiddclass vc4gfx" \
  AARCH64_BSP=aros-aarch64-bsp-qemu.rom
```

Then use `aros-aarch64-bsp-qemu.rom` as the `-initrd` in QEMU.

## QEMU 11.0.0 Regression

QEMU 11.0.0 has a regression in the raspi4b machine emulation that causes
an instruction abort during InitCode(RTF_SINGLETASK). The same binary that
boots successfully on QEMU 10.2.2-3 crashes on 11.0.0 with:

```
*** EXCEPTION 0x1 ***
  ESR_EL1: 0x86000004 (EC=0x21 - Instruction Abort)
  ELR_EL1: 0xd61f008058000084 (garbage - PLT opcodes as address)
```

This is confirmed as a QEMU bug: the exact same unmodified binary crashes.
Use QEMU 10.2.2 or real hardware for testing until this is fixed upstream.

Required Arch Linux packages for QEMU 10.2.2-3:
- qemu-common-10.2.2-3
- qemu-system-aarch64-10.2.2-3
- qemu-hw-display-virtio-gpu-10.2.2-3

Note: QEMU 10.2.2-3 requires glib2 <= 2.82.x. If glib2 has been updated
beyond this, QEMU 10.2.2 may also exhibit issues.
