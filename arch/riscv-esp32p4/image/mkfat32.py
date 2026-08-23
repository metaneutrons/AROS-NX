#!/usr/bin/env python3
"""
Build a reproducible, read-only-by-intent FAT32 test image for the ESP32-P4
bring-up.  No root privileges and no external tools: everything from the MBR
to the directory entries is written here, which is also what makes the output
byte-reproducible.  mtools is not present on every host, and newfs_msdos
stamps a volume ID from the clock.

Layout is deliberately the same shape as the D1001 test card, so the A2
discovery path sees what it already knows how to read: one MBR partition of
type 0x0b starting at LBA 2048.

Nondeterminism is eliminated rather than documented away.  There is exactly
one field that a normal formatter randomises, the volume serial, and it is
fixed by --serial.  File timestamps are fixed by --timestamp.  Given the same
inputs and the same two options, the output is identical byte for byte.
"""

import argparse
import hashlib
import os
import struct
import sys

SECTOR = 512


def lfn_checksum(short_name: bytes) -> int:
    s = 0
    for b in short_name:
        s = (((s & 1) << 7) | (s >> 1)) + b
        s &= 0xFF
    return s


class Entry:
    """One file or directory to place in the image."""

    def __init__(self, name, is_dir=False, data=b"", children=None):
        self.name = name
        self.is_dir = is_dir
        self.data = data
        self.children = children or []
        self.cluster = 0


