# Test image for the ESP32-P4 bring-up

A reproducible FAT32 image with a single MBR partition, built on the host
without root privileges and without external tools.

## Building

    gmake kernel-image-esp32p4-riscv

The target writes four files next to each other in
`bin/esp32p4-riscv/AROS/boot/esp32p4`:

| file | what it is |
| :--- | :--- |
| `aros-test.img` | the image itself |
| `aros-test.layout` | decoded geometry and file-system layout |
| `aros-test.manifest` | every entry with size and SHA-256 |
| `aros-test.sha256` | checksum of the image |

## Verifying

    sh arch/riscv-esp32p4/image/verify-image.sh <path>/aros-test.img

Three checks run, none of them using the generator's own code: the host's MBR
and FAT32 parser has to accept the image, `fsck_msdos` has to report a clean
file system, and the mounted contents have to match the manifest exactly.

The mount happens on a copy, never on the image. macOS writes to a FAT volume
the moment it mounts it, creating `.fseventsd`, which would change the image
and invalidate the before/after hash that board runs depend on. The script
hashes the original before and after to prove it was untouched.

## Reproducibility

Given the same content and the same `--serial` and `--timestamp`, the output
is identical byte for byte. Those two options exist because they are the only
fields a normal formatter randomises: the volume serial, and per-entry
timestamps. Both are fixed by the build target rather than left to the clock.

## Layout choices worth knowing

The partition starts at LBA 2048 with type `0x0b`, the same shape as the D1001
test card, so discovery sees a layout it already reads correctly.

One sector per cluster is the default. FAT32 is only FAT32 above 65524
clusters; a 64 MiB image cannot reach that with larger clusters, and a host
will refuse to mount a volume that calls itself FAT32 with fewer, correctly,
because the specification says such a volume is FAT16. The generator refuses
to emit one rather than producing something a host would reject.

`AROS.boot` carries the CPU marker that `__dos_IsBootable()` looks for with
`strstr`. The build target passes `$(AROS_TARGET_CPU)` so the marker cannot
drift away from the target it is built for.
