# Test-card ground truth for the D1001 A1 matrix

The A1 comparison matrix used to check CMD18 against CMD17 and nothing else.
Both go through the same controller, FIFO and cache wrapper, so a fault
common to both read as a pass.  This file is the external reference that
closes that gap, and `krnP4SDCardCompareBlocks()` in
`../kernel/kernel_startup.c` carries the same numbers as compiled-in tables.

## Capture

Card: the D1001 test card in a host card reader, `/dev/disk21`,
127,865,454,592 bytes, exactly 249,737,216 512-byte units.  That unit count
matches the geometry the AROS boot log reports, which is how the card was
identified.

Scheme: MBR (`FDisk_partition_scheme`), one `DOS_FAT_32` partition
`AMIGATAUSCH` at partition offset 2048 sectors.  The MBR's single entry is
type `0x0b`, start 2048, 249,735,168 sectors.  LBA 0 carries `0x55 0xaa` at
offsets 510/511.

Procedure: `diskutil unmountDisk`, then `dd if=/dev/rdisk21 bs=512
count=4096`, read-only.  The first 2 MiB have SHA-256
`3aeaff747c68f8c087cda1e3a9fcd877e323bb7ad238f9462efd5d9322fa6e45`.  Nothing
was written to the card.

Date: 2026-08-22.

## Hashes

FNV-1a-32, identical to the hash the harness computes
(`hash ^= byte; hash *= 16777619`).

Every sector in LBA 0..4095 that is not listed here is zero-filled, 4086 of
4096.  A misdirected read therefore looks like zeroes rather than like an
error, which is why the old matrix could not see one.

| LBA  | first word  | sector hash  | note |
|------|-------------|--------------|------|
| 0    | 0x00000000  | 0xdebe99c1   | MBR |
| 2048 | 0x429058eb  | 0x730d1cbd   | FAT32 VBR, OEM "BSD  4.4" |
| 2049 | 0x41615252  | 0x1647bc76   | FSInfo, volatile |
| 2054 | 0x429058eb  | 0x730d1cbd   | backup VBR, byte-identical to 2048 |
| 2055 | 0x41615252  | 0xc7ef8842   | backup FSInfo |
| 2080 | 0x0ffffff8  | 0x024ab7b6   | FAT 1, volatile |
| 2081 | 0x00000081  | 0xda4505e0   | FAT 1, volatile |
| 2083 | 0x00000000  | 0x9a2ba598   | FAT 1 |
| 2084 | 0x00000201  | 0xcf015645   | FAT 1 |
| 2085 | 0x00000281  | 0x05ba934e   | FAT 1 |

Ranges, hashed over the whole request rather than per sector:

| start LBA | 1 sector   | 2 sectors  | 32 sectors | 128 sectors |
|-----------|------------|------------|------------|-------------|
| 0         | 0xdebe99c1 | 0x32fbe1c1 | 0xba6a51c1 | 0x6d6551c1 |
| 2047      | 0x4d7705c5 | 0x44a784bd | 0x641fe591 | 0xd7d95735 |
| 2048      | 0x730d1cbd | 0x3180955e | 0xd1854591 | 0x8690d735 |
| 2049      | 0x1647bc76 | 0xcb52ec76 | 0x3e1ddd9a | 0xa0c2e66d |
| 2083      | 0x9a2ba598 | 0xdbf2f318 | 0xadc117e7 | 0xe07e17e7 |

Constants for reading a log, FNV-1a-32 over N identical bytes:

| payload | 512 bytes  | 1024 bytes | 16 KiB     | 64 KiB     |
|---------|------------|------------|------------|------------|
| 0x00    | 0x4d7705c5 | 0x1f116dc5 | 0x38699dc5 | 0x5e509dc5 |
| 0xa5    | 0x52c707c5 | -          | -          | -          |

## Two properties that limit what a test can prove

LBA 2054 is byte-identical to LBA 2048.  It is the FAT32 backup boot sector.
A repeat of a first block holding 2048 and a misdirected read of 2054 cannot
be told apart by content, so no test may treat a 2054-shaped result as proof
of a repeat.

LBA 2049, 2080 and 2081 are volatile.  A host that mounts the volume rewrites
FSInfo and the FAT; those three hashes changed between a mounted and an
unmounted read of the same card.  Do not mount this card on a host before a
reference run, or recapture this file afterwards.

## Volatility, observed rather than predicted

The warning above came true on 2026-08-22.  Between two board runs the card
left the socket, the board reported `GPIO45 high: no card present` twice, and
when it came back LBA 2049 held `0x758fc4ce` instead of `0x1647bc76`.  Every
matrix cell whose range contained 2049 failed and every other cell matched,
with CMD17 and CMD18 agreeing on the new value, so the driver was not at
fault; the reference was.

The harness therefore no longer relies on any range containing 2049, 2080 or
2081, and those three sectors are absent from its naming table so a block is
reported as unknown rather than mislabelled.  On this card only LBA 0 and 2083
have stable 1, 2, 32 and 128-sector ranges.  The straddle cases use 2047,
2048, 2053 and 2054, which stay below the FSInfo and FAT sectors.

If this file is recaptured, the capture must be the last thing that touches
the card before the run that uses it.

## The card end, and why the 32-bit boundary replaced it

The last 128 sectors were captured on 2026-08-22 and are entirely zero-filled,
a single distinct sector hash across all of them.  Their references are the
zero-payload constants above.  Those cells can therefore be checked against
the card, but they cannot detect a repeated or misplaced block, and the
harness prints that warning separately.

What the card-end cells were meant to prove is a byte offset beyond what a
32-bit ULONG can express.  LBA 8388608 is exactly that boundary, the first
sector a 32-bit byte offset cannot reach, and it holds file data:

| start LBA | 1 sector   | 2 sectors  | 32 sectors | 128 sectors |
|-----------|------------|------------|------------|-------------|
| 8388608   | 0x55a37550 | 0xeba3bb4f | 0x3fc8a95e | 0x894be777 |
| 10000000  | 0x73bf86bf | 0xe462007b | 0x02ddf4f0 | 0xa3d171be |

Both ranges have 128 distinct sectors, so unlike the card end they can detect
a repeat.  File contents are much more stable than the FAT metadata above, but
they are not eternal: if the files on this card change, recapture.

Nothing was written to the card to achieve this.  For the record, writing was
assessed and would have been lossless: only four sectors at the very end lie
past the last addressable cluster, and the clusters behind the last 128
sectors (3901159 to 3901161) read as free in the FAT.  It was unnecessary.
