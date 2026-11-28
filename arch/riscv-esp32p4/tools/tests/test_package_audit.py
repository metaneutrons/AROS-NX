import contextlib
import importlib.util
import io
import os
import struct
import tempfile
import unittest


REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
AUDIT_PATH = os.path.join(REPO, "arch/riscv-esp32p4/boot/audit-package.py")
SPEC = importlib.util.spec_from_file_location("audit_package", AUDIT_PATH)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def _pack_strings(strings):
    table = bytearray(b"\0")
    offsets = {"": 0}
    for value in strings:
        if value not in offsets:
            offsets[value] = len(table)
            table.extend(value.encode("utf-8"))
            table.append(0)
    return bytes(table), offsets


def make_elf(runtime_relocations=(), eh_frame_relocations=(),
             debug_relocations=(), zero_relocations=(), symbols=()):
    """Build a small ELF32 RISC-V ET_REL object for loader-audit tests."""
    section_specs = [
        {"name": ".shstrtab", "type": 3, "flags": 0, "data": b"", "align": 1},
        {"name": ".strtab", "type": 3, "flags": 0, "data": b"", "align": 1},
        {"name": ".symtab", "type": 2, "flags": 0, "data": b"", "align": 4,
         "link_name": ".strtab", "entry_size": 16},
        {"name": ".text", "type": 1, "flags": 0x6, "data": b"\0" * 8, "align": 4},
        {"name": ".eh_frame", "type": 1, "flags": 0x2, "data": b"\0" * 4, "align": 4},
        {"name": ".debug_info", "type": 1, "flags": 0, "data": b"\0" * 4, "align": 1},
        {"name": ".zero", "type": 1, "flags": 0x2, "data": b"", "align": 4},
    ]
    section_index = {spec["name"]: index + 1
                     for index, spec in enumerate(section_specs)}

    all_symbols = [{"name": "", "section": 0, "value": 0}]
    all_symbols.extend(symbols)
    symbol_names = [symbol["name"] for symbol in all_symbols]
    symbol_strings, symbol_name_offsets = _pack_strings(symbol_names)
    symbol_index = {}
    symbol_bytes = bytearray()
    for index, symbol in enumerate(all_symbols):
        if index and symbol["name"] in symbol_index:
            raise ValueError("test symbols must have unique names")
        if index:
            symbol_index[symbol["name"]] = index
        symbol_bytes.extend(struct.pack(
            "<IIIBBH", symbol_name_offsets[symbol["name"]],
            symbol.get("value", 0), symbol.get("size", 0),
            symbol.get("info", 0), symbol.get("other", 0),
            symbol["section"],
        ))
    section_specs[1]["data"] = symbol_strings
    section_specs[2]["data"] = bytes(symbol_bytes)

    relocation_specs = [
        (".rela.text", ".text", runtime_relocations),
        (".rela.eh_frame", ".eh_frame", eh_frame_relocations),
        (".rela.debug_info", ".debug_info", debug_relocations),
        (".rela.zero", ".zero", zero_relocations),
    ]
    for name, target, relocations in relocation_specs:
        if not relocations:
            continue
        relocation_data = bytearray()
        for offset, symbol, relocation_type, addend in relocations:
            symbol_number = symbol_index[symbol] if isinstance(symbol, str) else symbol
            relocation_data.extend(struct.pack(
                "<IIi", offset, (symbol_number << 8) | relocation_type, addend))
        section_specs.append({
            "name": name,
            "type": 4,
            "flags": 0,
            "data": bytes(relocation_data),
            "align": 4,
            "entry_size": 12,
            "link_name": ".symtab",
            "target_name": target,
        })
    section_index = {spec["name"]: index + 1
                     for index, spec in enumerate(section_specs)}
    section_names, section_name_offsets = _pack_strings(
        spec["name"] for spec in section_specs)
    section_specs[0]["data"] = section_names

    image = bytearray(struct.calcsize("<16sHHIIIIIHHHHHH"))
    section_headers = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for spec in section_specs:
        alignment = spec.get("align", 1) or 1
        padding = (-len(image)) & (alignment - 1)
        image.extend(b"\0" * padding)
        offset = len(image)
        payload = spec["data"]
        image.extend(payload)
        section_headers.append((
            section_name_offsets[spec["name"]], spec["type"], spec["flags"], 0,
            offset, len(payload), section_index.get(spec.get("link_name"), 0),
            section_index.get(spec.get("target_name"), 0), alignment,
            spec.get("entry_size", 0),
        ))

    image.extend(b"\0" * ((-len(image)) & 3))
    section_offset = len(image)
    for header in section_headers:
        image.extend(struct.pack("<IIIIIIIIII", *header))

    ident = b"\x7fELF" + bytes((1, 1, 1, 0, 0)) + b"\0" * 7
    struct.pack_into(
        "<16sHHIIIIIHHHHHH", image, 0, ident, 1, 243, 1, 0, 0,
        section_offset, 0, 52, 0, 0, 40, len(section_headers), 1,
    )
    return bytes(image)


