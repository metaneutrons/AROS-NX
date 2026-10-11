#!/usr/bin/env python3
"""Compile and exercise the WiFi security-display helpers against SANA ABI types."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
WIFI = Path(__file__).resolve().parents[1]

TYPES_STUB = """\
#ifndef SECURITY_DISPLAY_TEST_EXEC_TYPES_H
#define SECURITY_DISPLAY_TEST_EXEC_TYPES_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef unsigned char UBYTE;
typedef void *APTR;
#endif
"""

TAGITEM_STUB = """\
#ifndef SECURITY_DISPLAY_TEST_TAGITEM_H
#define SECURITY_DISPLAY_TEST_TAGITEM_H
#define TAG_USER 0x80000000UL
#endif
"""

TEST_SOURCE = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "security_display.h"

static unsigned checks;

static void check_protection(
    const struct Sana2WirelessSecuritySummary *summary,
    const char *expected, int line)
{
    const char *actual = WifiSummaryProtection(summary);

    ++checks;
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL line %d: protection is '%s', expected '%s'\n",
            line, actual, expected);
        exit(1);
    }
}

#define CHECK_PROTECTION(summary, expected) \
    check_protection((summary), (expected), __LINE__)

static void check_capability(
    const struct Sana2WirelessCapabilities *caps, int expected, int line)
{
    int actual = WifiSummaryCapability(caps);

    ++checks;
    if (actual != expected) {
        fprintf(stderr, "FAIL line %d: summary capability is %d, expected %d\n",
            line, actual, expected);
        exit(1);
    }
}

#define CHECK_CAPABILITY(caps, expected) \
    check_capability((caps), (expected), __LINE__)

static struct Sana2WirelessCapabilities make_caps(ULONG flags)
{
    struct Sana2WirelessCapabilities caps;

    memset(&caps, 0, sizeof(caps));
    caps.SizeAvailable = sizeof(caps);
    caps.SizeSupplied = sizeof(caps);
    caps.Version = S2WIRELESS_CAPABILITIES_VERSION;
    caps.Flags = flags;
    return caps;
}

static void set_valid_offload(struct Sana2WirelessCapabilities *caps)
{
    caps->Protocols = S2WIRELESS_PROTO_WPA;
    caps->KeyManagement = S2WIRELESS_KEY_PSK;
    caps->PairwiseCiphers = S2WIRELESS_CIPHER_TKIP;
    caps->GroupCiphers = S2WIRELESS_CIPHER_CCMP;
}

static struct Sana2WirelessSecuritySummary make_summary(
    ULONG flags, ULONG protocols, ULONG key_mgmt, ULONG pairwise, ULONG group)
{
    struct Sana2WirelessSecuritySummary summary;

    summary.Size = sizeof(summary);
    summary.Version = S2WIRELESS_SUMMARY_VERSION;
    summary.Flags = flags;
    summary.Protocols = protocols;
    summary.KeyManagement = key_mgmt;
    summary.PairwiseCiphers = pairwise;
    summary.GroupCiphers = group;
    return summary;
}

static struct Sana2WirelessSecuritySummary protected_summary(
    ULONG protocol, ULONG pairwise, ULONG group)
{
    return make_summary(S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY,
        protocol, S2WIRELESS_KEY_PSK, pairwise, group);
}

static void test_capabilities(void)
{
    const ULONG unknown = 0x80000000UL;
    const ULONG summary_flag = S2WIRELESS_CAPF_SECURITY_SUMMARY;
    struct Sana2WirelessCapabilities caps;

    CHECK_CAPABILITY(NULL, 0);

    /* A metadata-only device has no offload suites, yet is summary-capable. */
    caps = make_caps(summary_flag);
    CHECK_CAPABILITY(&caps, 1);
    caps = make_caps(0);
    CHECK_CAPABILITY(&caps, 0);

    caps = make_caps(summary_flag);
    caps.SizeAvailable = sizeof(caps) - 1;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.SizeAvailable = sizeof(caps) + 1;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.SizeSupplied = sizeof(caps) - 1;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.SizeSupplied = sizeof(caps) + 1;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.Version = 0;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.Version = S2WIRELESS_CAPABILITIES_VERSION + 1;
    CHECK_CAPABILITY(&caps, 0);

    /* Every capability flag and suite mask rejects unknown bits. */
    caps = make_caps(summary_flag | unknown);
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.Protocols = unknown;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.KeyManagement = unknown;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.PairwiseCiphers = unknown;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.GroupCiphers = unknown;
    CHECK_CAPABILITY(&caps, 0);

    /* Without FOURWAY, RAW_PSK/IE_EXTENT and all suite masks are forbidden. */
    caps = make_caps(summary_flag);
    caps.Protocols = S2WIRELESS_PROTO_WPA;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.KeyManagement = S2WIRELESS_KEY_PSK;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag);
    caps.GroupCiphers = S2WIRELESS_CIPHER_TKIP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag | S2WIRELESS_CAPF_RAW_PSK);
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag | S2WIRELESS_CAPF_IE_EXTENT);
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_CONNECT_FAILED);
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(summary_flag | S2WIRELESS_CAPF_CONNECT_FAILED);
    CHECK_CAPABILITY(&caps, 1);

    /* A valid offload tuple is accepted for display gating but grants no
     * offload behavior itself. This helper returns only summary capability. */
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY);
    set_valid_offload(&caps);
    CHECK_CAPABILITY(&caps, 0);
    caps.Flags |= summary_flag;
    CHECK_CAPABILITY(&caps, 1);
    caps.Flags |= S2WIRELESS_CAPF_RAW_PSK | S2WIRELESS_CAPF_IE_EXTENT |
        S2WIRELESS_CAPF_CONNECT_FAILED;
    CHECK_CAPABILITY(&caps, 1);

    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    caps.Protocols = S2WIRELESS_PROTO_WPA | S2WIRELESS_PROTO_RSN;
    caps.KeyManagement = S2WIRELESS_KEY_PSK;
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_TKIP | S2WIRELESS_CIPHER_CCMP;
    caps.GroupCiphers = S2WIRELESS_CIPHER_TKIP | S2WIRELESS_CIPHER_CCMP;
    CHECK_CAPABILITY(&caps, 1);

    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.Protocols = 0;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.Protocols = S2WIRELESS_PROTO_RSN_SAE;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    caps.Protocols = S2WIRELESS_PROTO_RSN;
    caps.KeyManagement = S2WIRELESS_KEY_SAE;
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP;
    caps.GroupCiphers = S2WIRELESS_CIPHER_CCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    caps.Protocols = S2WIRELESS_PROTO_RSN_SAE;
    caps.KeyManagement = S2WIRELESS_KEY_SAE;
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP;
    caps.GroupCiphers = S2WIRELESS_CIPHER_CCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.KeyManagement = S2WIRELESS_KEY_EAP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.KeyManagement = S2WIRELESS_KEY_PSK | S2WIRELESS_KEY_EAP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.PairwiseCiphers = 0;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_GCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.PairwiseCiphers = S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.GroupCiphers = 0;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.GroupCiphers = S2WIRELESS_CIPHER_GCMP;
    CHECK_CAPABILITY(&caps, 0);
    caps = make_caps(S2WIRELESS_CAPF_FOURWAY | summary_flag);
    set_valid_offload(&caps);
    caps.GroupCiphers = S2WIRELESS_CIPHER_TKIP | S2WIRELESS_CIPHER_GCMP;
    CHECK_CAPABILITY(&caps, 0);
}

static void test_summaries(void)
{
    const ULONG known = S2WIRELESS_SUMF_KNOWN;
    const ULONG protected_flags =
        S2WIRELESS_SUMF_KNOWN | S2WIRELESS_SUMF_PRIVACY;
    const ULONG unknown = 0x80000000UL;
    struct Sana2WirelessSecuritySummary summary;
    ULONG pairwise, group;

    CHECK_PROTECTION(NULL, "?");

    summary = make_summary(known, 0, 0, 0, 0);
    summary.Size = 0;
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, 0, 0);
    summary.Size = sizeof(summary) - 1;
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, 0, 0);
    summary.Size = sizeof(summary) + 1;
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, 0, 0);
    summary.Version = 0;
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, 0, 0);
    summary.Version = S2WIRELESS_SUMMARY_VERSION + 1;
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(0, 0, 0, 0, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(S2WIRELESS_SUMF_PRIVACY, 0, 0, 0, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known | unknown, 0, 0, 0, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags | unknown,
        S2WIRELESS_PROTO_WPA, S2WIRELESS_KEY_PSK,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");

    /* Open is valid only with KNOWN and four zero suite masks. */
    summary = make_summary(known, 0, 0, 0, 0);
    CHECK_PROTECTION(&summary, "open");
    summary = make_summary(known, S2WIRELESS_PROTO_WPA, 0, 0, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, S2WIRELESS_KEY_PSK, 0, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, S2WIRELESS_CIPHER_TKIP, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(known, 0, 0, 0, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");

    /* All eight accepted singleton protocol/pairwise/group tuples. */
    for (pairwise = S2WIRELESS_CIPHER_TKIP;
         pairwise <= S2WIRELESS_CIPHER_CCMP; pairwise <<= 1) {
        for (group = S2WIRELESS_CIPHER_TKIP;
             group <= S2WIRELESS_CIPHER_CCMP; group <<= 1) {
            summary = protected_summary(S2WIRELESS_PROTO_WPA,
                pairwise, group);
            CHECK_PROTECTION(&summary, "WPA");
            summary = protected_summary(S2WIRELESS_PROTO_RSN,
                pairwise, group);
            CHECK_PROTECTION(&summary, "WPA2");
        }
    }

    /* Protected data must be an exact supported singleton, not a union. */
    summary = protected_summary(0, S2WIRELESS_CIPHER_TKIP,
        S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA | S2WIRELESS_PROTO_RSN,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_KEY_SAE, S2WIRELESS_CIPHER_CCMP,
        S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "WPA3");
    summary = protected_summary(S2WIRELESS_PROTO_WPA | unknown,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");

    /* WPA3 is only the exact RSN-SAE/SAE/CCMP tuple. */
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_KEY_PSK, S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_RSN,
        S2WIRELESS_KEY_SAE, S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_WPA,
        S2WIRELESS_KEY_SAE, S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags,
        S2WIRELESS_PROTO_RSN | S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_KEY_SAE, S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_KEY_PSK | S2WIRELESS_KEY_SAE,
        S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_GCMP, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_CCMP, S2WIRELESS_CIPHER_GCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_TKIP,
        S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP,
        S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        unknown, S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_RSN_SAE,
        S2WIRELESS_CIPHER_CCMP, unknown);
    CHECK_PROTECTION(&summary, "?");

    summary = make_summary(protected_flags, S2WIRELESS_PROTO_WPA, 0,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_WPA,
        S2WIRELESS_KEY_EAP, S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_WPA,
        S2WIRELESS_KEY_PSK | S2WIRELESS_KEY_EAP,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = make_summary(protected_flags, S2WIRELESS_PROTO_WPA,
        S2WIRELESS_KEY_SAE, S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");

    summary = protected_summary(S2WIRELESS_PROTO_WPA, 0,
        S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_GCMP, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP | S2WIRELESS_CIPHER_CCMP,
        S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP,
        S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP, 0);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP, S2WIRELESS_CIPHER_GCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP,
        S2WIRELESS_CIPHER_TKIP | S2WIRELESS_CIPHER_CCMP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP,
        S2WIRELESS_CIPHER_CCMP | S2WIRELESS_CIPHER_GCMP);
    CHECK_PROTECTION(&summary, "?");

    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        unknown, S2WIRELESS_CIPHER_TKIP);
    CHECK_PROTECTION(&summary, "?");
    summary = protected_summary(S2WIRELESS_PROTO_WPA,
        S2WIRELESS_CIPHER_TKIP, unknown);
    CHECK_PROTECTION(&summary, "?");
}

int main(void)
{
    test_capabilities();
    test_summaries();
    printf("PASS security-display checks=%u\n", checks);
    return 0;
}
'''


def main() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="wifi-security-display-") as temp:
        temp_dir = Path(temp)
        stubs = temp_dir / "include"
        (stubs / "exec").mkdir(parents=True)
        (stubs / "utility").mkdir()
        (stubs / "exec" / "types.h").write_text(TYPES_STUB)
        (stubs / "utility" / "tagitem.h").write_text(TAGITEM_STUB)

        for optimization in (0, 2):
            binary = temp_dir / f"test-security-display-O{optimization}"
            command = compiler + [
                "-std=c99",
                f"-O{optimization}",
                "-g",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I",
                str(stubs),
                "-I",
                str(ROOT / "compiler" / "include"),
                "-I",
                str(WIFI),
                "-x",
                "c",
                "-",
                "-o",
                str(binary),
            ]
            print(f"== O{optimization} ASan/UBSan ==", flush=True)
            subprocess.run(command, input=TEST_SOURCE, text=True, check=True)
            run_env = os.environ.copy()
            run_env["ASAN_OPTIONS"] = "halt_on_error=1"
            run_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            subprocess.run([str(binary)], env=run_env, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
