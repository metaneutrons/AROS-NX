#!/usr/bin/env python3
"""Exercise the SDK source exporter against isolated synthetic fixtures."""

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import tomllib
import unittest


ROOTS = ["core-linklibs", "includes", "linklibs"]
PROFILES = ("generic-a", "generic-b")
ARCH_EXEC_RECIPES = (
    "arch/aarch64-native/exec/mmakefile.src",
    "arch/arm-native/exec/mmakefile.src",
    "arch/ppc-chrp/exec/mmakefile.src",
    "arch/riscv-native/exec/mmakefile.src",
)
ARCH_EXEC_DEPENDENCIES = (
    "includes-${AROS_TARGET_PLATFORM}-${AROS_TARGET_CPU}-${AROS_TARGET_VARIANT}",
    "kernel-exec-includes-${AROS_TARGET_PLATFORM}-${AROS_TARGET_CPU}-${AROS_TARGET_VARIANT}",
    "kernel-kernel-includes-${AROS_TARGET_PLATFORM}-${AROS_TARGET_CPU}-${AROS_TARGET_VARIANT}",
    "kernel-rtas-${AROS_TARGET_PLATFORM}-${AROS_TARGET_CPU}",
)


class Fixture:
    """A synthetic source tree with two independently declared contracts."""

    def __init__(self, root):
        self.root = root
        self.script = root / "compiler" / "sdk" / "export.py"
        self.targets = root / "aros-targets.toml"
        self.contracts = {}
        self.original_contracts = {}

    @classmethod
    def create(cls, root):
        root = Path(root)
        fixture = cls(root)
        (root / "compiler" / "sdk").mkdir(parents=True)
        fixture.script.write_bytes(Path(__file__).with_name("export.py").read_bytes())
        (root / "compiler" / "sdk" / "contracts").mkdir()
        (root / "compiler" / "sdk" / "probes").mkdir()

        target_lines = []
        for profile in PROFILES:
            target_lines.extend(
                [
                    "[[targets]]",
                    f'name = "{profile}"',
                    f'sdk_consumer_contract = "compiler/sdk/contracts/{profile}.json"',
                    f'abi = "m68k-{"68000" if profile == "generic-a" else "68020"}"',
                    f'sysroot = "synthetic-{profile}"',
                    'sdk_roots = ["core-linklibs", "includes", "linklibs"]',
                    "",
                ]
            )
        fixture.targets.write_text("\n".join(target_lines), encoding="utf-8")

        for profile in PROFILES:
            c_probe = f"compiler/sdk/probes/{profile}.c"
            cxx_probe = f"compiler/sdk/probes/{profile}.cpp"
            inputs = [
                f"linklibs/{profile}.a",
                f"includes/{profile}.h",
                cxx_probe,
                "aros-targets.toml",
                f"core-linklibs/{profile}.lib",
                c_probe,
            ]
            for source in inputs:
                if source == "aros-targets.toml":
                    continue
                path = root / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(("synthetic source: " + source + "\n").encode())

            document = {
                "schema": "aros-sdk-consumer-contract-v1",
                "profile": profile,
                "roots": list(ROOTS),
                "abi": {
                    "architecture": "m68k",
                    "cpu": "68000" if profile == "generic-a" else "68020",
                    "calling_convention": "m68k-elf",
                    "object_format": "hunk",
                },
                "configuration": {
                    "compiler": f"{profile}-cc",
                    "sysroot": f"synthetic-{profile}",
                    "link_mode": "static",
                },
                "native_sdk_link_probes": {
                    "c": {"source": c_probe, "libraries": ["sdkc"]},
                    "cxx": {"source": cxx_probe, "libraries": ["sdkcxx"]},
                },
                # Deliberately unsorted: update must canonicalize every contract.
                "inputs": [{"path": source} for source in inputs],
            }
            contract_path = root / "compiler" / "sdk" / "contracts" / f"{profile}.json"
            contract_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
            fixture.contracts[profile] = contract_path
            fixture.original_contracts[profile] = document
        return fixture

    def document(self, profile):
        return json.loads(self.contracts[profile].read_text(encoding="utf-8"))

    def save_document(self, profile, document):
        self.contracts[profile].write_text(
            json.dumps(document, indent=2) + "\n", encoding="utf-8"
        )


def snapshot(root):
    """Capture fixture contents and relevant metadata to prove no writes occurred."""
    root = Path(root)
    paths = [root, *sorted(root.rglob("*"))]
    result = {}
    for path in paths:
        relative = "." if path == root else path.relative_to(root).as_posix()
        metadata = path.lstat()
        common = (metadata.st_mode, metadata.st_size, metadata.st_mtime_ns, metadata.st_nlink)
        if stat.S_ISLNK(metadata.st_mode):
            result[relative] = ("symlink", common, os.readlink(path))
        elif stat.S_ISREG(metadata.st_mode):
            result[relative] = ("file", common, path.read_bytes())
        elif stat.S_ISDIR(metadata.st_mode):
            result[relative] = ("directory", common)
        else:
            result[relative] = ("other", common)
    return result


