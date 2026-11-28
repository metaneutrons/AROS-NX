#!/usr/bin/env python3
"""Audit AROS PKG v1 members against the ESP32-P4 ELF loader.

Run after changing package contents:

    python3 arch/riscv-esp32p4/boot/audit-package.py <path-to-aros-bsp.pkg>

Only relocations whose target section the loader places are audited.  The
loader drops allocated, non-empty ``.eh_frame`` sections and skips relocation
tables for every section that it does not place.  Relocations in those tables
cannot make the runtime loader fail and are deliberately ignored here.
"""

import os
import re
import struct
import sys


SRC = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
LOADER_C = os.path.join(SRC, "arch/riscv-esp32p4/kernel/kernel_elf.c")
ELF_HEADER_H = os.path.join(SRC, "compiler/include/dos/elf.h")

ELF32_HEADER = "<16sHHIIIIIHHHHHH"
ELF32_SECTION = "<IIIIIIIIII"
ELF32_SYMBOL = "<IIIBBH"
ELF32_RELA = "<IIi"

ET_REL = 1
EM_RISCV = 243
ELFCLASS32 = 1
ELFDATA2LSB = 1
EV_CURRENT = 1

SHT_NULL = 0
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHF_ALLOC = 0x2

SHN_UNDEF = 0
SHN_LORESERVE = 0xFF00
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2
SHN_XINDEX = 0xFFFF

R_RISCV_NONE = 0
R_RISCV_RELAX = 51
R_RISCV_PCREL_LO12_I = 24
R_RISCV_PCREL_LO12_S = 25


class AuditError(ValueError):
    """An ELF or PKG member is malformed or cannot be loaded by the P4 loader."""


def _range_ok(total, offset, size):
    return offset <= total and size <= total - offset


def _unpack(fmt, data, offset, description):
    size = struct.calcsize(fmt)
    if not _range_ok(len(data), offset, size):
        raise AuditError("truncated %s" % description)
    try:
        return struct.unpack_from(fmt, data, offset)
    except struct.error as exc:
        raise AuditError("invalid %s: %s" % (description, exc)) from exc


def _cstring(table, offset, description):
    if offset >= len(table):
        raise AuditError("%s offset is outside its string table" % description)
    end = table.find(b"\0", offset)
    if end < 0:
        raise AuditError("unterminated %s" % description)
    return table[offset:end].decode("utf-8", errors="replace")


def members(path):
    """Return package bytes, (name, data-size, data-offset) entries, and size.

    PKG v1 stores big-endian lengths.  The kernel only scans through the
    declared package size, so trailing bytes after that boundary are ignored.
    """
    with open(path, "rb") as package_file:
        data = package_file.read()
    if len(data) < 8 or data[:3] != b"PKG" or data[3] != 1:
        raise AuditError("not an AROS PKG v1 package")
    declared = struct.unpack_from(">I", data, 4)[0]
    if declared < 8 or declared > len(data):
        raise AuditError("declared package size is outside the file")

    offset = 8
    entries = []
    while offset < declared:
        if not _range_ok(declared, offset, 4):
            raise AuditError("truncated package member name length")
        name_length = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if name_length == 0xFFFFFFFF or not _range_ok(declared, offset, name_length + 1):
            raise AuditError("package member name is outside the declared package")
        name_bytes = data[offset:offset + name_length]
        if data[offset + name_length] != 0:
            raise AuditError("package member name is not NUL terminated")
        # Package paths are NUL-terminated strings. Some package producers
        # leave the remainder of a fixed-size path field zero-padded.
        name = name_bytes.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        offset += name_length + 1

        if not _range_ok(declared, offset, 4):
            raise AuditError("truncated package member data length")
        data_size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if not _range_ok(declared, offset, data_size):
            raise AuditError("package member data is outside the declared package")
        entries.append((name, data_size, offset))
        offset += data_size

    return data, entries, declared


def _read_supported_relocations():
    with open(LOADER_C, "r", encoding="utf-8") as source_file:
        loader_source = source_file.read()
    with open(ELF_HEADER_H, "r", encoding="utf-8") as header_file:
        elf_header = header_file.read()

    names = set(re.findall(r"case\s+(R_RISCV_[A-Z0-9_]+)\s*:", loader_source))
    names.update({"R_RISCV_PCREL_LO12_I", "R_RISCV_PCREL_LO12_S",
                  "R_RISCV_GOT_HI20", "R_RISCV_ALIGN"})
    values = {
        name: int(value, 0)
        for name, value in re.findall(
            r"^\s*#define\s+(R_RISCV_[A-Z0-9_]+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)",
            elf_header,
            re.M,
        )
    }
    missing = sorted(names - values.keys())
    if missing:
        raise AuditError("loader relocation constants missing from ELF header: %s"
                         % " ".join(missing))
    return {values[name]: name for name in names}


