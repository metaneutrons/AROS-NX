#!/usr/bin/env python3
"""Host-only staging policy tests; ELF correctness is audited on real outputs."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("helpers", Path(__file__).with_name("make-flash-helpers.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

class Policy(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "AROS"
        self.out = Path(self.temp.name) / "stage"
        self.root.mkdir()
        self.out.mkdir()
        for name in (*helpers.FILES.values(), "Prefs/Touchscreen.info"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"FLASHDISK0P0:Devs/Networks\0FLASHDISK0P0:C/WirelessManager\0"
                             b"$VER: esp32p4wifi.device smp-riscv 1.1 (11.10.2026)\0")

    def run_overlay(self):
        with patch.object(helpers.stage.ELF, "_parse_elf", return_value=({}, None)), \
             patch.object(helpers.stage.ELF, "audit_elf", return_value=(None, None, [])), \
             patch.object(helpers.stage, "verify_volatile"):
            return helpers.overlay(self.root, self.out)

    def test_preserves_qualification(self):
        old = self.out / "ProductionQualify"
        old.write_bytes(b"retained")
        self.run_overlay()
        self.assertEqual(old.read_bytes(), b"retained")
        self.assertTrue((self.out / "WiFi.info").is_file())
        self.assertFalse((self.out / "AROS.boot").exists())

    def test_refuses_bootable(self):
        (self.out / "AROS.boot").touch()
        with self.assertRaises(ValueError): self.run_overlay()

    def test_refuses_archive(self):
        (self.out / "Prefs/Env-Archive").mkdir(parents=True)
        with self.assertRaises(ValueError): self.run_overlay()

    def test_refuses_stage_link(self):
        (self.out / "link").symlink_to(self.root)
        with self.assertRaises(ValueError): self.run_overlay()

    def test_refuses_input_link(self):
        path = self.root / "Prefs/WiFi"
        path.unlink()
        path.symlink_to(self.root / "C/WirelessManager")
        with self.assertRaises(ValueError): self.run_overlay()

    def test_requires_flash_policy(self):
        (self.root / "Prefs/WiFi").write_bytes(b"old app")
        with self.assertRaises(ValueError): self.run_overlay()
        self.assertFalse((self.out / "WiFi").exists())

    def test_rejects_stale_device_generation(self):
        (self.root / "Devs/Networks/esp32p4wifi.device").write_bytes(
            b"$VER: esp32p4wifi.device smp-riscv 1.0 (11.10.2026)")
        with self.assertRaises(ValueError): self.run_overlay()
        self.assertFalse((self.out / "WiFi").exists())

if __name__ == "__main__": unittest.main()