class Fat32Image:
    def __init__(self, total_sectors, part_start, sectors_per_cluster,
                 serial, timestamp, label):
        self.total_sectors = total_sectors
        self.part_start = part_start
        self.spc = sectors_per_cluster
        self.serial = serial
        self.timestamp = timestamp
        self.label = label

        self.part_sectors = total_sectors - part_start
        self.reserved = 32
        self.num_fats = 2

        # Solve for the FAT size: every cluster needs four bytes in each FAT.
        # Iterate because the cluster count depends on the FAT size and back.
        fat_sectors = 1
        while True:
            data_sectors = self.part_sectors - self.reserved \
                - self.num_fats * fat_sectors
            clusters = data_sectors // self.spc
            needed = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if needed <= fat_sectors:
                break
            fat_sectors = needed
        self.fat_sectors = fat_sectors
        self.data_start = self.reserved + self.num_fats * self.fat_sectors
        self.clusters = (self.part_sectors - self.data_start) // self.spc

        # Refuse to emit something that calls itself FAT32 and is not.  The
        # threshold is in the specification and hosts enforce it.
        if self.clusters < 65525:
            raise SystemExit(
                "only %d clusters: FAT32 requires at least 65525.  Use a "
                "larger --size-mb or a smaller --cluster-sectors."
                % self.clusters)

        # Cluster 0 and 1 are reserved; 2 is the root directory.
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2
        self.cluster_data = {}

    def alloc_chain(self, nbytes):
        """Allocate a cluster chain for nbytes and return its first cluster."""
        size = self.spc * SECTOR
        n = max(1, (nbytes + size - 1) // size)
        first = self.next_free
        for i in range(n):
            c = self.next_free
            self.next_free += 1
            if self.next_free > self.clusters + 2:
                raise SystemExit("image too small for its contents")
            self.fat[c] = 0x0FFFFFFF if i == n - 1 else c + 1
        return first, n

    def write_chain(self, first, data):
        size = self.spc * SECTOR
        c = first
        off = 0
        while off < len(data) or off == 0:
            chunk = data[off:off + size]
            self.cluster_data[c] = chunk.ljust(size, b"\0")
            off += size
            if off >= len(data):
                break
            c = self.fat[c]
        return

    # ---- directory entries -------------------------------------------------

    def _short_name(self, name, used):
        base, _, ext = name.partition(".")
        base = "".join(ch for ch in base.upper()
                       if ch.isalnum() or ch in "-_")[:8] or "FILE"
        ext = "".join(ch for ch in ext.upper() if ch.isalnum())[:3]
        cand = base.ljust(8)[:8] + ext.ljust(3)[:3]
        if cand in used:
            for n in range(1, 1000):
                tail = "~%d" % n
                cand = (base[:8 - len(tail)] + tail).ljust(8)[:8] \
                    + ext.ljust(3)[:3]
                if cand not in used:
                    break
        used.add(cand)
        return cand.encode("ascii")

    def _needs_lfn(self, name, short):
        base, _, ext = name.partition(".")
        return not (name.upper() == name
                    and len(base) <= 8 and len(ext) <= 3
                    and short.decode("ascii") ==
                    (base.upper().ljust(8) + ext.upper().ljust(3)))

    def dir_entry(self, name, short, attr, cluster, size, used):
        out = b""
        if self._needs_lfn(name, short):
            chk = lfn_checksum(short)
            units = name.encode("utf-16-le")
            units += b"\x00\x00"
            per = 26
            chunks = [units[i:i + per] for i in range(0, len(units), per)]
            for idx in range(len(chunks) - 1, -1, -1):
                part = chunks[idx].ljust(per, b"\xff")
                seq = idx + 1
                if idx == len(chunks) - 1:
                    seq |= 0x40
                out += bytes([seq]) + part[0:10] + b"\x0f\x00" \
                    + bytes([chk]) + part[10:22] + b"\x00\x00" + part[22:26]
        # Built field by field rather than with one struct format string: the
        # FAT32 cluster number is split across offsets 20 and 26, which no
        # single format can express in the right order.
        date, time = self.timestamp
        e = bytearray(32)
        e[0:11] = short
        e[11] = attr
        struct.pack_into("<H", e, 14, time)     # creation time
        struct.pack_into("<H", e, 16, date)     # creation date
        struct.pack_into("<H", e, 18, date)     # last access date
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 22, time)     # write time
        struct.pack_into("<H", e, 24, date)     # write date
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return out + bytes(e)

    def build_dir(self, entries, self_cluster, parent_cluster, is_root):
        """Lay out one directory and recurse.  Returns its raw bytes."""
        used = set()
        raw = b""
        if is_root:
            # The volume label, as a directory entry with the volume-id
            # attribute.  BS_VolLab in the VBR is not enough: it is a legacy
            # copy, and a filesystem that wants the name reads this entry
            # instead.  AROS's FAT handler does exactly that, and without it
            # it named the volume from its serial number (0x00D1505A appeared
            # as `00D1-505A` on the board on 2026-08-23).  Written first, and
            # before any file, because that is where every tool that writes
            # one puts it.
            date, time = self.timestamp
            e = bytearray(32)
            e[0:11] = self.label.ljust(11)[:11].encode("ascii").upper()
            e[11] = 0x08                        # ATTR_VOLUME_ID
            struct.pack_into("<H", e, 14, time)
            struct.pack_into("<H", e, 16, date)
            struct.pack_into("<H", e, 18, date)
            struct.pack_into("<H", e, 22, time)
            struct.pack_into("<H", e, 24, date)
            raw += bytes(e)
            # Reserve the name so a file called AROSP4TEST cannot collide
            # with it in the 8.3 namespace.
            used.add(bytes(e[0:11]))
        else:
            date, time = self.timestamp
            # The specification requires `..` to be zero when the parent is
            # the root directory, not the root's own cluster number.  fsck
            # flags the difference, correctly.
            up = 0 if parent_cluster == 2 else parent_cluster
            for nm, cl in ((b".          ", self_cluster),
                           (b"..         ", up)):
                e = bytearray(32)
                e[0:11] = nm
                e[11] = 0x10
                struct.pack_into("<H", e, 14, time)
                struct.pack_into("<H", e, 16, date)
                struct.pack_into("<H", e, 18, date)
                struct.pack_into("<H", e, 20, (cl >> 16) & 0xFFFF)
                struct.pack_into("<H", e, 22, time)
                struct.pack_into("<H", e, 24, date)
                struct.pack_into("<H", e, 26, cl & 0xFFFF)
                raw += bytes(e)

        for ent in entries:
            short = self._short_name(ent.name, used)
            if ent.is_dir:
                first, _ = self.alloc_chain(self.spc * SECTOR)
                ent.cluster = first
                raw += self.dir_entry(ent.name, short, 0x10, first, 0, used)
            else:
                if len(ent.data) == 0:
                    ent.cluster = 0
                    raw += self.dir_entry(ent.name, short, 0x01, 0, 0, used)
                else:
                    first, _ = self.alloc_chain(len(ent.data))
                    ent.cluster = first
                    self.write_chain(first, ent.data)
                    raw += self.dir_entry(ent.name, short, 0x01, first,
                                          len(ent.data), used)
        return raw

    def place_tree(self, root_entries):
        """Root directory is cluster 2; place it and everything under it."""
        first, _ = self.alloc_chain(self.spc * SECTOR)
        assert first == 2, "root directory must be cluster 2"
        pending = [(root_entries, 2, 0, True)]
        while pending:
            entries, self_cl, parent_cl, is_root = pending.pop(0)
            raw = self.build_dir(entries, self_cl, parent_cl, is_root)
            size = self.spc * SECTOR
            if len(raw) > size:
                raise SystemExit("directory larger than one cluster: "
                                 "raise --cluster-sectors")
            self.cluster_data[self_cl] = raw.ljust(size, b"\0")
            for ent in entries:
                if ent.is_dir:
                    pending.append((ent.children, ent.cluster, self_cl, False))

    # ---- on-disk structures ------------------------------------------------

    def mbr(self):
        b = bytearray(SECTOR)
        off = 446
        b[off] = 0x00                      # not bootable, nothing reads it
        b[off + 4] = 0x0B                  # FAT32 CHS
        struct.pack_into("<I", b, off + 8, self.part_start)
        struct.pack_into("<I", b, off + 12, self.part_sectors)
        b[510] = 0x55
        b[511] = 0xAA
        return bytes(b)

    def vbr(self):
        b = bytearray(SECTOR)
        b[0:3] = b"\xeb\x58\x90"
        b[3:11] = b"AROSP4  "
        struct.pack_into("<H", b, 11, SECTOR)
        b[13] = self.spc
        struct.pack_into("<H", b, 14, self.reserved)
        b[16] = self.num_fats
        struct.pack_into("<H", b, 17, 0)       # root entries, 0 on FAT32
        struct.pack_into("<H", b, 19, 0)       # small total, 0 on FAT32
        b[21] = 0xF8
        struct.pack_into("<H", b, 22, 0)       # FAT16 size, 0 on FAT32
        struct.pack_into("<H", b, 24, 63)
        struct.pack_into("<H", b, 26, 255)
        struct.pack_into("<I", b, 28, self.part_start)
        struct.pack_into("<I", b, 32, self.part_sectors)
        struct.pack_into("<I", b, 36, self.fat_sectors)
        struct.pack_into("<H", b, 40, 0)       # flags
        struct.pack_into("<H", b, 42, 0)       # version
        struct.pack_into("<I", b, 44, 2)       # root cluster
        struct.pack_into("<H", b, 48, 1)       # FSInfo sector
        struct.pack_into("<H", b, 50, 6)       # backup VBR sector
        b[64] = 0x80
        b[66] = 0x29
        struct.pack_into("<I", b, 67, self.serial)
        b[71:82] = self.label.ljust(11)[:11].encode("ascii")
        b[82:90] = b"FAT32   "
        b[510] = 0x55
        b[511] = 0xAA
        return bytes(b)

    def fsinfo(self):
        b = bytearray(SECTOR)
        b[0:4] = b"RRaA"
        b[484:488] = b"rrAa"
        free = self.clusters - (self.next_free - 2)
        struct.pack_into("<I", b, 488, free)
        struct.pack_into("<I", b, 492, self.next_free - 1)
        b[510] = 0x55
        b[511] = 0xAA
        return bytes(b)

    def render(self):
        img = bytearray(self.total_sectors * SECTOR)

        def put(lba, data):
            img[lba * SECTOR:lba * SECTOR + len(data)] = data

        put(0, self.mbr())
        p = self.part_start
        put(p, self.vbr())
        put(p + 1, self.fsinfo())
        put(p + 6, self.vbr())
        put(p + 7, self.fsinfo())

        fat = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", fat, i * 4, v)
        for n in range(self.num_fats):
            put(p + self.reserved + n * self.fat_sectors, bytes(fat))

        for cl, data in self.cluster_data.items():
            lba = p + self.data_start + (cl - 2) * self.spc
            put(lba, data)

        return bytes(img)


