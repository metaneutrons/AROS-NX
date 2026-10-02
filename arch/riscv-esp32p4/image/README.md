# Test image for the ESP32-P4 bring-up

A reproducible FAT32 image with a single MBR partition, built on the host
without root privileges and without external tools.

## Building

    gmake kernel-image-esp32p4-riscv

For the normal C3 Wanderer image, select the explicit graphical-boot profile:

    gmake kernel-image-esp32p4-riscv P4_C3_GRAPHICAL_BOOT=1

That profile replaces the minimal proof Startup-Sequence with the read-only
Wanderer sequence and stages its runtime libraries, preferences and Zune
classes.  The unqualified target retains the smaller A3-A5 proof content, as
does the 4-MB flash development-volume workflow.

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

## Getting an untouched after-image

The host writes to a FAT volume as soon as it mounts one. On a board run that
means the sector hash before and after will differ even if the board wrote
nothing, so a bare hash comparison cannot answer the question the gate asks.

What worked, and what the roadmap entry of 2026-08-23 records, is to attribute
every changed sector instead. After a run the difference was eight sectors:
FSInfo, both FATs, the root directory and four clusters, with exactly one new
directory entry, `FSEVE~12`, which is `.fseventsd`. The MBR, the VBR and every
cluster holding generated content were unchanged. That is stronger evidence
than an equal hash would have been, because it says where any difference came
from.

If a future run needs a literally equal hash, automount has to be suppressed
before the host can write.  Merely issuing `diskutil eject` immediately after
`dd` is not sufficient: the 2026-08-29 C3 retry reproduced `.fseventsd` in the
interval after `dd` closed the raw device and before the eject completed.

The macOS-only helper keeps the raw descriptor exclusively open through the
write, cache synchronization and `DKIOCEJECT`, so Disk Arbitration never sees
an online rewritten FAT volume:

The helper also reads back and compares the complete image prefix after cache
synchronization, while still holding that exclusive descriptor. It fails on a
short read or byte mismatch before reporting successful verification/eject.
Back up the complete write range before invoking it; bytes beyond the image
prefix are not written. A prefix backup is not a whole-medium backup.

    cc -std=c11 -Wall -Wextra -Werror -O2 \
        arch/riscv-esp32p4/tools/write-image-eject.c \
        -o /tmp/aros-write-image-eject
    diskutil unmountDisk /dev/diskN
    sudo /tmp/aros-write-image-eject \
        <path>/aros-test.img /dev/rdiskN EXPECTED_DEVICE_BYTES

Identify the exact removable medium independently before this destructive
command.  The required capacity is checked through the raw-device ioctls so a
stale `/dev/rdiskN` name fails closed.  After the helper ejects the unlocked
card, engage the adapter's LOCK switch before the baseline reinsertion and
verify that macOS reports `Media Read-Only: Yes` before reading it.  Eject that
locked baseline, use the MicroSD in the board, then return it in an already
locked adapter for the first post-run host insertion.


## The volume label

FAT keeps the volume name in two places and they are not equivalent.
`BS_VolLab` in the boot sector is a legacy copy; the name a filesystem
actually reads is a directory entry in the root with the `ATTR_VOLUME_ID`
attribute (0x08).  `mkfat32.py` originally wrote only the first, and the
consequence showed up on the board during A4: AROS's FAT handler searched the
root directory, found no such entry and named the volume from its serial
number, so `00D1505A` appeared as `00D1-505A`.  The generator now writes the
directory entry as well, as the first entry in the root, and the name is
reserved in the 8.3 namespace so a file could not collide with it.  macOS
reads the volume as `AROSP4TEST` and `fsck_msdos` is clean.

Note that the card written before this change carries an image without the
entry.  Rewriting it is a separate, deliberate act; nothing about booting
depends on the name.

## Layout choices worth knowing

The partition starts at LBA 2048 with type `0x0b`, the same shape as the D1001
test card, so discovery sees a layout it already reads correctly.

One sector per cluster is the default. FAT32 is only FAT32 above 65524
clusters; a 64 MiB image cannot reach that with larger clusters, and a host
will refuse to mount a volume that calls itself FAT32 with fewer, correctly,
because the specification says such a volume is FAT16. The generator refuses
to emit one rather than producing something a host would reject.

Directories are ordinary FAT cluster chains, not restricted to one cluster.
This matters for the C3 `Classes/Zune` directory: its long-name entries exceed
512 bytes even though the complete volume is still small enough for one-sector
clusters.  The generator allocates the first directory cluster from its parent
and extends that chain after the complete directory-entry size is known.

`AROS.boot` carries the CPU marker that `__dos_IsBootable()` looks for with
`strstr`. The build target passes `$(AROS_TARGET_CPU)` so the marker cannot
drift away from the target it is built for.
