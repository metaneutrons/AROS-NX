"""Deterministic tests for the native SDK source-seal exporter."""

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CONTRACT_RELATIVE = Path("arch/riscv-esp32p4/native-sdk/consumer-v2.json")
EXPORTER_RELATIVE = Path("arch/riscv-esp32p4/native-sdk/export.py")
EXPECTED_ROOTS = ["core-linklibs", "includes", "linklibs"]


class ExporterTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.parent = Path(self.temporary_directory.name)
        self.root = self.parent / "checkout"
        self.root.mkdir()

        self.exporter_path = self.root / EXPORTER_RELATIVE
        self.exporter_path.parent.mkdir(parents=True)
        shutil.copyfile(Path(__file__).with_name("export.py"), self.exporter_path)
        self.contract_path = self.root / CONTRACT_RELATIVE

        self.write_source("src/a.in", b"alpha source\n")
        self.write_source("src/b.in", b"beta source\n")
        self.write_source("src/probe.c", b"int main(void) { return 0; }\n")
        self.write_source("src/probe.cpp", b"int main() { return 0; }\n")
        self.write_contract(self.make_contract())

    def write_source(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def make_contract(self, input_paths=("src/a.in", "src/b.in", "src/probe.c", "src/probe.cpp")):
        return {
            "schema": "aros-native-consumer-contract-v2",
            "native_sdk_link_probes": {
                "c": {"source": "src/probe.c", "libraries": []},
                "cxx": {"source": "src/probe.cpp", "libraries": []},
            },
            "profile": "esp32p4-riscv-gnu",
            "source_baseline": "fixture-baseline",
            "roots": EXPECTED_ROOTS.copy(),
            "inputs": [
                {
                    "path": name,
                    "sha256": hashlib.sha256((self.root / name).read_bytes()).hexdigest()
                    if (self.root / name).is_file()
                    else "0" * 64,
                }
                for name in input_paths
            ],
            "abi": {
                "source_cpu": "riscv",
                "target_triple": "riscv-aros",
                "isa": "rv32imafc_zicsr_zifencei_zaamo_zalrsc",
                "abi": "ilp32f",
                "code_model": "medany",
                "flavour": "standalone",
                "platform_smp": True,
                "use_mmu": False,
            },
            "metamake_projection": "arch/riscv-esp32p4/native-sdk/metamake-v1.json",
            "make_variables": {"FLAVOUR": "standalone", "TARGET_UNITTESTS": "no"},
            "host_make_variables": {
                "linux-x86_64": {"AROS_HOST_ARCH": "linux", "AROS_HOST_CPU": "x86_64"}
            },
            "make_include_bindings": {},
            "generated_make_templates": {
                "compiler/include/geninc.cfg": {
                    "template": "compiler/include/geninc.cfg.in",
                    "configure_source": "configure.in",
                    "substitutions": {"@ENABLE_EXECSMP@": "#define __AROSEXEC_SMP__"},
                }
            },
        }

    def write_contract(self, contract):
        self.contract_path.parent.mkdir(parents=True, exist_ok=True)
        self.contract_path.write_text(json.dumps(contract, indent=2) + "\n", encoding="utf-8")

    def run_exporter(self, *arguments):
        return subprocess.run(
            [sys.executable, str(self.exporter_path), *arguments],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_update_hashes_inputs_and_check_round_trips(self):
        result = self.run_exporter("--update")

        self.assertEqual(result.returncode, 0, result.stderr)
        updated = json.loads(self.contract_path.read_text(encoding="utf-8"))
        expected_paths = ["src/a.in", "src/b.in", "src/probe.c", "src/probe.cpp"]
        self.assertEqual([item["path"] for item in updated["inputs"]], expected_paths)
        self.assertEqual(
            [item["sha256"] for item in updated["inputs"]],
            [hashlib.sha256((self.root / name).read_bytes()).hexdigest() for name in expected_paths],
        )

        exported_bytes = self.contract_path.read_bytes()
        check = self.run_exporter("--check")
        self.assertEqual(check.returncode, 0, check.stderr)
        self.assertEqual(self.contract_path.read_bytes(), exported_bytes)

    def test_check_rejects_stale_input_without_rewriting_contract(self):
        self.write_source("src/a.in", b"changed after the recorded seal\n")
        before = self.contract_path.read_bytes()

        result = self.run_exporter("--check")

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.contract_path.read_bytes(), before)
        self.assertEqual((self.root / "src/a.in").read_bytes(), b"changed after the recorded seal\n")

    def test_update_preserves_abi_roots_and_configuration(self):
        before = json.loads(self.contract_path.read_text(encoding="utf-8"))
        self.assertEqual(before["roots"], EXPECTED_ROOTS)
        before_metadata = {key: value for key, value in before.items() if key != "inputs"}
        before_inputs = {item["path"]: item["sha256"] for item in before["inputs"]}
        self.write_source("src/a.in", b"updated source input\n")

        result = self.run_exporter("--update")

        self.assertEqual(result.returncode, 0, result.stderr)
        after = json.loads(self.contract_path.read_text(encoding="utf-8"))
        after_metadata = {key: value for key, value in after.items() if key != "inputs"}
        self.assertEqual(after_metadata, before_metadata)
        after_inputs = {item["path"]: item["sha256"] for item in after["inputs"]}
        self.assertEqual(set(after_inputs), set(before_inputs))
        self.assertNotEqual(after_inputs["src/a.in"], before_inputs["src/a.in"])
        self.assertEqual(
            after_inputs["src/a.in"],
            hashlib.sha256((self.root / "src/a.in").read_bytes()).hexdigest(),
        )
        self.assertEqual(after_inputs["src/b.in"], before_inputs["src/b.in"])

    def assert_invalid_contract_has_no_effect(self, expected_reason=None):
        before_contract = self.contract_path.read_bytes()
        source_snapshots = {
            path.relative_to(self.root): path.read_bytes()
            for path in self.root.rglob("*")
            if path.is_file() and path != self.contract_path and path != self.exporter_path
        }

        result = self.run_exporter("--update")

        self.assertNotEqual(result.returncode, 0)
        if expected_reason is not None:
            self.assertIn("native SDK export: " + expected_reason, result.stderr)
        self.assertEqual(self.contract_path.read_bytes(), before_contract)
        for relative_path, content in source_snapshots.items():
            self.assertEqual((self.root / relative_path).read_bytes(), content)

    def test_duplicate_json_key_is_rejected_without_rewriting_contract(self):
        contract = self.contract_path.read_text(encoding="utf-8")
        self.contract_path.write_text(
            contract.replace(
                '{\n  "schema":',
                '{\n  "schema": "duplicate-key-probe",\n  "schema":',
                1,
            ),
            encoding="utf-8",
        )

        self.assert_invalid_contract_has_no_effect()

    def test_missing_sdk_probe_metadata_is_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        del contract["native_sdk_link_probes"]
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect()

    def test_missing_core_linklibs_root_is_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        contract["roots"].remove("core-linklibs")
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect(
            "consumer roots must be exactly the sorted SDK roots"
        )

    def test_duplicate_root_is_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        contract["roots"].append("linklibs")
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect(
            "consumer roots must be exactly the sorted SDK roots"
        )

    def test_unexpected_root_is_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        contract["roots"].append("linklibs-extra")
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect(
            "consumer roots must be exactly the sorted SDK roots"
        )

    def test_reordered_roots_are_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        contract["roots"] = ["includes", "core-linklibs", "linklibs"]
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect(
            "consumer roots must be exactly the sorted SDK roots"
        )

    def test_unsealed_sdk_probe_is_rejected_without_rewriting_contract(self):
        contract = self.make_contract()
        contract["inputs"] = [entry for entry in contract["inputs"]
                              if entry["path"] != "src/probe.cpp"]
        self.write_contract(contract)

        self.assert_invalid_contract_has_no_effect()

    def test_duplicate_input_path_is_rejected_without_rewriting_contract(self):
        self.write_contract(self.make_contract(("src/a.in", "src/a.in")))

        self.assert_invalid_contract_has_no_effect()

    def test_recursive_contract_input_is_rejected_without_rewriting_contract(self):
        self.write_contract(self.make_contract((str(CONTRACT_RELATIVE),)))

        self.assert_invalid_contract_has_no_effect()

    def test_parent_symlink_input_is_rejected_without_touching_target(self):
        outside = self.parent / "outside-parent"
        outside.mkdir()
        target = outside / "source.in"
        target.write_bytes(b"external parent target\n")
        (self.root / "linked-parent").symlink_to(outside, target_is_directory=True)
        self.write_contract(self.make_contract(("linked-parent/source.in",)))

        self.assert_invalid_contract_has_no_effect()
        self.assertEqual(target.read_bytes(), b"external parent target\n")

    def test_leaf_symlink_input_is_rejected_without_touching_target(self):
        target = self.parent / "outside-leaf.in"
        target.write_bytes(b"external leaf target\n")
        (self.root / "linked-leaf.in").symlink_to(target)
        self.write_contract(self.make_contract(("linked-leaf.in",)))

        self.assert_invalid_contract_has_no_effect()
        self.assertEqual(target.read_bytes(), b"external leaf target\n")

    def test_parent_traversal_path_is_rejected_without_reading_or_writing_target(self):
        target = self.parent / "outside.src"
        target.write_bytes(b"outside checkout\n")
        self.write_contract(self.make_contract(("../outside.src",)))

        self.assert_invalid_contract_has_no_effect()
        self.assertEqual(target.read_bytes(), b"outside checkout\n")

    def test_absolute_path_is_rejected_without_reading_or_writing_target(self):
        target = self.parent / "absolute.src"
        target.write_bytes(b"absolute path target\n")
        self.write_contract(self.make_contract((str(target),)))

        self.assert_invalid_contract_has_no_effect()
        self.assertEqual(target.read_bytes(), b"absolute path target\n")


if __name__ == "__main__":
    unittest.main()
