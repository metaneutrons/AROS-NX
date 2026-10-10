#!/usr/bin/env python3
"""Host checks for the generated overlay, not AROS parser/runtime acceptance."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "wifi_defaults", Path(__file__).with_name("make-wifi-defaults.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DefaultsTest(unittest.TestCase):
    def test_outputs_and_repeatability(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "one", Path(tmp) / "two"
            module.generate(first)
            module.generate(second)
            for name, content in module.defaults().items():
                self.assertEqual((first / name).read_bytes(), content.encode("ascii"))
                self.assertEqual((first / name).read_bytes(), (second / name).read_bytes())
            self.assertEqual({str(p.relative_to(first)) for p in first.rglob("*")
                              if p.is_file()}, set(module.defaults()))

    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "saved"
            output.mkdir()
            saved = output / "user.prefs"
            saved.write_bytes(b"private user state")
            with self.assertRaises(FileExistsError):
                module.generate(output)
            self.assertEqual(saved.read_bytes(), b"private user state")
            self.assertEqual(list(output.iterdir()), [saved])

    def test_symlink_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            real = Path(tmp) / "real"
            real.mkdir()
            alias = Path(tmp) / "alias"
            alias.symlink_to(real, target_is_directory=True)
            with self.assertRaises(FileExistsError):
                module.generate(alias)
            self.assertEqual(list(real.iterdir()), [])

    def test_no_secrets_no_startup(self):
        values = module.defaults()
        self.assertEqual(values["Prefs/Env-Archive/Sys/Wireless.prefs"],
                         "ap_scan=1\n")
        self.assertFalse(any(name.startswith("S/") for name in values))
        for name in ("AutoRun", "WirelessAutoRun"):
            self.assertEqual(values["Prefs/Env-Archive/AROSTCP/" + name], "False")
        interface = values["Prefs/Env-Archive/AROSTCP/db/interfaces"]
        self.assertIn("UNIT=0 IP=DHCP UP DEFER", interface)
        self.assertNotIn("network={", "".join(values.values()))
        self.assertEqual(values["Prefs/Env-Archive/PATH_DHCLIENT_DB"],
                         "/T/dhclient.leases")

    def test_stack_package_and_dhcp_owned_configuration(self):
        values = module.defaults()
        self.assertEqual(values["Prefs/Env-Archive/Sys/Packages/AROSTCP"],
                         "SYS:System/Network/AROSTCP")
        for name in ("netdb-myhost", "static-routes"):
            lines = values["Prefs/Env-Archive/AROSTCP/db/" + name].splitlines()
            self.assertTrue(lines)
            self.assertTrue(all(line.startswith(";") for line in lines))
        self.assertNotIn("192.168.", "".join(values.values()))


if __name__ == "__main__":
    unittest.main()
