#!/usr/bin/env python3
"""Source-policy checks for LLVM SDK declarations, not SDK qualification."""

import json
from pathlib import Path
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[2]
LLVM_VERSION = "23.1.0"
LLVM_RUNTIME_STYLE = "umbrella"


class LlvmSdkDeclarationTests(unittest.TestCase):
    def test_source_defaults_and_fallbacks_select_llvm_23(self) -> None:
        self.assertEqual((ROOT / "config/llvm_def").read_text().strip(), LLVM_VERSION)
        fallbacks = (
            ("tools/crosstools/llvm.cfg", "LLVM_VERSION:=23.1.0"),
            ("workbench/libs/llvm/mmakefile.src", "OPT_LLVM := 23.1.0"),
            (
                "workbench/libs/mesa/libgalliumvm/mmakefile.src",
                "OPT_LLVM := 23.1.0",
            ),
            (
                "workbench/libs/mesa/libllvmpipe/mmakefile.src",
                "OPT_LLVM := 23.1.0",
            ),
        )
        for relative, expected in fallbacks:
            with self.subTest(path=relative):
                self.assertIn(expected, (ROOT / relative).read_text())

        configure_source = (ROOT / "configure.in").read_text()
        generated_configure = (ROOT / "configure").read_text()
        self.assertIn('default_llvm_version="23.1.0"', configure_source)
        self.assertIn('default_llvm_version="23.1.0"', generated_configure)
        self.assertEqual(
            configure_source.count('default_llvm_version="23.1.0"'),
            generated_configure.count('default_llvm_version="23.1.0"'),
        )

    def test_llvm_sdk_profiles_select_llvm_23_umbrella_runtimes(self) -> None:
        targets = tomllib.loads((ROOT / "aros-targets.toml").read_text())["targets"]
        profiles = {
            target["name"]: target
            for target in targets
            if target.get("sdk_consumer_contract")
        }
        self.assertEqual(set(profiles), {"arm-raspi", "pc-x86_64", "rpi-aarch64"})

        for name, profile in profiles.items():
            with self.subTest(profile=name):
                self.assertEqual(profile["transpiler"]["toolchain"], "llvm")
                contract = ROOT / profile["sdk_consumer_contract"]
                policy_path = contract.with_name("metamake-v1.json")
                policy = json.loads(policy_path.read_bytes())
                snapshots = {
                    snapshot["source"]: snapshot["text"]
                    for snapshot in policy["global_snapshots"]
                }
                target = snapshots["config/target.cfg.in"]
                self.assertIn("AROS_TOOLCHAIN := llvm\n", target)
                self.assertIn("KERNEL_TOOLCHAIN := llvm\n", target)
                self.assertIn(f"TARGET_LLVM_VER := {LLVM_VERSION}\n", target)
                self.assertIn(
                    f"TARGET_LLVM_RUNTIMES_STYLE := {LLVM_RUNTIME_STYLE}\n", target
                )

    def test_esp32p4_native_sdk_remains_gnu(self) -> None:
        targets = tomllib.loads((ROOT / "aros-targets.toml").read_text())["targets"]
        profiles = [target for target in targets if target["name"] == "esp32p4-riscv-gnu"]
        self.assertEqual(len(profiles), 1)
        profile = profiles[0]
        self.assertEqual(profile["transpiler"]["toolchain"], "gnu")

        policy_path = ROOT / "arch/riscv-esp32p4/native-sdk/metamake-v1.json"
        policy = json.loads(policy_path.read_bytes())
        snapshots = {
            snapshot["source"]: snapshot["text"]
            for snapshot in policy["global_snapshots"]
        }
        target = snapshots["config/target.cfg.in"]
        self.assertIn("AROS_TOOLCHAIN := gnu\n", target)
        self.assertIn("KERNEL_TOOLCHAIN := gnu\n", target)
        self.assertIn(f"TARGET_LLVM_VER := {LLVM_VERSION}\n", target)
        self.assertIn(
            f"TARGET_LLVM_RUNTIMES_STYLE := {LLVM_RUNTIME_STYLE}\n", target
        )


if __name__ == "__main__":
    unittest.main()
