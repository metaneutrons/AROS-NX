#!/usr/bin/env python3
"""Generate a credential-free preferences overlay, never a live-device image.

The caller must supply a new output directory. Image staging, binary
dependencies, single-manager startup and hardware acceptance are separate gates.
"""

import argparse
from pathlib import Path


def defaults():
    """Fixed native device/unit and DHCP; no private configuration inputs."""
    root = "Prefs/Env-Archive/"
    return {
        root + "AROSTCP/Config": "ENV:AROSTCP/db",
        root + "AROSTCP/AutoRun": "False",
        root + "AROSTCP/WirelessAutoRun": "False",
        root + "AROSTCP/WirelessDevice":
            "DEVS:Networks/esp32p4wlan.device UNIT 0",
        root + "AROSTCP/db/interfaces":
            "wlan0 DEV=DEVS:Networks/esp32p4wlan.device UNIT=0 "
            "IP=DHCP UP DEFER\n",
        root + "AROSTCP/db/general.config":
            "USELOOPBACK=YES\nDEBUGSANA=NO\nUSENS=SECOND\nGATEWAY=NO\n"
            "HOSTNAME=arosp4\nLOG FILTERFILE=5\nGUI PANEL=MUI\nOPENGUI=NO\n"
            "DHCLIENT=SYS:System/Network/AROSTCP/C/dhclient\n",
        root + "PATH_DHCLIENT_DB": "/T/dhclient.leases",
        root + "PATH_DHCLIENT_PID": "/T/dhclient.pid",
        root + "PATH_DHCLIENT_CONF": "ENV:AROSTCP/db/dhclient.conf",
        root + "AROSTCP/db/dhclient.conf":
            "# No fixed address or private network configuration.\n",
        # CONFIG_NO_CONFIG_WRITE omits update_config from the parser table.
        root + "Sys/Wireless.prefs": "ap_scan=1\n",
    }


def generate(output):
    # mkdir is the exclusive reservation, including refusal of existing
    # directories/symlinks. Never merge into a user's saved preferences.
    output.mkdir(parents=False, exist_ok=False)
    for name, content in defaults().items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new overlay directory")
    args = parser.parse_args()
    generate(args.output)
    print("Generated credential-free defaults; no autostart or hardware action.")


if __name__ == "__main__":
    main()