def collect(spec_dir):
    """Turn a directory on the host into the tree to place in the image."""
    def walk(path):
        out = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            if os.path.isdir(full):
                out.append(Entry(name, True, children=walk(full)))
            else:
                with open(full, "rb") as f:
                    out.append(Entry(name, False, f.read()))
        return out
    return walk(spec_dir)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--content", required=True,
                    help="host directory whose contents become the image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=64)
    ap.add_argument("--part-start", type=int, default=2048)
    # One sector per cluster by default.  FAT32 is only FAT32 above 65524
    # clusters, and a 64 MiB image cannot reach that with larger clusters:
    # macOS refuses to mount a smaller cluster count, correctly, since the
    # specification says such a volume is FAT16.
    ap.add_argument("--cluster-sectors", type=int, default=1)
    ap.add_argument("--serial", default="0xa5051d00",
                    help="volume serial; fixed so the image is reproducible")
    ap.add_argument("--timestamp", default="2026-01-01",
                    help="date stamped on every entry, for reproducibility")
    ap.add_argument("--label", default="AROSP4TEST")
    args = ap.parse_args()

    y, m, d = (int(x) for x in args.timestamp.split("-"))
    fat_date = ((y - 1980) << 9) | (m << 5) | d
    fat_time = 0

    total = args.size_mb * 1024 * 1024 // SECTOR
    img = Fat32Image(total, args.part_start, args.cluster_sectors,
                     int(args.serial, 0), (fat_date, fat_time), args.label)
    tree = collect(args.content)
    img.place_tree(tree)
    blob = img.render()

    with open(args.out, "wb") as f:
        f.write(blob)

    # Layout and manifest next to the image, so a run can be checked without
    # re-deriving anything.
    base = os.path.splitext(args.out)[0]
    with open(base + ".layout", "w") as f:
        f.write("image            %s\n" % os.path.basename(args.out))
        f.write("total sectors    %d\n" % img.total_sectors)
        f.write("partition start  %d\n" % img.part_start)
        f.write("partition type   0x0b\n")
        f.write("partition sectors %d\n" % img.part_sectors)
        f.write("sectors/cluster  %d\n" % img.spc)
        f.write("reserved sectors %d\n" % img.reserved)
        f.write("fats             %d of %d sectors\n"
                % (img.num_fats, img.fat_sectors))
        f.write("data start       partition sector %d, LBA %d\n"
                % (img.data_start, img.part_start + img.data_start))
        f.write("clusters         %d\n" % img.clusters)
        f.write("clusters used    %d\n" % (img.next_free - 2))
        f.write("volume serial    0x%08x\n" % img.serial)
        f.write("volume label     %s\n" % img.label)

    lines = []

    def manifest(entries, prefix):
        for e in entries:
            path = prefix + "/" + e.name
            if e.is_dir:
                lines.append("dir   %s" % path)
                manifest(e.children, path)
            else:
                lines.append("file  %s  %d  %s"
                             % (path, len(e.data),
                                hashlib.sha256(e.data).hexdigest()))
    manifest(tree, "")
    with open(base + ".manifest", "w") as f:
        f.write("\n".join(lines) + "\n")

    print("image     %s" % args.out)
    print("size      %d bytes" % len(blob))
    print("sha256    %s" % hashlib.sha256(blob).hexdigest())
    print("entries   %d" % len(lines))


if __name__ == "__main__":
    main()
