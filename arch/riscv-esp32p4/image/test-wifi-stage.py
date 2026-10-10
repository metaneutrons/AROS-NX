#!/usr/bin/env python3
"""Host tests for the credential-free ESP32-P4 WLAN stage generator."""

import importlib.util
from pathlib import Path
import re
import subprocess
import struct
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
STAGE_PATH = HERE / "make-wifi-stage.py"
AUDIT_TESTS_PATH = REPO / "arch/riscv-esp32p4/tools/tests/test_package_audit.py"
WANDERER_DEFAULTS = ("global", "toolbar", "statusbar", "backdrop")
DESKTOP_STAGE_PATHS = {
    name: "Prefs/Env-Archive/Sys/Wanderer/" + name + ".prefs"
    for name in WANDERER_DEFAULTS
}
DESKTOP_SOURCE_PATHS = {
    name: "workbench/system/Wanderer/Env/default/" + name + ".prefs"
    for name in WANDERER_DEFAULTS
}
ABI_SOURCE_PATH = "rom/aros/arosinquirea.c"
ABI_STAGE_PATH = "Prefs/Env-Archive/ABI"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


STAGE = load_module("wifi_stage", STAGE_PATH)
ELF_FIXTURES = load_module("package_audit_fixtures", AUDIT_TESTS_PATH)


def make_wifi_elf(value=1, storage="data", symbol_size=2, symbol_info=0x11,
                  symbol_section=4, copies=1):
    """Build a small ELF32 RISC-V ET_REL object containing WifiVolatile."""
    section_names = ("", ".shstrtab", ".strtab", ".symtab", ".data")
    section_types = (0, 3, 3, 2, 1 if storage == "data" else 8)
    section_flags = (0, 0, 0, 0, 0x3)
    section_alignments = (0, 1, 1, 4, 2)
    if storage not in ("data", "bss"):
        raise ValueError("unknown test storage class")

    name_offsets = {}
    names = bytearray(b"\0")
    for name in section_names[1:]:
        name_offsets[name] = len(names)
        names.extend(name.encode("ascii") + b"\0")
    string_table = b"\0WifiVolatile\0"
    value_offset = 1

    symbols = [b"\0" * 16]
    for _ in range(copies):
        symbols.append(struct.pack(
            "<IIIBBH", value_offset, 0, symbol_size, symbol_info, 0,
            symbol_section,
        ))
    symbol_table = b"".join(symbols)

    data = bytearray(struct.calcsize("<16sHHIIIIIHHHHHH"))
    section_payloads = {
        1: bytes(names),
        2: string_table,
        3: symbol_table,
        4: (value.to_bytes(2, "little") if storage == "data" else b""),
    }
    section_sizes = {index: len(payload)
                     for index, payload in section_payloads.items()}
    if storage == "bss":
        section_sizes[4] = 2

    offsets = [0] * len(section_names)
    for index in range(1, len(section_names)):
        alignment = section_alignments[index]
        if alignment:
            data.extend(b"\0" * ((-len(data)) & (alignment - 1)))
        offsets[index] = len(data)
        if section_types[index] != 8:
            data.extend(section_payloads[index])

    data.extend(b"\0" * ((-len(data)) & 3))
    section_header_offset = len(data)
    section_headers = [b"\0" * 40]
    for index in range(1, len(section_names)):
        name = section_names[index]
        link = 2 if index == 3 else 0
        entry_size = 16 if index == 3 else 0
        section_headers.append(struct.pack(
            "<IIIIIIIIII", name_offsets[name], section_types[index],
            section_flags[index], 0, offsets[index], section_sizes[index],
            link, 0, section_alignments[index], entry_size,
        ))
    data.extend(b"".join(section_headers))

    ident = b"\x7fELF" + bytes((1, 1, 1, 0, 0)) + b"\0" * 7
    struct.pack_into(
        "<16sHHIIIIIHHHHHH", data, 0, ident, 1, 243, 1, 0, 0,
        section_header_offset, 0, 52, 0, 0, 40, len(section_headers), 1,
    )
    return bytes(data)


def wifi_volatile_value(data):
    """Inspect the required global, initialized two-byte policy object."""
    parsed, _used = STAGE.ELF._parse_elf(data)
    symbols = [symbol for table in parsed["symbol_tables"].values()
               for symbol in table if symbol["name"] == "WifiVolatile"]
    if len(symbols) != 1:
        raise AssertionError("fixture does not contain one WifiVolatile symbol")
    symbol = symbols[0]
    section = parsed["sections"][symbol["section_index"]]
    if (symbol["info"] != 0x11 or symbol["size"] != 2 or
            section["type"] == STAGE.ELF.SHT_NOBITS or
            section["flags"] & 0x3 != 0x3):
        raise AssertionError("WifiVolatile is not a writable initialized BOOL")
    start = section["offset"] + symbol["value"]
    return int.from_bytes(data[start:start + 2], "little")


class WifiStageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="wifi-stage-test-")
        self.base = Path(self.temporary.name)
        self.aros_root = self.base / "aros-root"
        self.source_root = self.base / "source-root"
        self.output = self.base / "stage"
        self._make_inputs()

    def tearDown(self):
        self.temporary.cleanup()

    def _make_inputs(self, wifi_elf=None, aros_root=None, source_root=None):
        aros_root = aros_root or self.aros_root
        source_root = source_root or self.source_root
        aros_root.mkdir(parents=True, exist_ok=True)
        source_root.mkdir(parents=True, exist_ok=True)

        wifi_elf = wifi_elf or make_wifi_elf()
        for relative in STAGE.BINARIES:
            path = aros_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(wifi_elf if relative == "Prefs/WiFi"
                             else ELF_FIXTURES.make_elf())

        for relative in STAGE.IMAGES:
            path = aros_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("image fixture: " + relative + "\n").encode("ascii"))

        icon = aros_root / "Prefs/Touchscreen.info"
        icon.parent.mkdir(parents=True, exist_ok=True)
        icon.write_bytes(b"test icon fixture\n")

        database = source_root / "workbench/network/stacks/AROSTCP/Env/db"
        database.mkdir(parents=True, exist_ok=True)
        for name in STAGE.DB_FILES:
            (database / name).write_bytes(("database fixture: " + name + "\n")
                                          .encode("ascii"))

        defaults = source_root / "workbench/system/Wanderer/Env/default"
        defaults.mkdir(parents=True, exist_ok=True)
        for name in WANDERER_DEFAULTS:
            (defaults / (name + ".prefs")).write_bytes(
                ("stock Wanderer fixture: " + name + "\n").encode("ascii"))

        abi_source = source_root / ABI_SOURCE_PATH
        abi_source.parent.mkdir(parents=True, exist_ok=True)
        abi_source.write_text(
            "/* source ABI fixture */\n"
            "#define AROS_ABI_VERSION_MAJOR -1\n",
            encoding="ascii",
        )

        # These represent private build-tree content that must not be imported.
        saved = aros_root / "Prefs/Env-Archive/AROSTCP/db/private-saved-network"
        saved.parent.mkdir(parents=True, exist_ok=True)
        saved.write_text("host saved network sentinel\n", encoding="ascii")
        (aros_root / "Package-Startup").write_text(
            "host package startup sentinel\n", encoding="ascii")
        (aros_root / "C/Package-Startup").write_text(
            "host C startup sentinel\n", encoding="ascii")

    def _generate(self, output=None, aros_root=None, source_root=None,
                  include_desktop=False):
        return STAGE.generate(
            aros_root or self.aros_root,
            output or self.output,
            source_root or self.source_root,
            include_desktop=include_desktop,
        )

    def _assert_rejected_before_reservation(self, output=None, aros_root=None,
                                            source_root=None,
                                            include_desktop=False):
        target = output or self.output
        with self.assertRaises(ValueError):
            self._generate(target, aros_root, source_root, include_desktop)
        self.assertFalse(target.exists(), "invalid input reserved an output directory")

    def test_desktop_defaults_add_exact_stock_preferences_and_abi(self):
        arbitrary = (self.aros_root /
                     "Prefs/Env-Archive/Sys/Wanderer/custom-build.prefs")
        arbitrary.parent.mkdir(parents=True, exist_ok=True)
        arbitrary.write_bytes(b"arbitrary build preference credential sentinel\n")
        build_copy = (self.aros_root /
                      "Prefs/Env-Archive/Sys/Wanderer/global.prefs")
        build_copy.write_bytes(b"build-tree global preference sentinel\n")

        baseline = self._generate(self.base / "stage-without-desktop")
        desktop_output = self.base / "stage-with-desktop"
        desktop = self._generate(desktop_output, include_desktop=True)
        repeated = self._generate(
            self.base / "stage-with-desktop-repeat", include_desktop=True)

        expected_additions = {
            DESKTOP_STAGE_PATHS[name]:
                (self.source_root / DESKTOP_SOURCE_PATHS[name]).read_bytes()
            for name in WANDERER_DEFAULTS
        }
        expected_additions[ABI_STAGE_PATH] = b"-1\n"
        self.assertEqual(set(baseline) | set(expected_additions), set(desktop))
        self._assert_exact_tree(set(desktop), desktop_output)
        repeated_output = self.base / "stage-with-desktop-repeat"
        self._assert_exact_tree(set(repeated), repeated_output)
        self.assertEqual(desktop, repeated)
        for relative, content in expected_additions.items():
            self.assertEqual(content, desktop[relative])
            self.assertEqual(content, (desktop_output / relative).read_bytes())

        all_content = b"\n".join(desktop.values())
        self.assertNotIn("custom-build.prefs", desktop)
        self.assertNotIn(b"arbitrary build preference credential sentinel", all_content)
        self.assertNotIn(b"build-tree global preference sentinel", all_content)

    def test_desktop_defaults_reject_missing_empty_and_symlink_preferences(self):
        for label in ("missing", "empty", "symlink"):
            with self.subTest(label=label):
                aros_root = self.base / ("aros-" + label)
                source_root = self.base / ("source-" + label)
                self._make_inputs(aros_root=aros_root, source_root=source_root)
                source = source_root / DESKTOP_SOURCE_PATHS["toolbar"]
                if label == "missing":
                    source.unlink()
                elif label == "empty":
                    source.write_bytes(b"")
                else:
                    outside = self.base / "external-toolbar-prefs"
                    outside.write_bytes(b"external stock preference fixture\n")
                    source.unlink()
                    source.symlink_to(outside)

                self._assert_rejected_before_reservation(
                    output=self.base / ("stage-invalid-desktop-" + label),
                    aros_root=aros_root,
                    source_root=source_root,
                    include_desktop=True,
                )

    def test_desktop_defaults_reject_missing_duplicate_and_nonnumeric_abi(self):
        for label, contents in (
            ("missing", None),
            ("duplicate", "#define AROS_ABI_VERSION_MAJOR -1\n"
                          "#define AROS_ABI_VERSION_MAJOR 3\n"),
            ("nonnumeric", "#define AROS_ABI_VERSION_MAJOR current\n"),
        ):
            with self.subTest(label=label):
                aros_root = self.base / ("aros-abi-" + label)
                source_root = self.base / ("source-abi-" + label)
                self._make_inputs(aros_root=aros_root, source_root=source_root)
                source = source_root / ABI_SOURCE_PATH
                if contents is None:
                    source.unlink()
                else:
                    source.write_text(contents, encoding="ascii")

                self._assert_rejected_before_reservation(
                    output=self.base / ("stage-invalid-abi-" + label),
                    aros_root=aros_root,
                    source_root=source_root,
                    include_desktop=True,
                )

    def test_cli_desktop_defaults_flag_stages_the_five_extra_files(self):
        output = self.base / "stage-cli-desktop"
        result = subprocess.run(
            [sys.executable, str(STAGE_PATH),
             "--aros-root", str(self.aros_root),
             "--output", str(output), "--desktop-defaults"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Staged ", result.stdout)
        for name in WANDERER_DEFAULTS:
            source = REPO / DESKTOP_SOURCE_PATHS[name]
            self.assertEqual(source.read_bytes(),
                             (output / DESKTOP_STAGE_PATHS[name]).read_bytes())
        abi_source = (REPO / ABI_SOURCE_PATH).read_text(encoding="ascii")
        abi_values = re.findall(
            r"^#define\s+AROS_ABI_VERSION_MAJOR\s+(-?\d+)\s*(?:/\*.*)?$",
            abi_source, re.MULTILINE,
        )
        self.assertEqual(1, len(abi_values))
        self.assertEqual((abi_values[0] + "\n").encode("ascii"),
                         (output / ABI_STAGE_PATH).read_bytes())

    def test_stage_has_the_exact_runtime_set_and_reproducible_content(self):
        first = self._generate(self.output)
        second_output = self.base / "stage-second"
        second = self._generate(second_output)

        defaults = STAGE.DEFAULTS.defaults()
        required_binaries = {
            "Prefs/WiFi", "Prefs/Network", "Prefs/NetModules/net4.netprefs",
            "Prefs/NetModules/net6.netprefs", "C/WirelessManager",
            "Devs/Networks/esp32p4wlan.device",
            "System/Network/AROSTCP/C/AROSTCP",
            "System/Network/AROSTCP/C/dhclient", "Libs/posixc.library",
            "Libs/stdcio.library", "Libs/stdc.library",
        }
        required_images = {
            "System/Images/interface", "System/Images/host",
            "System/Images/wireless", "System/Images/protocol",
        }
        required_defaults = set(defaults)
        database_files = {
            "Prefs/Env-Archive/AROSTCP/db/" + name
            for name in STAGE.DB_FILES
        }
        expected = (required_defaults | required_binaries | required_images |
                    database_files | {"Prefs/WiFi.info", "Prefs/Network.info"})

        self.assertEqual(required_binaries, set(STAGE.BINARIES))
        self.assertEqual(required_images, set(STAGE.IMAGES))
        self.assertEqual(expected, set(first))
        self.assertEqual(expected, set(second))
        self._assert_exact_tree(expected, self.output)
        self._assert_exact_tree(expected, second_output)
        self.assertEqual(first, second)
        for relative, content in first.items():
            self.assertEqual(content, (self.output / relative).read_bytes())
            self.assertEqual(content, (second_output / relative).read_bytes())

        for relative, content in defaults.items():
            self.assertEqual(content.encode("ascii"), first[relative])
        for name in STAGE.DB_FILES:
            source = self.source_root / "workbench/network/stacks/AROSTCP/Env/db" / name
            self.assertEqual(source.read_bytes(),
                             first["Prefs/Env-Archive/AROSTCP/db/" + name])
        self.assertEqual("/T/dhclient.leases",
                         defaults["Prefs/Env-Archive/PATH_DHCLIENT_DB"])
        self.assertEqual("/T/dhclient.pid",
                         defaults["Prefs/Env-Archive/PATH_DHCLIENT_PID"])
        self.assertEqual("False", defaults["Prefs/Env-Archive/AROSTCP/AutoRun"])
        self.assertEqual(
            "False", defaults["Prefs/Env-Archive/AROSTCP/WirelessAutoRun"])
        self.assertIn("DEVS:Networks/esp32p4wlan.device UNIT 0",
                      defaults["Prefs/Env-Archive/AROSTCP/WirelessDevice"])
        self.assertIn("IP=DHCP", defaults["Prefs/Env-Archive/AROSTCP/db/interfaces"])
        self.assertEqual(1, wifi_volatile_value(first["Prefs/WiFi"]))
        self.assertEqual((self.aros_root / "Prefs/Touchscreen.info").read_bytes(),
                         first["Prefs/WiFi.info"])
        self.assertEqual(first["Prefs/WiFi.info"], first["Prefs/Network.info"])

        all_content = b"\n".join(first.values())
        staged_names = b"\n".join(path.encode("ascii") for path in first)
        self.assertNotIn(b"Package-Startup", staged_names)
        self.assertNotIn(b"private-saved-network", staged_names)
        self.assertNotIn(b"host saved network sentinel", all_content)
        self.assertNotIn(b"host package startup sentinel", all_content)
        self.assertNotIn(b"host C startup sentinel", all_content)
        self.assertNotRegex(all_content.decode("latin-1"), r"192\.168\.0\.\d+")
        self.assertIn(b"default route comes from DHCP", first[
            "Prefs/Env-Archive/AROSTCP/db/static-routes"])

    @staticmethod
    def _tree_files(root):
        return {path.relative_to(root).as_posix() for path in root.rglob("*")
                if path.is_file()}

    def _assert_exact_tree(self, expected, root):
        actual = self._tree_files(root)
        if self._filesystem_is_case_sensitive():
            self.assertEqual(expected, actual)
            return
        folded_expected = {path.casefold() for path in expected}
        folded_actual = {path.casefold() for path in actual}
        self.assertEqual(len(actual), len(folded_actual),
                         "output has names that collide on case-insensitive filesystems")
        self.assertEqual(folded_expected, folded_actual)

    def _filesystem_is_case_sensitive(self):
        probe = self.base / ".CaseSensitivityProbe"
        probe.mkdir(exist_ok=False)
        try:
            return not (self.base / ".casesensitivityprobe").exists()
        finally:
            probe.rmdir()

    def test_missing_input_is_rejected_before_reserving_output(self):
        (self.aros_root / "Prefs/Network").unlink()
        self._assert_rejected_before_reservation()

    def test_empty_input_is_rejected_before_reserving_output(self):
        (self.aros_root / "System/Images/interface").write_bytes(b"")
        self._assert_rejected_before_reservation()

    def test_non_riscv_native_elf_is_rejected_before_reserving_output(self):
        path = self.aros_root / "Prefs/Network"
        data = bytearray(path.read_bytes())
        struct.pack_into("<H", data, 18, 62)  # ELF machine: x86-64
        path.write_bytes(data)
        self._assert_rejected_before_reservation()

    def test_symlinked_input_and_parent_are_rejected_before_reservation(self):
        outside = self.base / "outside"
        outside.write_bytes(ELF_FIXTURES.make_elf())
        manager = self.aros_root / "C/WirelessManager"
        manager.unlink()
        manager.symlink_to(outside)
        self._assert_rejected_before_reservation()

        # A clean fixture with an intermediate directory symlink.
        manager.unlink()
        self.output = self.base / "stage-parent-link"
        self._make_inputs()
        system = self.aros_root / "System"
        real_system = self.aros_root / "System-real"
        system.rename(real_system)
        system.symlink_to(real_system, target_is_directory=True)
        self._assert_rejected_before_reservation()

    def test_symlinked_aros_and_source_roots_are_rejected(self):
        ar_link = self.base / "aros-link"
        ar_link.symlink_to(self.aros_root, target_is_directory=True)
        self._assert_rejected_before_reservation(
            output=self.base / "stage-ar-link", aros_root=ar_link)

        source_link = self.base / "source-link"
        source_link.symlink_to(self.source_root, target_is_directory=True)
        self._assert_rejected_before_reservation(
            output=self.base / "stage-source-link", source_root=source_link)

    def test_symlinked_database_input_is_rejected_before_reservation(self):
        source = (self.source_root /
                  "workbench/network/stacks/AROSTCP/Env/db/hosts")
        outside = self.base / "external-hosts"
        outside.write_text("fixture only\n", encoding="ascii")
        source.unlink()
        source.symlink_to(outside)
        self._assert_rejected_before_reservation()

    def test_empty_source_database_input_is_rejected_before_reserving_output(self):
        source = (self.source_root /
                  "workbench/network/stacks/AROSTCP/Env/db/services")
        source.write_bytes(b"")
        self._assert_rejected_before_reservation()

    def test_symlinked_catalog_directory_is_rejected_before_reservation(self):
        catalogs = self.aros_root / "Locale/Catalogs"
        catalogs.mkdir(parents=True)
        outside = self.base / "outside-language"
        outside.mkdir()
        (catalogs / "English").symlink_to(outside, target_is_directory=True)
        self._assert_rejected_before_reservation()

    def test_bad_wifi_volatile_policies_are_rejected_before_reservation(self):
        variants = (
            ("default-zero", make_wifi_elf(value=0)),
            ("uninitialized", make_wifi_elf(storage="bss")),
            ("duplicate", make_wifi_elf(copies=2)),
            ("bad-size", make_wifi_elf(symbol_size=4)),
            ("not-global-object", make_wifi_elf(symbol_info=0x10)),
            ("undefined", make_wifi_elf(symbol_section=0)),
        )
        for label, elf in variants:
            with self.subTest(label=label):
                self._make_inputs(wifi_elf=elf)
                target = self.base / ("stage-" + label)
                self._assert_rejected_before_reservation(output=target)

    def test_catalogs_are_copied_only_from_the_allowlisted_paths(self):
        wifi_catalog = (self.aros_root /
                        "Locale/Catalogs/English/System/Prefs/WiFi.catalog")
        network_catalog = (self.aros_root /
                           "Locale/Catalogs/English/System/Prefs/Network.catalog")
        wifi_catalog.parent.mkdir(parents=True)
        wifi_catalog.write_bytes(b"fixture WiFi catalog\n")
        network_catalog.write_bytes(b"fixture Network catalog\n")
        (self.aros_root / "Locale/Catalogs/English/System/Prefs/Other.catalog")\
            .write_bytes(b"not allowlisted\n")

        payload = self._generate()
        self.assertEqual(b"fixture WiFi catalog\n",
                         payload["Locale/Catalogs/English/System/Prefs/WiFi.catalog"])
        self.assertEqual(
            b"fixture Network catalog\n",
            payload["Locale/Catalogs/English/System/Prefs/Network.catalog"])
        self.assertNotIn("Locale/Catalogs/English/System/Prefs/Other.catalog", payload)

    def test_existing_and_symlink_outputs_are_left_untouched(self):
        self.output.mkdir()
        marker = self.output / "keep.txt"
        marker.write_bytes(b"existing output marker\n")
        with self.assertRaises(FileExistsError):
            self._generate()
        self.assertEqual({"keep.txt"}, self._tree_files(self.output))
        self.assertEqual(b"existing output marker\n", marker.read_bytes())

        target = self.base / "output-target"
        target.mkdir()
        target_marker = target / "keep.txt"
        target_marker.write_bytes(b"symlink target marker\n")
        output_link = self.base / "output-link"
        output_link.symlink_to(target, target_is_directory=True)
        with self.assertRaises(FileExistsError):
            self._generate(output_link)
        self.assertTrue(output_link.is_symlink())
        self.assertEqual({"keep.txt"}, self._tree_files(target))
        self.assertEqual(b"symlink target marker\n", target_marker.read_bytes())


if __name__ == "__main__":
    unittest.main()
