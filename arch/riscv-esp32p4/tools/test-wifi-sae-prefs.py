#!/usr/bin/env python3
"""Sanitized host checks for the native WiFi security-profile preferences."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WIFI = ROOT / "workbench" / "prefs" / "wifi"

STUBS = {
    "exec/types.h": r'''#ifndef TEST_EXEC_TYPES_H
#define TEST_EXEC_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned char UBYTE;
typedef int BOOL;
typedef int LONG;
typedef void *APTR;
typedef const void *CONST_APTR;
typedef const char *CONST_STRPTR;
typedef intptr_t BPTR;
#define TRUE 1
#define FALSE 0
#define BNULL ((BPTR)0)
#endif
''',
    "dos/dos.h": r'''#ifndef TEST_DOS_DOS_H
#define TEST_DOS_DOS_H
#include <exec/types.h>
#define MODE_NEWFILE 1006
BPTR Open(CONST_STRPTR path, LONG mode);
LONG FPuts(BPTR file, CONST_STRPTR text);
BOOL Close(BPTR file);
#endif
''',
    "proto/dos.h": r'''#ifndef TEST_PROTO_DOS_H
#define TEST_PROTO_DOS_H
#include <dos/dos.h>
#endif
''',
}

HARNESS = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volatile_config.h"
#include <dos/dos.h>

static unsigned checks;
static char opened_path[80];
static char written[512];
static LONG opened_mode;
static BOOL fail_open, fail_puts, fail_close;

static void check_at(int condition, const char *description, int line)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL line %d: %s\n", line, description);
        exit(1);
    }
}
#define CHECK(condition) check_at((condition), #condition, __LINE__)

BPTR Open(CONST_STRPTR path, LONG mode)
{
    snprintf(opened_path, sizeof(opened_path), "%s", path);
    opened_mode = mode;
    written[0] = '\0';
    return fail_open ? BNULL : (BPTR)1;
}

LONG FPuts(BPTR file, CONST_STRPTR text)
{
    (void)file;
    snprintf(written, sizeof(written), "%s", text);
    return fail_puts ? -1 : 0;
}

BOOL Close(BPTR file)
{
    (void)file;
    return !fail_close;
}

static void check_wiped(const char *buffer, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
        CHECK(buffer[i] == '\0');
}

static void test_labels(void)
{
    CHECK(WifiSecurityProfileFromLabel("open") == WIFI_SECURITY_OPEN);
    CHECK(WifiSecurityProfileFromLabel("WPA") == WIFI_SECURITY_PSK);
    CHECK(WifiSecurityProfileFromLabel("WPA2") == WIFI_SECURITY_PSK);
    CHECK(WifiSecurityProfileFromLabel("WPA3") == WIFI_SECURITY_SAE);
    CHECK(WifiSecurityProfileFromLabel("?") == WIFI_SECURITY_UNKNOWN);
    CHECK(WifiSecurityProfileFromLabel("wpa3") == WIFI_SECURITY_UNKNOWN);
    CHECK(WifiSecurityProfileFromLabel("OPEN") == WIFI_SECURITY_UNKNOWN);
    CHECK(WifiSecurityProfileFromLabel(NULL) == WIFI_SECURITY_UNKNOWN);
}

static void test_credential_bounds(void)
{
    char key63[64];
    char key64[65];
    char control[] = "abcdefgh";

    memset(key63, 'x', 63);
    key63[63] = '\0';
    memset(key64, 'x', 64);
    key64[64] = '\0';

    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_PSK, "abcdefgh"));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_PSK, key63));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_PSK, "1234567"));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_PSK, key64));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_SAE, "x"));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_SAE, key63));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_SAE, ""));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_SAE, key64));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_SAE, "a\"#\\{}"));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_OPEN, NULL));
    CHECK(WifiValidateProfileCredential(WIFI_SECURITY_OPEN, ""));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_OPEN, "secret"));
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_UNKNOWN, "secret"));

    control[2] = '\n';
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_SAE, control));
    control[2] = '\r';
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_PSK, control));
    control[2] = 1;
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_SAE, control));
    control[2] = 127;
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_PSK, control));
    control[2] = (char)0x80;
    CHECK(!WifiValidateProfileCredential(WIFI_SECURITY_SAE, control));
}

static void test_formatting(void)
{
    char output[384];
    char key63[64];
    char key64[65];
    size_t i;

    CHECK(WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab", NULL,
        WIFI_SECURITY_OPEN));
    CHECK(strcmp(output,
        "ap_scan=1\nnetwork={\n\tssid=4c6162\n"
        "\tkey_mgmt=NONE\n}\n") == 0);
    CHECK(strstr(output, "psk=") == NULL);
    CHECK(strstr(output, "sae_password=") == NULL);

    CHECK(WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab",
        "abcdefgh", WIFI_SECURITY_PSK));
    CHECK(strstr(output, "\tpsk=\"abcdefgh\"\n") != NULL);
    CHECK(strstr(output, "key_mgmt=WPA-PSK") != NULL);
    CHECK(strstr(output, "sae_password=") == NULL);

    CHECK(WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab",
        "a\"#\\{}", WIFI_SECURITY_SAE));
    CHECK(strcmp(output,
        "ap_scan=1\nnetwork={\n\tssid=4c6162\n"
        "\tsae_password=6122235c7b7d\n\tkey_mgmt=SAE\n"
        "\tproto=RSN\n\tpairwise=CCMP\n\tgroup=CCMP\n}\n") == 0);
    CHECK(strstr(output, "psk=") == NULL);

    CHECK(WifiFormatPersistentProfileBlock(output, sizeof(output), "Lab",
        "x", WIFI_SECURITY_SAE));
    CHECK(strcmp(output,
        "network={\n\tssid=4c6162\n\tsae_password=78\n"
        "\tkey_mgmt=SAE\n\tproto=RSN\n\tpairwise=CCMP\n"
        "\tgroup=CCMP\n}\n\n") == 0);
    CHECK(strstr(output, "psk=") == NULL);

    memset(key63, 'z', 63);
    key63[63] = '\0';
    CHECK(WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab", key63,
        WIFI_SECURITY_SAE));
    CHECK(strlen(strstr(output, "sae_password=") + 13) >= 126);
    memset(key64, 'z', 64);
    key64[64] = '\0';
    memset(output, 0xa5, sizeof(output));
    CHECK(!WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab", key64,
        WIFI_SECURITY_SAE));
    check_wiped(output, sizeof(output));

    memset(output, 0xa5, sizeof(output));
    CHECK(!WifiFormatVolatileProfilePrefs(output, 17, "Lab", "abcdefgh",
        WIFI_SECURITY_PSK));
    check_wiped(output, 17);
    memset(output, 0xa5, sizeof(output));
    CHECK(!WifiFormatPersistentProfileBlock(output, 12, "Lab", "x",
        WIFI_SECURITY_SAE));
    check_wiped(output, 12);
    CHECK(!WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab",
        "a\nb", WIFI_SECURITY_SAE));
    check_wiped(output, sizeof(output));
    CHECK(!WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab",
        "1234567", WIFI_SECURITY_PSK));
    check_wiped(output, sizeof(output));
    CHECK(!WifiFormatVolatileProfilePrefs(output, sizeof(output), "Lab", NULL,
        WIFI_SECURITY_UNKNOWN));
    check_wiped(output, sizeof(output));

    for (i = 1; i < sizeof(key63); ++i)
        CHECK(key63[i] == 'z' || key63[i] == '\0');
}

static void test_merge_ssid_matcher(void)
{
    const char hex_block[] = "network={\n\tssid=4c6162\n}\n";
    const char quoted_block[] = "network={\n\tssid=\"Lab\"\n}\n";
    const char comment_only[] = "network={\n\t# ssid=4c6162\n}\n";
    const char special_block[] = "network={\n\tssid=612262\n}\n";

    CHECK(WifiConfigBlockMatchesSSID(hex_block, strlen(hex_block), "Lab"));
    CHECK(WifiConfigBlockMatchesSSID(quoted_block, strlen(quoted_block), "Lab"));
    CHECK(!WifiConfigBlockMatchesSSID(hex_block, strlen(hex_block), "La"));
    CHECK(!WifiConfigBlockMatchesSSID(comment_only, strlen(comment_only), "Lab"));
    CHECK(WifiConfigBlockMatchesSSID(special_block, strlen(special_block), "a\"b"));
    CHECK(!WifiConfigBlockMatchesSSID(hex_block, strlen(hex_block), NULL));
}

static void test_quoted_brace_foreign_block_preservation(void)
{
    const char prefs[] =
        "ctrl_interface=/var/run/wpa_supplicant\n"
        "network={\n\tssid=466f726569676e\n"
        "\tpsk=\"foreign\"}key\"\n}\n\n"
        "network={\n\tssid=4c6162\n\tpsk=\"oldpassword\"\n}\n\n";
    const char foreign[] =
        "network={\n\tssid=466f726569676e\n"
        "\tpsk=\"foreign\"}key\"\n}";
    char replacement[384];
    char merged[1024] = {0};
    const char *p = prefs;
    const char *limit = prefs + sizeof(prefs) - 1;
    const char *first = strstr(prefs, "network={");
    const char unterminated[] = "network={\n\tpsk=\"unfinished}\n";
    size_t first_length = 0;
    size_t ignored_length = 0;
    size_t used = 0;

    CHECK(!WifiConfigBlockLength(unterminated,
        sizeof(unterminated) - 1, &ignored_length));
    CHECK(first != NULL);
    CHECK(WifiConfigBlockLength(first, (size_t)(limit - first),
        &first_length));
    CHECK(first_length == strlen(foreign));
    CHECK(memcmp(first, foreign, first_length) == 0);

    CHECK(WifiFormatPersistentProfileBlock(replacement, sizeof(replacement),
        "Lab", "abcdefgh", WIFI_SECURITY_PSK));
    while (p < limit) {
        const char *start = strstr(p, "network={");
        size_t block_length;
        if (start == NULL)
            break;
        memcpy(merged + used, p, (size_t)(start - p));
        used += (size_t)(start - p);
        CHECK(WifiConfigBlockLength(start, (size_t)(limit - start),
            &block_length));
        if (!WifiConfigBlockMatchesSSID(start, block_length, "Lab")) {
            memcpy(merged + used, start, block_length);
            used += block_length;
        }
        p = start + block_length;
    }
    memcpy(merged + used, p, (size_t)(limit - p));
    used += (size_t)(limit - p);
    memcpy(merged + used, replacement, strlen(replacement));
    used += strlen(replacement);
    merged[used] = '\0';

    CHECK(strstr(merged, foreign) != NULL);
    CHECK(strstr(merged, "oldpassword") == NULL);
    CHECK(strstr(merged, "foreign\"}key") != NULL);
    CHECK(strstr(merged, "ssid=4c6162\n\tpsk=\"abcdefgh\"") != NULL);
}

static void test_write_path(void)
{
    CHECK(WifiWriteVolatileProfilePrefs("Lab", "x", WIFI_SECURITY_SAE));
    CHECK(strcmp(opened_path, "ENV:Sys/Wireless.prefs") == 0);
    CHECK(opened_mode == MODE_NEWFILE);
    CHECK(strstr(written, "sae_password=78") != NULL);
    CHECK(strstr(written, "\tkey_mgmt=SAE\n") != NULL);
    CHECK(strstr(written, "\tpsk=") == NULL);

    fail_open = TRUE;
    CHECK(!WifiWriteVolatileProfilePrefs("Lab", "x", WIFI_SECURITY_SAE));
    fail_open = FALSE;
    fail_puts = TRUE;
    CHECK(!WifiWriteVolatilePrefs("Lab", "abcdefgh", TRUE));
    fail_puts = FALSE;
    fail_close = TRUE;
    CHECK(!WifiWriteVolatileProfilePrefs("Lab", "x", WIFI_SECURITY_SAE));
    fail_close = FALSE;
}

int main(void)
{
    test_labels();
    test_credential_bounds();
    test_formatting();
    test_merge_ssid_matcher();
    test_quoted_brace_foreign_block_preservation();
    test_write_path();
    printf("Production preference checks: %u passed\n", checks);
    return 0;
}
'''


def function_body(source: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    opening = source.find("{", match.start())
    depth = 1
    position = opening + 1
    while depth and position < len(source):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
        position += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return source[opening + 1 : position - 1]


def source_contract_checks() -> int:
    main = (WIFI / "main.c").read_text(encoding="utf-8")
    worker = (WIFI / "worker.c").read_text(encoding="utf-8")
    connect_ui = function_body(main, "ConnectFunc")
    do_connect = function_body(worker, "DoConnect")
    remembered = function_body(worker, "PrefsHaveNetwork")
    start_connect = function_body(main, "StartConnect")
    merge = function_body(worker, "MergePrefs")
    checks = 0

    def require(condition: bool, description: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(description)

    require("WifiSecurityProfileFromLabel" in connect_ui,
        "selected scan labels must map through the exact production table")
    require(connect_ui.find("WIFI_SECURITY_UNKNOWN") < connect_ui.find("StartConnect(NULL)"),
        "unknown security must return before dispatching a connection")
    require(do_connect.find("WIFI_SECURITY_UNKNOWN") < do_connect.find("WifiDevice(m)"),
        "worker must reject unknown profiles before device or manager work")
    require("WifiValidateProfileCredential" in do_connect,
        "submitted PSK/SAE text must be validated before preferences are written")
    require("profile != WIFI_SECURITY_PSK" in remembered and "return FALSE" in remembered,
        "SAE must request a fresh password until safe remembered-profile parsing exists")
    require(re.search(r"PrefsHaveNetwork\(n->wn_SSID,\s*"
                      r"WifiSecurityProfileFromLabel\(", worker) is not None,
        "scan-known status must use the same exact security-profile mapping")
    require("WifiConfigBlockMatchesSSID" in merge and
            "WifiFormatPersistentProfileBlock" in merge,
        "persistent replacement must match the SSID and use the production formatter")
    require("WifiWriteVolatileProfilePrefs" in do_connect,
        "volatile SAE must use the profile-aware formatter")
    require("WifiConfigBlockLength" in merge,
        "persistent merge must use quote-aware bounded block boundaries")
    require(start_connect.find("key_length == sizeof(msg->wm_Key)") <
            start_connect.find("snprintf(msg->wm_Key"),
        "connection input must reject overlong keys before truncating")
    print(f"Native Prefs wiring checks: {checks} passed")
    return checks


def main() -> int:
    compiler = shutil.which("clang")
    if compiler is None:
        raise SystemExit("clang is required for ASan/UBSan checks")
    source_contract_checks()
    with tempfile.TemporaryDirectory(prefix="wifi-sae-prefs-") as temp:
        temp_path = Path(temp)
        include = temp_path / "include"
        for name, contents in STUBS.items():
            path = include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        harness = temp_path / "wifi-sae-prefs.c"
        harness.write_text(HARNESS, encoding="utf-8")
        for optimization in ("-O0", "-O2"):
            executable = temp_path / f"wifi-sae-prefs-{optimization[2:]}"
            command = [
                compiler,
                "-std=c99",
                optimization,
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                f"-I{include}",
                f"-I{WIFI}",
                str(harness),
                str(WIFI / "volatile_config.c"),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True)
            environment = os.environ.copy()
            environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
            environment["UBSAN_OPTIONS"] = "halt_on_error=1"
            result = subprocess.run(
                [str(executable)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                env=environment,
            )
            print(f"{optimization} ASan/UBSan: {result.stdout.strip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
