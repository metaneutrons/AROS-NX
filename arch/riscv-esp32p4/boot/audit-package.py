#!/usr/bin/env python3
"""Audit every member of an AROS PKG v1 container against what the ESP32-P4
platform ELF loader actually implements.

Run it after any change to the package contents:

    READELF=<build>/bin/darwin-aarch64/tools/crosstools/riscv-aros-readelf \\
        python3 arch/riscv-esp32p4/boot/audit-package.py <path to aros-bsp.pkg>

Every member has to be a little endian ELF32 relocatable RISC-V object, and
every relocation type it carries has to be one the loader implements.  The set
of accepted types is read out of kernel_elf.c itself rather than restated here,
so this check cannot drift away from the loader by being edited separately.

The exit status is non-zero if any member fails, so it can gate a build."""
import os, re, struct, subprocess, sys, tempfile

SRC = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ELF = os.path.join(SRC, "arch/riscv-esp32p4/kernel/kernel_elf.c")
READELF = os.environ.get("READELF", "riscv-aros-readelf")

src = open(ELF).read()
# every R_RISCV_* named in a case label of the relocation switch, plus the
# four the loader special-cases outside it
supported = set(re.findall(r'case\s+(R_RISCV_[A-Z0-9_]+)\s*:', src))
supported |= {"R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
              "R_RISCV_GOT_HI20", "R_RISCV_ALIGN"}

def members(path):
    """PKG v1, tools/package/FORMAT: 'PKG', version 1, package size, then per
       entry a path length, the path with its NUL, a data length and the data.
       All longs big endian, as tools/package/pkg writes them."""
    d = open(path, 'rb').read()
    assert d[:3] == b'PKG' and d[3] == 1, d[:4]
    declared = struct.unpack('>I', d[4:8])[0]
    off, out = 8, []
    while off < len(d):
        plen = struct.unpack('>I', d[off:off+4])[0]
        off += 4
        name = d[off:off+plen].decode()
        off += plen + 1                       # path plus its NUL
        dlen = struct.unpack('>I', d[off:off+4])[0]
        off += 4
        out.append((name, dlen, off))
        off += dlen
    return d, out, declared

def audit(path):
    d, ms, declared = members(path)
    print("package %s  %d bytes on disk, %d declared, %d members\n"
          % (os.path.basename(path), len(d), declared, len(ms)))
    bad = 0
    for name, size, off in ms:
        tmp = os.path.join(tempfile.gettempdir(), "_pkgmember.o")
        open(tmp, 'wb').write(d[off:off+size])
        hdr = subprocess.run([READELF, "-h", tmp], capture_output=True, text=True).stdout
        rel = subprocess.run([READELF, "-rW", tmp], capture_output=True, text=True).stdout
        def field(k):
            m = re.search(r'^\s*%s:\s*(.*)$' % re.escape(k), hdr, re.M)
            return m.group(1).strip() if m else "?"
        cls, data, typ, mach = field("Class"), field("Data"), field("Type"), field("Machine")
        flags = field("Flags")
        used = sorted(set(re.findall(r'\b(R_RISCV_[A-Z0-9_]+)\b', rel)))
        unsup = [t for t in used if t not in supported]
        ok = (cls == "ELF32" and "little endian" in data
              and typ.startswith("REL") and "RISC-V" in mach and not unsup)
        print("  %-24s %8d  %s %s  %s" % (name, size, cls, typ.split()[0],
                                          "OK" if ok else "FAIL"))
        print("      machine %s   flags %s" % (mach, flags))
        print("      %d reloc types: %s" % (len(used), " ".join(t[8:] for t in used)))
        if unsup:
            print("      UNSUPPORTED BY LOADER: %s" % " ".join(unsup))
        if not ok:
            bad += 1
        os.unlink(tmp)
    print("\n%d member(s) failed" % bad)
    return bad

sys.exit(1 if audit(sys.argv[1]) else 0)
