#!/usr/bin/env python3
"""Audit the A5 proof files against what dos.library's ELF loader implements.

Run it on the built proof files before writing them to a card:

    READELF=<build>/bin/darwin-aarch64/tools/crosstools/riscv-aros-readelf \\
        python3 arch/riscv-esp32p4/proof/audit-proof.py <file> [<file>...]

The companion to boot/audit-package.py, and deliberately a separate tool,
because it checks a different loader.  The flash package is placed by the
platform loader in arch/riscv-esp32p4/kernel/kernel_elf.c; these files are
loaded by rom/dos/internalloadseg_elf.c, which has its own relocator.  A
relocation type one of them handles says nothing about the other.

As in the package audit, the accepted set is read out of the loader's source
rather than restated here, so this check cannot drift away from the loader by
being edited separately.

It also prints the ELF header and the per-type relocation counts, which the
A5 phase asks to be recorded in the image manifest.  Exit status is non-zero
if any file would fail to load, so it can gate a build.
"""
import os, re, subprocess, sys

SRC = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
LOADER = os.path.join(SRC, "rom/dos/internalloadseg_elf.c")
READELF = os.environ.get("READELF", "riscv-aros-readelf")

src = open(LOADER).read()
# The RISC-V branch of the relocation switch, plus the four types the loader
# special-cases outside it.
i = src.index("#elif defined(__riscv)")
j = src.index("#else", i)
supported = set(re.findall(r'case\s+(R_RISCV_[A-Z0-9_]+)\s*:', src[i:j]))
supported |= {"R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
              "R_RISCV_GOT_HI20", "R_RISCV_ALIGN"}


def field(header, key):
    m = re.search(r'^\s*%s:\s*(.*)$' % re.escape(key), header, re.M)
    return m.group(1).strip() if m else "?"


def audit(path):
    hdr = subprocess.run([READELF, "-h", path], capture_output=True,
                         text=True).stdout
    rel = subprocess.run([READELF, "-rW", path], capture_output=True,
                         text=True).stdout

    cls, data = field(hdr, "Class"), field(hdr, "Data")
    typ, mach = field(hdr, "Type"), field(hdr, "Machine")
    osabi, flags = field(hdr, "OS/ABI"), field(hdr, "Flags")

    counts = {}
    for t in re.findall(r'\b(R_RISCV_[A-Z0-9_]+)\b', rel):
        counts[t] = counts.get(t, 0) + 1
    unsup = sorted(t for t in counts if t not in supported)

    ok = (cls == "ELF32" and "little endian" in data
          and typ.startswith("REL") and "RISC-V" in mach and not unsup)

    print("%s  %d bytes  %s" % (os.path.basename(path),
                                os.path.getsize(path),
                                "OK" if ok else "FAIL"))
    print("    class %s, data %s, type %s, machine %s"
          % (cls, data.split(",")[0], typ.split()[0], mach))
    print("    OS/ABI %s   flags %s" % (osabi, flags))
    print("    %d relocations in %d types:" % (sum(counts.values()), len(counts)))
    for t in sorted(counts):
        print("      %-24s %5d%s"
              % (t[8:], counts[t], "   UNSUPPORTED" if t in unsup else ""))
    if unsup:
        print("    the DOS loader does not implement: %s"
              % " ".join(t[8:] for t in unsup))
    print()
    return 0 if ok else 1


if len(sys.argv) < 2:
    sys.exit(__doc__)

print("dos.library's loader implements %d relocation types\n" % len(supported))
bad = sum(audit(p) for p in sys.argv[1:])
print("%d file(s) failed" % bad)
sys.exit(1 if bad else 0)
