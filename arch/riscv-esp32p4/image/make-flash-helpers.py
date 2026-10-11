#!/usr/bin/env python3
"""Overlay verified helpers onto a COPY of the backed-up development volume.

Never opens hardware. Refuse boot markers, symlinks and credential archives;
preserve existing qualification tools and board touch firmware. No SYS assigns.
"""
import argparse
import hashlib
import importlib.util
import re
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("wifi_stage", HERE / "make-wifi-stage.py")
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

FILES = {
    "WiFi": "Prefs/WiFi",
    "ESP32Board": "Tools/ESP32Board",
    "C/WirelessManager": "C/WirelessManager",
    "Devs/Networks/esp32p4wifi.device": "Devs/Networks/esp32p4wifi.device",
}

def overlay(aros_root, output):
    if aros_root.is_symlink() or not aros_root.is_dir():
        raise ValueError("AROS root must be a real directory")
    if output.is_symlink() or not output.is_dir():
        raise ValueError("output must be a copied development stage")
    for path in output.rglob("*"):
        if path.is_symlink():
            raise ValueError("symlink in development stage")
    if (output / "AROS.boot").exists() or (output / "Prefs/Env-Archive").exists():
        raise ValueError("refuse bootable or saved-preference volume")
    payload = {}
    for dest, source in FILES.items():
        data = stage.read_file(aros_root, source)
        elf, _ = stage.ELF._parse_elf(data)
        if dest == "WiFi":
            stage.verify_volatile(elf, data)
            for marker in (b"FLASHDISK0P0:Devs/Networks", b"FLASHDISK0P0:C/WirelessManager"):
                if marker not in data:
                    raise ValueError("WiFi lacks deterministic flash load policy")
        if dest.endswith(".device"):
            if not re.search(rb"\$VER: esp32p4wifi\.device [^\x00\r\n]* 1\.1 \(", data):
                raise ValueError("flash helper requires device generation1.1")
            _, _, issues = stage.ELF.audit_elf(data)
            if issues:
                raise ValueError("device loader audit: " + "; ".join(issues))
        payload[dest] = data
    icon = stage.read_file(aros_root, "Prefs/Touchscreen.info")
    payload["WiFi.info"] = icon
    payload["ESP32Board.info"] = icon
    payload["HELPERS.txt"] = (
        "P4 flash helpers: open this drive's WiFi or ESP32Board icon.\n"
        "CLI: FLASHDISK0P0:WiFi or FLASHDISK0P0:ESP32Board\n"
        "WiFi loads only the flash device and WirelessManager; no SD fallback.\n"
        "SYS:Prefs/WiFi is a legacy copy, not this helper entry point.\n"
        "After reboot open flash WiFi first; resident SD driver1.0 is rejected.\n"
        "TCP/IP stack and shared libraries remain on SYS:.\n"
        "No autostart, credentials, saved configuration or C6 firmware.\n"
    ).encode("ascii")
    manifest = "".join(hashlib.sha256(data).hexdigest() + "  " + name + "\n"
                       for name, data in sorted(payload.items()))
    payload["HELPERS.sha256"] = manifest.encode("ascii")
    for name, data in payload.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return payload

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aros-root", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args()
    payload = overlay(args.aros_root, args.stage)
    print("Verified %d helper files; preserved other stage files." % len(payload))

if __name__ == "__main__":
    main()