def make_package(members):
    body = bytearray(b"PKG\x01\0\0\0\0")
    for name, elf in members:
        encoded_name = name.encode("utf-8")
        body.extend(struct.pack(">I", len(encoded_name)))
        body.extend(encoded_name)
        body.append(0)
        body.extend(struct.pack(">I", len(elf)))
        body.extend(elf)
    struct.pack_into(">I", body, 4, len(body))
    return bytes(body)


class PackageAuditTests(unittest.TestCase):
    def _audit_elf(self, elf):
        return AUDIT.audit_elf(elf, AUDIT._read_supported_relocations())

    def test_valid_runtime_relocations_and_absolute_symbols_pass(self):
        elf = make_elf(
            runtime_relocations=[(0, "text_symbol", 1, 0),
                                 (4, "absolute", 1, 3),
                                 (8, 0, AUDIT.R_RISCV_NONE, 0)],
            symbols=[
                {"name": "text_symbol", "section": 4},
                {"name": "absolute", "section": AUDIT.SHN_ABS,
                 "value": 0x12345678},
            ],
        )
        _parsed, _used, issues = self._audit_elf(elf)
        self.assertEqual([], issues)

    def test_runtime_relocation_to_discarded_eh_frame_symbol_fails(self):
        elf = make_elf(
            runtime_relocations=[(0, "__eh_frame_start", 1, 0)],
            symbols=[{"name": "__eh_frame_start", "section": 5}],
        )
        _parsed, _used, issues = self._audit_elf(elf)
        self.assertEqual(1, len(issues))
        self.assertIn("references unplaced symbol __eh_frame_start", issues[0])
        self.assertIn("section .eh_frame is not placed", issues[0])

    def test_relocations_in_discarded_nonalloc_and_zero_size_targets_are_ignored(self):
        elf = make_elf(
            eh_frame_relocations=[(0, "undefined", 250, 0)],
            debug_relocations=[(0, "undefined", 250, 0)],
            zero_relocations=[(0, "undefined", 250, 0)],
            symbols=[{"name": "undefined", "section": AUDIT.SHN_UNDEF}],
        )
        _parsed, used, issues = self._audit_elf(elf)
        self.assertEqual([], issues)
        self.assertEqual(set(), used)

    def test_symbol_in_allocated_zero_size_section_is_unplaced(self):
        elf = make_elf(
            runtime_relocations=[(0, "zero_symbol", 1, 0)],
            symbols=[{"name": "zero_symbol", "section": 7}],
        )
        _parsed, _used, issues = self._audit_elf(elf)
        self.assertEqual(1, len(issues))
        self.assertIn("section .zero is not placed", issues[0])

    def test_nonzero_symbol_index_is_resolved_even_for_noop_relocations(self):
        elf = make_elf(
            runtime_relocations=[(0, "undefined", AUDIT.R_RISCV_NONE, 0)],
            symbols=[{"name": "undefined", "section": AUDIT.SHN_UNDEF}],
        )
        _parsed, _used, issues = self._audit_elf(elf)
        self.assertEqual(1, len(issues))
        self.assertIn("references unplaced symbol undefined", issues[0])

    def test_symbol_zero_noop_relocation_does_not_resolve_a_symbol(self):
        elf = make_elf(runtime_relocations=[
            (0, 0, AUDIT.R_RISCV_NONE, 4),
            (4, 0, AUDIT.R_RISCV_RELAX, 0),
        ])
        _parsed, _used, issues = self._audit_elf(elf)
        self.assertEqual([], issues)

    def test_truncated_elf_section_table_is_rejected(self):
        with self.assertRaises(AUDIT.AuditError):
            self._audit_elf(make_elf()[:52])

    def test_package_cli_audit_returns_failure_for_runtime_bad_symbol(self):
        broken = make_elf(
            runtime_relocations=[(0, "__eh_frame_start", 1, 0)],
            symbols=[{"name": "__eh_frame_start", "section": 5}],
        )
        with tempfile.NamedTemporaryFile() as package_file:
            package_file.write(make_package([("esp32p4sdmmc.resource", broken)]))
            package_file.flush()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                failed = AUDIT.audit(package_file.name)
        self.assertEqual(1, failed)
        self.assertIn("esp32p4sdmmc.resource", output.getvalue())
        self.assertIn("references unplaced symbol __eh_frame_start", output.getvalue())


if __name__ == "__main__":
    unittest.main()