def _parse_elf(data):
    if len(data) < struct.calcsize(ELF32_HEADER):
        raise AuditError("truncated ELF header")

    (ident, elf_type, machine, version, _entry, _phoff, section_offset,
     _flags, elf_header_size, _phentsize, _phnum, section_entry_size,
     section_count, names_index) = _unpack(ELF32_HEADER, data, 0, "ELF header")

    if ident[:4] != b"\x7fELF":
        raise AuditError("not an ELF file")
    if ident[4] != ELFCLASS32 or ident[5] != ELFDATA2LSB or ident[6] != EV_CURRENT:
        raise AuditError("ELF must be current-version, little-endian ELF32")
    if elf_type != ET_REL or machine != EM_RISCV:
        raise AuditError("ELF must be an RISC-V relocatable object")
    if elf_header_size != struct.calcsize(ELF32_HEADER):
        raise AuditError("unexpected ELF header size")
    if section_entry_size != struct.calcsize(ELF32_SECTION) or not section_count:
        raise AuditError("unsupported or missing ELF section table")
    if names_index >= section_count or section_offset & 3:
        raise AuditError("invalid ELF section-name table index or alignment")
    if not _range_ok(len(data), section_offset, section_count * section_entry_size):
        raise AuditError("ELF section table is outside the file")

    sections = []
    for index in range(section_count):
        fields = _unpack(ELF32_SECTION, data,
                         section_offset + index * section_entry_size,
                         "ELF section header %d" % index)
        (name_offset, section_type, flags, address, offset, size, link, info,
         alignment, entry_size) = fields
        if alignment and alignment & (alignment - 1):
            raise AuditError("section %d has non-power-of-two alignment" % index)
        if section_type != SHT_NOBITS and not _range_ok(len(data), offset, size):
            raise AuditError("section %d is outside the ELF file" % index)
        sections.append({
            "index": index,
            "name_offset": name_offset,
            "name": "",
            "type": section_type,
            "flags": flags,
            "address": address,
            "offset": offset,
            "size": size,
            "link": link,
            "info": info,
            "alignment": alignment,
            "entry_size": entry_size,
        })

    names_section = sections[names_index]
    if names_section["type"] != SHT_STRTAB:
        raise AuditError("ELF section-name table is not a string table")
    names = data[names_section["offset"]:names_section["offset"] + names_section["size"]]
    for section in sections:
        section["name"] = _cstring(names, section["name_offset"],
                                   "section %d name" % section["index"])

    symbol_tables = {}
    for section in sections:
        if section["type"] == SHT_RELA:
            if (section["offset"] & 3 or section["info"] >= section_count or
                    section["link"] >= section_count or
                    section["entry_size"] != struct.calcsize(ELF32_RELA) or
                    section["size"] % struct.calcsize(ELF32_RELA)):
                raise AuditError("malformed relocation section %s" % section["name"])
        elif section["type"] == SHT_SYMTAB:
            symbol_size = struct.calcsize(ELF32_SYMBOL)
            if (section["offset"] & 3 or section["link"] >= section_count or
                    section["entry_size"] != symbol_size or
                    section["size"] % symbol_size):
                raise AuditError("malformed symbol table %s" % section["name"])
            strings = sections[section["link"]]
            if strings["type"] != SHT_STRTAB:
                raise AuditError("symbol table %s does not link to a string table"
                                 % section["name"])
            string_data = data[strings["offset"]:strings["offset"] + strings["size"]]
            symbols = []
            count = section["size"] // symbol_size
            for symbol_index in range(count):
                fields = _unpack(ELF32_SYMBOL, data,
                                 section["offset"] + symbol_index * symbol_size,
                                 "symbol %d in %s" % (symbol_index, section["name"]))
                name_offset, value, symbol_size_bytes, info, other, section_index = fields
                name = _cstring(string_data, name_offset,
                                "symbol %d name in %s" % (symbol_index, section["name"]))
                if section_index < SHN_LORESERVE:
                    if section_index >= section_count:
                        raise AuditError("symbol %s has an invalid section index" % name)
                    if value > sections[section_index]["size"]:
                        raise AuditError("symbol %s exceeds its section" % name)
                symbols.append({
                    "name": name,
                    "value": value,
                    "size": symbol_size_bytes,
                    "info": info,
                    "other": other,
                    "section_index": section_index,
                })
            symbol_tables[section["index"]] = symbols

    return {
        "type": elf_type,
        "machine": machine,
        "flags": _unpack(ELF32_HEADER, data, 0, "ELF header")[7],
        "sections": sections,
        "symbol_tables": symbol_tables,
    }, data