class ExportCLITest(unittest.TestCase):
    def new_fixture(self):
        temporary = tempfile.TemporaryDirectory(prefix="aros-sdk-export-")
        self.addCleanup(temporary.cleanup)
        return Fixture.create(Path(temporary.name))

    def invoke(self, fixture, mode):
        return subprocess.run(
            [sys.executable, str(fixture.script), mode],
            cwd=fixture.root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def assert_rejected_without_mutation(self, fixture, expected_error, mode="--update"):
        before = snapshot(fixture.root)
        result = self.invoke(fixture, mode)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(expected_error, result.stderr)
        self.assertEqual(snapshot(fixture.root), before)

    def test_update_hashes_and_sorts_each_contract_and_check_is_read_only(self):
        fixture = self.new_fixture()
        targets_before = fixture.targets.read_bytes()
        originals = {
            profile: json.loads(fixture.contracts[profile].read_text(encoding="utf-8"))
            for profile in PROFILES
        }

        update = self.invoke(fixture, "--update")
        self.assertEqual(update.returncode, 0, update.stdout + update.stderr)
        self.assertEqual(fixture.targets.read_bytes(), targets_before)

        for profile in PROFILES:
            before = originals[profile]
            after = fixture.document(profile)
            expected_inputs = [
                {
                    "path": source,
                    "sha256": hashlib.sha256((fixture.root / source).read_bytes()).hexdigest(),
                }
                for source in sorted(entry["path"] for entry in before["inputs"])
            ]
            self.assertEqual(after["inputs"], expected_inputs)
            self.assertEqual(
                {key: value for key, value in after.items() if key != "inputs"},
                {key: value for key, value in before.items() if key != "inputs"},
            )

        after_update = snapshot(fixture.root)
        check = self.invoke(fixture, "--check")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        self.assertEqual(snapshot(fixture.root), after_update)

    def test_rejects_invalid_declarations_without_mutation(self):
        cases = (
            (
                "missing C probe",
                lambda fixture: self._remove_probe(fixture, "generic-a", "c"),
                "expected exactly C and C++ application probes",
            ),
            (
                "missing C++ probe",
                lambda fixture: self._remove_probe(fixture, "generic-a", "cxx"),
                "expected exactly C and C++ application probes",
            ),
            (
                "reduced roots",
                self._reduce_roots,
                "SDK consumer requires the complete sorted SDK roots",
            ),
            (
                "duplicate JSON key",
                self._duplicate_json_key,
                "duplicate JSON key: profile",
            ),
            (
                "duplicate SDK preset",
                self._duplicate_preset,
                "duplicate source preset",
            ),
            (
                "duplicate contract selector",
                self._duplicate_selector,
                "ambiguous SDK source contract",
            ),
            (
                "symlink input",
                self._symlink_input,
                "symlink source input: includes/generic-a.h",
            ),
            (
                "hardlinked input",
                self._hardlink_input,
                "nonregular, aliased or oversized source input: includes/generic-a.h",
            ),
            (
                "unsafe input path",
                self._unsafe_input,
                "unsafe source input: ../escape.txt",
            ),
        )
        for label, mutate, expected_error in cases:
            with self.subTest(case=label):
                fixture = self.new_fixture()
                mutate(fixture)
                self.assert_rejected_without_mutation(fixture, expected_error)

    def test_changed_sealed_input_fails_check_without_mutation(self):
        fixture = self.new_fixture()
        update = self.invoke(fixture, "--update")
        self.assertEqual(update.returncode, 0, update.stdout + update.stderr)

        changed = fixture.root / "includes" / "generic-a.h"
        changed.write_bytes(changed.read_bytes() + b"changed after sealing\n")
        self.assert_rejected_without_mutation(
            fixture, "SDK source seals are stale; review inputs before --update", "--check"
        )

    def test_validates_all_contracts_before_writing_any(self):
        fixture = self.new_fixture()
        self._remove_probe(fixture, "generic-b", "cxx")
        # generic-a is stale and would change if the exporter wrote incrementally.
        self.assert_rejected_without_mutation(
            fixture, "expected exactly C and C++ application probes"
        )

    @staticmethod
    def _remove_probe(fixture, profile, language):
        document = fixture.document(profile)
        del document["native_sdk_link_probes"][language]
        fixture.save_document(profile, document)

    @staticmethod
    def _reduce_roots(fixture):
        document = fixture.document("generic-a")
        document["roots"] = ["includes", "linklibs"]
        fixture.save_document("generic-a", document)

    @staticmethod
    def _duplicate_json_key(fixture):
        path = fixture.contracts["generic-a"]
        contents = path.read_text(encoding="utf-8")
        old = '"profile": "generic-a",'
        if old not in contents:
            raise AssertionError("fixture profile key was not found")
        path.write_text(contents.replace(old, old + ' "profile": "generic-a",', 1), encoding="utf-8")

    @staticmethod
    def _duplicate_preset(fixture):
        contents = fixture.targets.read_text(encoding="utf-8")
        old = 'name = "generic-b"'
        if old not in contents:
            raise AssertionError("fixture generic-b preset was not found")
        fixture.targets.write_text(contents.replace(old, 'name = "generic-a"', 1), encoding="utf-8")

    @staticmethod
    def _duplicate_selector(fixture):
        contents = fixture.targets.read_text(encoding="utf-8")
        old = 'sdk_consumer_contract = "compiler/sdk/contracts/generic-b.json"'
        new = 'sdk_consumer_contract = "compiler/sdk/contracts/generic-a.json"'
        if old not in contents:
            raise AssertionError("fixture generic-b selector was not found")
        fixture.targets.write_text(contents.replace(old, new, 1), encoding="utf-8")

    @staticmethod
    def _symlink_input(fixture):
        path = fixture.root / "includes" / "generic-a.h"
        target = fixture.root / "includes" / "alternate.h"
        target.write_bytes(b"alternate synthetic header\n")
        path.unlink()
        path.symlink_to("alternate.h")

    @staticmethod
    def _hardlink_input(fixture):
        path = fixture.root / "includes" / "generic-a.h"
        os.link(path, fixture.root / "includes" / "generic-a-alias.h")

    @staticmethod
    def _unsafe_input(fixture):
        document = fixture.document("generic-a")
        document["inputs"].append({"path": "../escape.txt"})
        fixture.save_document("generic-a", document)


class SourceContractTest(unittest.TestCase):
    def test_native_sdk_flags_preserve_recipe_local_isa_and_order(self):
        # Execute only the source-owned variable binding in an isolated Make
        # environment; never query Make's database or inherited environment.
        configuration = Path(__file__).with_name("config-common.mk").read_text()
        with tempfile.TemporaryDirectory() as directory:
            makefile = Path(directory) / "flags.mk"

            def flags(source):
                makefile.write_text(
                    source
                    + "\nISA_FLAGS += -mavx\n"
                    + "USER_CFLAGS := -DORDER=1 -UORDER -DORDER=2\n"
                    + ".PHONY: sdk-flags\nsdk-flags:\n"
                    + "\t@printf '%s\\n' '$(CFLAGS)'\n"
                )
                result = subprocess.run(
                    ["make", "-f", str(makefile), "sdk-flags"],
                    cwd=directory,
                    env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"},
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout.strip()

            self.assertEqual(
                flags(configuration),
                "-mavx -O2 -fno-common -DORDER=1 -UORDER -DORDER=2",
            )
            disconnected = configuration.replace(
                "TARGET_ISA_CFLAGS = $(ISA_FLAGS)", "TARGET_ISA_CFLAGS :="
            )
            self.assertNotEqual(disconnected, configuration)
            self.assertEqual(
                flags(disconnected), "-O2 -fno-common -DORDER=1 -UORDER -DORDER=2"
            )

    def test_arch_exec_optional_dependencies_are_exact_and_recipe_scoped(self):
        root = Path(__file__).resolve().parents[2]
        profiles = {
            target["name"]: target
            for target in tomllib.loads((root / "aros-targets.toml").read_text())["targets"]
        }

        for profile in ("pc-x86_64", "arm-raspi", "rpi-aarch64"):
            path = root / "compiler" / "sdk" / profile / "consumer-v1.json"
            contract = json.loads(path.read_text(encoding="utf-8"))
            selected = profiles[profile]
            target = f"kernel-exec-{selected['platform']}-{selected['arch']}"
            expected = {
                (recipe, target, dependency)
                for recipe in ARCH_EXEC_RECIPES
                for dependency in ARCH_EXEC_DEPENDENCIES
            }
            entries = contract["optional_meta_dependencies"]
            relevant = [
                (entry["recipe"], entry["target"], entry["dependency"])
                for entry in entries
                if entry["recipe"].startswith("arch/")
                or entry["target"].startswith("kernel-exec-")
            ]

            with self.subTest(profile=profile):
                self.assertEqual(len(relevant), len(expected))
                self.assertEqual(set(relevant), expected)
                self.assertTrue(
                    set(ARCH_EXEC_RECIPES).issubset(
                        {entry["path"] for entry in contract["inputs"]}
                    ),
                    "each optional selector owner must be sealed",
                )
                self.assertFalse(
                    any(
                        entry["recipe"] == "rom/exec/mmakefile.src"
                        and entry["target"] == "kernel-exec-includes"
                        for entry in entries
                    ),
                    "the mandatory rom/exec kernel-exec-includes edge must not be optional",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
