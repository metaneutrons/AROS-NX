#!/usr/bin/env python3
"""Static checks for the external DHCP service (services/dhcp, db/services.d/dhcp).

Upstream removed the DHCP client from the AROSTCP stack (f93780cc5a): the stack
starts services/dhcp/dhcpd from the manifest db/services.d/dhcp, and dhcpd runs
the ISC dhclient. These checks read the sources and compile nothing. They
replace test-dhclient-paths.py, which tested the removed dhclient_path_changed()
callback and the DHCLIENT config line. They do not run the stack, dhcpd or any
filesystem access on AROS.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys
import unittest

sys.dont_write_bytecode = True

AROSTCP = Path(__file__).resolve().parents[1]
ROOT = AROSTCP.parents[3]
MANIFEST = AROSTCP / "Env/db/services.d/dhcp"
DHCPD_C = AROSTCP / "services/dhcp/dhcpd.c"
DHCPD_MM = AROSTCP / "services/dhcp/mmakefile.src"
MAIN_C = AROSTCP / "bsdsocket/kern/amiga_main.c"
CONFIG_VAR_C = AROSTCP / "bsdsocket/kern/config_var.c"
SVCMGR_C = AROSTCP / "bsdsocket/kern/amiga_svcmgr.c"
CLIENT_MM = ROOT / "workbench/network/common/dhcp/client/mmakefile.src"
DEFAULTS = ROOT / "arch/riscv-esp32p4/image/make-wifi-defaults.py"


def manifest_keys() -> dict[str, str]:
    keys = {}
    for line in MANIFEST.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        keys[key.strip().lower()] = value.strip()
    return keys


def install_path(makefile: Path, progname_re: str) -> str:
    """SYS: path of a %build_prog target from its targetdir and progname."""
    text = makefile.read_text(encoding="ascii")
    target = re.search(r'targetdir="?\$\(AROS_SYSTEM\)/([^"\s\\]+)', text)
    name = re.search(progname_re, text)
    assert target and name, makefile
    return "SYS:System/" + target.group(1) + "/" + name.group(1)


class DhcpServiceTests(unittest.TestCase):
    def test_manifest_is_valid_for_the_service_manager(self):
        keys = manifest_keys()
        self.assertTrue(keys.get("path"), "svc_apply_cfg() needs Path=")
        self.assertIn(keys.get("enabled", "yes").lower(), ("yes", "true", "1", "on"))
        self.assertIn(keys.get("policy", "signal").lower()[:1], ("s", "r", "i"))
        # svc_apply_cfg() accepts 0..31 and maps it to 1 << n; 12 is CTRL-C.
        self.assertTrue(0 <= int(keys["stopsig"]) < 32)
        self.assertIn("services.d", SVCMGR_C.read_text(encoding="utf-8"))

    def test_manifest_path_is_where_dhcpd_is_installed(self):
        self.assertEqual(install_path(DHCPD_MM, r"progname=(\w+)"),
                         manifest_keys()["path"])
        self.assertEqual("SYS:System/Network/Services/dhcpd",
                         manifest_keys()["path"])

    def test_dhcpd_resolves_its_own_client(self):
        source = DHCPD_C.read_text(encoding="utf-8")
        self.assertIn('GetVar("AROSTCP/dhcp/client"', source)
        fallback = re.search(r'#define DHCLIENT_FALLBACK\s+"([^"]+)"', source)
        self.assertIsNotNone(fallback)
        self.assertEqual(install_path(CLIENT_MM, r"progname=(\w+)"),
                         fallback.group(1))

    def test_stack_does_not_manage_the_client_any_more(self):
        main = MAIN_C.read_text(encoding="utf-8")
        self.assertNotIn("dhclient_path_changed", main)
        config = CONFIG_VAR_C.read_text(encoding="utf-8")
        slot = re.search(r"VAR_STRP, VF_RCONF, NULL, &dhclient_name, (\w+) \}", config)
        self.assertIsNotNone(slot, "legacy DHCLIENT slot changed; revisit this test")
        self.assertEqual("NULL", slot.group(1), "the slot must have no callback")

    def test_esp32p4_overlay_does_not_write_the_removed_line(self):
        text = DEFAULTS.read_text(encoding="utf-8")
        self.assertNotIn('"DHCLIENT=', text)


if __name__ == "__main__":
    unittest.main()
