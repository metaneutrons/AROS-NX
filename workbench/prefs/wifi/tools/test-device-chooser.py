#!/usr/bin/env python3
"""Exercise the production scan-to-device-list repair without hardware."""

import os
import re
import subprocess
import tempfile
from pathlib import Path

WIFI = Path(__file__).resolve().parents[1]
worker = (WIFI / "worker.c").read_text()
helper = re.search(r"static void RememberScannedDevice\(.*?\n}\n", worker, re.S)
assert helper is not None
scan = worker[worker.index("static void DoScan("):]
scan_io = scan[scan.index("dev.wd_Req->ios2_Req.io_Command = S2_GETNETWORKS;"):]
assert re.search(r"if \(DoIO\(\(struct IORequest \*\)dev.wd_Req\) == 0\)"
                 r"\s*\{[^{}]*RememberScannedDevice\(m\);", scan_io, re.S)
main = (WIFI / "main.c").read_text()
assert "case WCMD_SCAN:\n            RefreshDeviceChooser();" in main
assert "SetAttrs(devcycle, MUIA_NoNotify, TRUE," in main

prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef int32_t LONG;
#define WIFI_MAX_DEVS 8
#define WIFI_DEV_MAX 64
#define Stricmp strcasecmp
struct WifiMsg {
    char wm_Device[WIFI_DEV_MAX];
    LONG wm_Unit, wm_DeviceCount;
    char wm_Devices[WIFI_MAX_DEVS][WIFI_DEV_MAX];
    LONG wm_DeviceUnits[WIFI_MAX_DEVS];
};
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d\n", __LINE__); exit(1); } } while (0)
static unsigned checks;
'''
tests = r'''
int main(void) {
    struct WifiMsg m = {0}, before;
    RememberScannedDevice(&m); CHECK(m.wm_DeviceCount == 0);
    strcpy(m.wm_Device, "DEVS:Networks/esp32p4wifi.device");
    RememberScannedDevice(&m);
    CHECK(m.wm_DeviceCount == 1);
    CHECK(strcmp(m.wm_Devices[0], m.wm_Device) == 0);
    CHECK(m.wm_DeviceUnits[0] == 0);
    RememberScannedDevice(&m); CHECK(m.wm_DeviceCount == 1);
    strcpy(m.wm_Device, "devs:networks/ESP32P4WIFI.device");
    RememberScannedDevice(&m); CHECK(m.wm_DeviceCount == 1);
    m.wm_Unit = 1; RememberScannedDevice(&m);
    CHECK(m.wm_DeviceCount == 2); CHECK(m.wm_DeviceUnits[1] == 1);
    for (int i = 2; i < WIFI_MAX_DEVS; i++) {
        m.wm_Unit = i; RememberScannedDevice(&m);
        CHECK(m.wm_DeviceCount == i + 1);
    }
    m.wm_Unit = WIFI_MAX_DEVS; before = m;
    RememberScannedDevice(&m); CHECK(memcmp(&m, &before, sizeof(m)) == 0);
    m.wm_DeviceCount = -1; before = m;
    RememberScannedDevice(&m); CHECK(memcmp(&m, &before, sizeof(m)) == 0);
    m.wm_DeviceCount = WIFI_MAX_DEVS + 1; before = m;
    RememberScannedDevice(&m); CHECK(memcmp(&m, &before, sizeof(m)) == 0);
    m.wm_DeviceCount = 0; m.wm_Unit = -1; before = m;
    RememberScannedDevice(&m); CHECK(memcmp(&m, &before, sizeof(m)) == 0);
    printf("PASS device chooser checks=%u\n", checks);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-chooser-") as directory:
    source = Path(directory) / "test.c"
    source.write_text(prefix + helper.group() + tests)
    for optimization in (0, 2):
        binary = Path(directory) / f"test-O{optimization}"
        subprocess.run([os.environ.get("CC", "cc"), f"-O{optimization}",
                        "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(source),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
