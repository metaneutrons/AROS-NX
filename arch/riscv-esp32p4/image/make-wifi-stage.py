#!/usr/bin/env python3
"""Stage a credential-free WLAN overlay from verified RV32 build outputs.

Host files only. Never opens hardware or starts a manager. The caller supplies
a new directory; the C3 image target consumes it after validation succeeds.
"""

import argparse
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DEFAULTS = load_module("wifi_defaults", HERE / "make-wifi-defaults.py")
ELF = load_module("wifi_elf_audit", HERE.parent / "boot/audit-package.py")

# Only runtime files; no Package-Startup/autostart, build tree preferences,
# saved networks or private C6 firmware are copied.
BINARIES = (
    "Prefs/WiFi", "Prefs/Network", "Prefs/NetModules/net4.netprefs",
    "Prefs/NetModules/net6.netprefs", "C/WirelessManager",
    "Devs/Networks/esp32p4wlan.device",
    "System/Network/AROSTCP/C/AROSTCP",
    "System/Network/AROSTCP/C/dhclient",
    "Libs/posixc.library", "Libs/stdcio.library", "Libs/stdc.library",
)
IMAGES = tuple("System/Images/" + name for name in
               ("interface", "host", "wireless", "protocol"))
DB_FILES = ("netdb", "hosts", "networks", "protocols", "services",
            "inet.access", "group", "passwd", "inetd.conf")


def read_file(root, relative):
    """Reject links and non-files, including symlinked intermediate directories."""
    path = root
    for part in Path(relative).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError("symlink input refused: " + str(path))
    if not path.is_file():
        raise ValueError("missing runtime input: " + str(path))
    data = path.read_bytes()
    if not data:
        raise ValueError("empty runtime input: " + str(path))
    return data


def verify_volatile(elf, data):
    symbols = [symbol for table in elf["symbol_tables"].values()
               for symbol in table if symbol["name"] == "WifiVolatile"]
    if len(symbols) != 1:
        raise ValueError("WiFi requires one defined WifiVolatile build symbol")
    symbol = symbols[0]
    index, size = symbol["section_index"], symbol["size"]
    # STB_GLOBAL/STT_OBJECT; native AROS BOOL occupies two bytes.
    if not 0 < index < len(elf["sections"]) or size != 2 or symbol["info"] != 0x11:
        raise ValueError("WiFi volatile policy has unsupported storage")
    section = elf["sections"][index]
    offset = symbol["value"]
    if (section["type"] == ELF.SHT_NOBITS or not section["flags"] & ELF.SHF_ALLOC
            or not section["flags"] & 1  # SHF_WRITE
            or offset + size > section["size"]):
        raise ValueError("WiFi volatile policy must be initialized loadable data")
    start = section["offset"] + offset
    if int.from_bytes(data[start:start + size], "little") != 1:
        raise ValueError("WiFi must be rebuilt with WIFI_VOLATILE_DEFAULT=1")


def collect(aros_root, source_root=REPO):
    """Validate every required input before reserving or writing output."""
    for root in (aros_root, source_root):
        if root.is_symlink() or not root.is_dir():
            raise ValueError("runtime/source root must be a real directory: " + str(root))
    payload = {name: text.encode("ascii")
               for name, text in DEFAULTS.defaults().items()}
    for name in BINARIES:
        data = read_file(aros_root, name)
        elf, _ = ELF._parse_elf(data)
        if name == "Prefs/WiFi":
            verify_volatile(elf, data)
        payload[name] = data
    for name in IMAGES:
        payload[name] = read_file(aros_root, name)
    # Tool icons attach to the same-name executable; no separate manager GUI.
    icon = read_file(aros_root, "Prefs/Touchscreen.info")
    payload["Prefs/WiFi.info"] = icon
    payload["Prefs/Network.info"] = icon
    db = "workbench/network/stacks/AROSTCP/Env/db/"
    for name in DB_FILES:
        payload["Prefs/Env-Archive/AROSTCP/db/" + name] = read_file(
            source_root, db + name)
    # Stage built catalogs if present, but never import arbitrary locale files.
    catalogs = aros_root / "Locale/Catalogs"
    if catalogs.is_dir() and not catalogs.is_symlink():
        for language in sorted(catalogs.iterdir()):
            if language.is_symlink():
                raise ValueError("symlink catalog directory refused: " + str(language))
            for app in ("WiFi", "Network"):
                relative = "Locale/Catalogs/" + language.name + "/System/Prefs/" + app + ".catalog"
                if (aros_root / relative).exists():
                    payload[relative] = read_file(aros_root, relative)
    return payload


def generate(aros_root, output, source_root=REPO):
    payload = collect(aros_root, source_root)
    output.mkdir(parents=False, exist_ok=False)
    for name, content in sorted(payload.items()):
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(content)
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aros-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    payload = generate(args.aros_root, args.output)
    print("Staged %d WLAN files; no credentials, autostart or hardware action." % len(payload))


if __name__ == "__main__":
    main()
