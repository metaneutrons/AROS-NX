#!/usr/bin/env python3
"""Source-authoring checks, not native SDK or release qualification."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "arch/riscv-esp32p4/native-sdk"
CONTRACT = SDK / "consumer-v2.json"


class NativeSdkDeclarationTests(unittest.TestCase):
    def test_reviewed_source_seals_are_current(self) -> None:
        before = CONTRACT.read_bytes()
        subprocess.run([sys.executable, str(SDK / "export.py"), "--check"],
                       check=True, capture_output=True, text=True, timeout=30)
        self.assertEqual(CONTRACT.read_bytes(), before)

    def test_ordinary_c_and_cxx_probes_are_sealed_source_inputs(self) -> None:
        contract = json.loads(CONTRACT.read_bytes())
        self.assertEqual(contract["schema"], "aros-native-consumer-contract-v2")
        probes = contract["native_sdk_link_probes"]
        self.assertEqual(set(probes), {"c", "cxx"})
        inputs = {entry["path"]: entry["sha256"] for entry in contract["inputs"]}
        self.assertEqual(len(inputs), len(contract["inputs"]))
        for language, suffix in (("c", ".c"), ("cxx", ".cpp")):
            with self.subTest(language=language):
                probe = probes[language]
                self.assertEqual(set(probe), {"source", "libraries"})
                self.assertEqual(probe["libraries"], [])
                self.assertEqual(probe["source"],
                                 f"arch/riscv-esp32p4/native-sdk/probes/application{suffix}")
                self.assertIn(probe["source"], inputs)
                self.assertEqual(hashlib.sha256((ROOT / probe["source"]).read_bytes()).hexdigest(),
                                 inputs[probe["source"]])

    def test_exporter_positive_and_no_mutation_counterprobes(self) -> None:
        subprocess.run([sys.executable, str(SDK / "test_export.py")],
                       check=True, capture_output=True, text=True, timeout=30)

    def test_exec_headers_remain_mandatory_source_dependencies(self) -> None:
        contract = json.loads(CONTRACT.read_bytes())
        self.assertFalse(any(edge["recipe"] == "rom/exec/mmakefile.src"
                             for edge in contract["optional_meta_dependencies"]))
        recipe = (ROOT / "rom/exec/mmakefile.src").read_text()
        self.assertIn("#MM kernel-exec-includes :", recipe)
        self.assertIn("#MM     kernel-exec-includes-$(ARCH)-$(CPU)", recipe)
        self.assertIn("#MM     includes-$(ARCH)-$(CPU)", recipe)
        architecture = (ROOT / "arch/riscv-esp32p4/exec/mmakefile.src").read_text()
        self.assertIn("#MM- kernel-exec-includes-esp32p4-riscv : kernel-kernel-includes",
                      architecture)

    def test_source_preset_and_host_projections_keep_one_target_abi(self) -> None:
        config = tomllib.loads((ROOT / "aros-targets.toml").read_text())
        selected = [target for target in config["targets"]
                    if target["name"] == "esp32p4-riscv-gnu"]
        self.assertEqual(len(selected), 1, "the SDK preset must be unique")
        profile = selected[0]
        self.assertEqual(profile["toolchain_profile"], "rv32-esp32p4")
        self.assertEqual(profile["native_consumer_contract"],
                         "arch/riscv-esp32p4/native-sdk/consumer-v2.json")
        self.assertEqual(profile["arch"], "riscv32")
        self.assertEqual(profile["float_abi"], "ilp32f")
        self.assertEqual(profile["bootstrap_abi"],
                         {"flavour": "standalone", "platform_smp": True})
        self.assertEqual(profile["transpiler"]["variant"], "smp")
        self.assertEqual(profile["transpiler"]["toolchain"], "gnu")
        self.assertFalse(profile["transpiler"]["use_mmu"])
        contract = json.loads(CONTRACT.read_bytes())
        policy = json.loads((SDK / "metamake-v1.json").read_bytes())
        self.assertEqual(contract["profile"], profile["name"])
        self.assertEqual(policy["profile"], profile["name"])
        self.assertEqual(contract["roots"], ["core-linklibs", "includes", "linklibs"])
        expected = {
            "linux-x86_64": {"AROS_HOST_ARCH": "linux", "AROS_HOST_CPU": "x86_64"},
            "linux-aarch64": {"AROS_HOST_ARCH": "linux", "AROS_HOST_CPU": "aarch64"},
            "macos-aarch64": {"AROS_HOST_ARCH": "darwin", "AROS_HOST_CPU": "aarch64"},
        }
        self.assertEqual(contract["host_make_variables"], expected)
        projected = {
            entry["host"]: {binding["name"]: binding["value"]
                            for binding in entry["bindings"]}
            for entry in policy["host_environment"]
        }
        self.assertEqual(len(policy["host_environment"]), len(projected))
        self.assertEqual(projected, expected)
        self.assertEqual(
            contract["generated_make_templates"]["compiler/include/geninc.cfg"]
            ["substitutions"]["@ENABLE_EXECSMP@"], "#define __AROSEXEC_SMP__")


if __name__ == "__main__":
    unittest.main()