def _runtime_section(section):
    """Mirror krnLoadModule's allocation and .eh_frame discard conditions."""
    return (bool(section["flags"] & SHF_ALLOC) and section["size"] > 0 and
            section["name"] != ".eh_frame")


def _symbol_is_resolved(symbol, sections):
    section_index = symbol["section_index"]
    if section_index == SHN_ABS:
        return True, None
    if section_index in (SHN_UNDEF, SHN_COMMON, SHN_XINDEX):
        return False, "undefined or special section index 0x%04x" % section_index
    if section_index >= SHN_LORESERVE or section_index >= len(sections):
        return False, "unsupported section index 0x%04x" % section_index
    section = sections[section_index]
    if not _runtime_section(section):
        return False, "section %s is not placed by the loader" % (
            section["name"] or "<unnamed>")
    return True, None


def audit_elf(data, supported=None):
    """Return (parsed ELF, runtime relocation names, issues) for one member."""
    if supported is None:
        supported = _read_supported_relocations()
    parsed, data = _parse_elf(data)
    sections = parsed["sections"]
    issues = []
    used = set()

    for relocation_section in sections:
        if relocation_section["type"] != SHT_RELA:
            continue
        target = sections[relocation_section["info"]]
        if not _runtime_section(target):
            continue

        symbol_section = sections[relocation_section["link"]]
        if symbol_section["type"] != SHT_SYMTAB:
            issues.append("runtime relocation section %s does not link to SHT_SYMTAB"
                          % relocation_section["name"])
            continue
        symbols = parsed["symbol_tables"].get(symbol_section["index"], [])
        count = relocation_section["size"] // struct.calcsize(ELF32_RELA)

        for relocation_index in range(count):
            offset = relocation_section["offset"] + relocation_index * struct.calcsize(ELF32_RELA)
            relocation_offset, info, addend = _unpack(
                ELF32_RELA, data, offset,
                "relocation %d in %s" % (relocation_index, relocation_section["name"]))
            symbol_index, relocation_type = info >> 8, info & 0xFF
            relocation_name = supported.get(relocation_type,
                                            "R_RISCV_UNKNOWN_%d" % relocation_type)
            used.add(relocation_name)

            if relocation_type not in supported:
                issues.append("unsupported runtime relocation %s in %s"
                              % (relocation_name, relocation_section["name"]))

            # krnLoadModule resolves every non-zero symbol index before
            # krnRelocOne, even when the relocation itself is NONE or RELAX.
            # PCREL_LO12 also calls symbol_value for symbol zero.
            needs_symbol_value = (symbol_index != 0 or relocation_type in
                                  (R_RISCV_PCREL_LO12_I, R_RISCV_PCREL_LO12_S))
            if not needs_symbol_value:
                continue
            if symbol_index >= len(symbols):
                issues.append("%s at 0x%x in %s uses out-of-range symbol index %d"
                              % (relocation_name, relocation_offset,
                                 target["name"], symbol_index))
                continue

            symbol = symbols[symbol_index]
            resolved, reason = _symbol_is_resolved(symbol, sections)
            if not resolved:
                symbol_name = symbol["name"] or "<unnamed>"
                issues.append("%s at 0x%x in %s references unplaced symbol %s: %s"
                              % (relocation_name, relocation_offset, target["name"],
                                 symbol_name, reason))

    return parsed, used, issues


def audit(path):
    data, package_members, declared = members(path)
    print("package %s  %d bytes on disk, %d declared, %d members\n"
          % (os.path.basename(path), len(data), declared, len(package_members)))
    supported = _read_supported_relocations()
    failed = 0

    for name, size, offset in package_members:
        member_data = data[offset:offset + size]
        try:
            parsed, used, issues = audit_elf(member_data, supported)
            status = "FAIL" if issues else "OK"
            print("  %-24s %8d  ELF32 REL  %s" % (name, size, status))
            print("      machine RISC-V   flags 0x%08x" % parsed["flags"])
            print("      %d runtime reloc types: %s"
                  % (len(used), " ".join(sorted(t.replace("R_RISCV_", "", 1)
                                                for t in used))))
            for issue in issues:
                print("      %s" % issue)
            if issues:
                failed += 1
        except AuditError as exc:
            print("  %-24s %8d  ? ?  FAIL" % (name, size))
            print("      %s" % exc)
            failed += 1

    print("\n%d member(s) failed" % failed)
    return failed


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if len(argv) != 1:
        print("usage: audit-package.py <path-to-aros-bsp.pkg>", file=sys.stderr)
        return 2
    try:
        return 1 if audit(argv[0]) else 0
    except (AuditError, OSError) as exc:
        print("audit-package: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
