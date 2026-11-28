#!/usr/bin/env python3
"""Compile and exercise the public SANA-II FullMAC contract predicates."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]

TYPES_STUB = """\
#ifndef FULLMAC_CONTRACT_TEST_EXEC_TYPES_H
#define FULLMAC_CONTRACT_TEST_EXEC_TYPES_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef unsigned char UBYTE;
typedef void *APTR;
#endif
"""

TAGITEM_STUB = """\
#ifndef FULLMAC_CONTRACT_TEST_TAGITEM_H
#define FULLMAC_CONTRACT_TEST_TAGITEM_H
#define TAG_USER 0x80000000UL
#endif
"""

TEST_SOURCE = r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <devices/sana2wireless_fullmac.h>

#ifndef __has_feature
#define __has_feature(feature) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define FULLMAC_TEST_ASAN 1
#else
#define FULLMAC_TEST_ASAN 0
#endif

#define STATIC_ASSERT(name, expression) \
    typedef char static_assert_##name[(expression) ? 1 : -1]

/* These v1 ABI values predate the FullMAC declarations and must not move. */
STATIC_ASSERT(v1_capabilities_size, sizeof(struct Sana2WirelessCapabilities) == 32);
STATIC_ASSERT(v1_capabilities_size_available_offset,
    offsetof(struct Sana2WirelessCapabilities, SizeAvailable) == 0);
STATIC_ASSERT(v1_capabilities_size_supplied_offset,
    offsetof(struct Sana2WirelessCapabilities, SizeSupplied) == 4);
STATIC_ASSERT(v1_capabilities_version_offset,
    offsetof(struct Sana2WirelessCapabilities, Version) == 8);
STATIC_ASSERT(v1_capabilities_flags_offset,
    offsetof(struct Sana2WirelessCapabilities, Flags) == 12);
STATIC_ASSERT(v1_capabilities_protocols_offset,
    offsetof(struct Sana2WirelessCapabilities, Protocols) == 16);
STATIC_ASSERT(v1_capabilities_key_management_offset,
    offsetof(struct Sana2WirelessCapabilities, KeyManagement) == 20);
STATIC_ASSERT(v1_capabilities_pairwise_offset,
    offsetof(struct Sana2WirelessCapabilities, PairwiseCiphers) == 24);
STATIC_ASSERT(v1_capabilities_group_offset,
    offsetof(struct Sana2WirelessCapabilities, GroupCiphers) == 28);
STATIC_ASSERT(v1_summary_size, sizeof(struct Sana2WirelessSecuritySummary) == 28);
STATIC_ASSERT(v1_summary_size_offset,
    offsetof(struct Sana2WirelessSecuritySummary, Size) == 0);
STATIC_ASSERT(v1_summary_version_offset,
    offsetof(struct Sana2WirelessSecuritySummary, Version) == 4);
STATIC_ASSERT(v1_summary_flags_offset,
    offsetof(struct Sana2WirelessSecuritySummary, Flags) == 8);
STATIC_ASSERT(v1_summary_protocols_offset,
    offsetof(struct Sana2WirelessSecuritySummary, Protocols) == 12);
STATIC_ASSERT(v1_summary_key_management_offset,
    offsetof(struct Sana2WirelessSecuritySummary, KeyManagement) == 16);
STATIC_ASSERT(v1_summary_pairwise_offset,
    offsetof(struct Sana2WirelessSecuritySummary, PairwiseCiphers) == 20);
STATIC_ASSERT(v1_summary_group_offset,
    offsetof(struct Sana2WirelessSecuritySummary, GroupCiphers) == 24);

static unsigned checks;
static unsigned accepted;
static unsigned rejected;
static unsigned unexpected;

static void expect_result(int actual, int wanted, const char *label, int line)
{
    ++checks;
    if (wanted)
        ++accepted;
    else
        ++rejected;
    if (actual != wanted) {
        ++unexpected;
        fprintf(stderr, "FAIL line %d: %s returned %d, expected %d\n",
            line, label, actual, wanted);
    }
}

static void expect_value(unsigned long actual, unsigned long wanted,
    const char *label, int line)
{
    ++checks;
    if (actual != wanted) {
        ++unexpected;
        fprintf(stderr, "FAIL line %d: %s is 0x%lx, expected 0x%lx\n",
            line, label, actual, wanted);
    }
}

#define EXPECT_VALID(expression, label) \
    expect_result((expression), 1, (label), __LINE__)
#define EXPECT_INVALID(expression, label) \
    expect_result((expression), 0, (label), __LINE__)
#define EXPECT_VALUE(actual, wanted, label) \
    expect_value((unsigned long)(actual), (unsigned long)(wanted), \
        (label), __LINE__)

static struct Sana2FullMACCapabilities make_caps(
    ULONG profiles, ULONG pwe_methods, ULONG flags)
{
    struct Sana2FullMACCapabilities caps;

    memset(&caps, 0, sizeof(caps));
    caps.SizeAvailable = (ULONG)sizeof(caps);
    caps.SizeSupplied = (ULONG)sizeof(caps);
    caps.Version = S2FULLMAC_VERSION;
    caps.Flags = flags;
    caps.Profiles = profiles;
    caps.SAEPweMethods = pwe_methods;
    return caps;
}

static struct Sana2FullMACAssociation make_selection(
    ULONG profile, ULONG pmf, ULONG pwe, ULONG flags)
{
    struct Sana2FullMACAssociation selection;

    memset(&selection, 0, sizeof(selection));
    selection.Size = (ULONG)sizeof(selection);
    selection.Version = S2FULLMAC_VERSION;
    selection.Profile = profile;
    selection.Flags = flags;
    selection.PMFPolicy = pmf;
    selection.SAEPweMethod = pwe;
    return selection;
}

static struct Sana2WirelessCredential make_credential(
    ULONG kind, const UBYTE *data, ULONG length)
{
    struct Sana2WirelessCredential credential;

    credential.Size = (ULONG)sizeof(credential);
    credential.Version = S2WIRELESS_CREDENTIAL_VERSION;
    credential.Kind = kind;
    credential.Length = length;
    credential.Data = data;
    return credential;
}

static int association_valid(
    const struct Sana2FullMACCapabilities *caps,
    const struct Sana2FullMACAssociation *selection,
    const struct Sana2WirelessCredential *credential)
{
    return Sana2FullMACAssociationValid(caps, selection, credential);
}

static void test_v1_regression_contract(void)
{
    EXPECT_VALUE(S2_GETSIGNALQUALITY, 0xc010, "S2_GETSIGNALQUALITY");
    EXPECT_VALUE(S2_GETNETWORKS, 0xc011, "S2_GETNETWORKS");
    EXPECT_VALUE(S2_SETOPTIONS, 0xc012, "S2_SETOPTIONS");
    EXPECT_VALUE(S2_SETKEY, 0xc013, "S2_SETKEY");
    EXPECT_VALUE(S2_GETNETWORKINFO, 0xc014, "S2_GETNETWORKINFO");
    EXPECT_VALUE(S2_READMGMT, 0xc015, "S2_READMGMT");
    EXPECT_VALUE(S2_WRITEMGMT, 0xc016, "S2_WRITEMGMT");
    EXPECT_VALUE(S2_GETRADIOBANDS, 0xc017, "S2_GETRADIOBANDS");
    EXPECT_VALUE(S2_GETCRYPTTYPES, 0xc018, "S2_GETCRYPTTYPES");
    EXPECT_VALUE(S2_GETWIRELESSCAPABILITIES, 0xc019,
        "S2_GETWIRELESSCAPABILITIES");
    EXPECT_VALUE(S2_GETFULLMACCAPABILITIES, 0xc01a,
        "S2_GETFULLMACCAPABILITIES");
    EXPECT_VALUE(S2INFO_SSID, 0x80000000UL, "S2INFO_SSID");
    EXPECT_VALUE(S2INFO_BSSID, 0x80000001UL, "S2INFO_BSSID");
    EXPECT_VALUE(S2INFO_AuthTypes, 0x80000002UL, "S2INFO_AuthTypes");
    EXPECT_VALUE(S2INFO_AssocID, 0x80000003UL, "S2INFO_AssocID");
    EXPECT_VALUE(S2INFO_Encryption, 0x80000004UL, "S2INFO_Encryption");
    EXPECT_VALUE(S2INFO_PortType, 0x80000005UL, "S2INFO_PortType");
    EXPECT_VALUE(S2INFO_BeaconInterval, 0x80000006UL,
        "S2INFO_BeaconInterval");
    EXPECT_VALUE(S2INFO_Channel, 0x80000007UL, "S2INFO_Channel");
    EXPECT_VALUE(S2INFO_Signal, 0x80000008UL, "S2INFO_Signal");
    EXPECT_VALUE(S2INFO_Noise, 0x80000009UL, "S2INFO_Noise");
    EXPECT_VALUE(S2INFO_Capabilities, 0x8000000aUL,
        "S2INFO_Capabilities");
    EXPECT_VALUE(S2INFO_InfoElements, 0x8000000bUL,
        "S2INFO_InfoElements");
    EXPECT_VALUE(S2INFO_WPAInfo, 0x8000000cUL, "S2INFO_WPAInfo");
    EXPECT_VALUE(S2INFO_Band, 0x8000000dUL, "S2INFO_Band");
    EXPECT_VALUE(S2INFO_DefaultKeyNo, 0x8000000eUL,
        "S2INFO_DefaultKeyNo");
    EXPECT_VALUE(S2INFO_Passphrase, 0x8000000fUL,
        "S2INFO_Passphrase");
    EXPECT_VALUE(S2INFO_Disassociate, 0x80000010UL,
        "S2INFO_Disassociate");
    EXPECT_VALUE(S2INFO_SecuritySummary, 0x80000011UL,
        "S2INFO_SecuritySummary");
    EXPECT_VALUE(S2INFO_PSK, 0x80000012UL, "S2INFO_PSK");
    EXPECT_VALUE(S2INFO_WPAInfoLength, 0x80000013UL,
        "S2INFO_WPAInfoLength");
    EXPECT_VALUE(S2INFO_AssociationCookie, 0x80000014UL,
        "S2INFO_AssociationCookie");
    EXPECT_VALUE(S2INFO_FullMACAssociation, 0x80000015UL,
        "S2INFO_FullMACAssociation");
    EXPECT_VALUE(S2INFO_WirelessCredential, 0x80000016UL,
        "S2INFO_WirelessCredential");
    EXPECT_VALUE(sizeof(struct Sana2WirelessCapabilities), 32,
        "v1 capability size");
    EXPECT_VALUE(offsetof(struct Sana2WirelessCapabilities, GroupCiphers), 28,
        "v1 GroupCiphers offset");
    EXPECT_VALUE(S2WIRELESS_CAPABILITIES_VERSION, 1,
        "v1 capability version");
    EXPECT_VALUE(S2WIRELESS_CAPF_CONNECT_FAILED, 1UL << 4,
        "v1 connect-failed event flag");
    EXPECT_VALUE(sizeof(struct Sana2WirelessSecuritySummary), 28,
        "v1 security summary size");
    EXPECT_VALUE(offsetof(struct Sana2WirelessSecuritySummary, GroupCiphers), 24,
        "v1 summary GroupCiphers offset");
    EXPECT_VALUE(S2WIRELESS_SUMMARY_VERSION, 1, "v1 summary version");
}

static void test_capabilities_validity(void)
{
    const ULONG all_profiles = S2FULLMAC_PROFILE_OPEN |
        S2FULLMAC_PROFILE_WPA2_CCMP | S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    const ULONG all_pwe = S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
        S2FULLMAC_SAE_PWE_H2E | S2FULLMAC_SAE_PWE_BOTH;
    const ULONG known_flags = S2FULLMAC_CAPF_DIAGNOSTIC_RISK;
    const ULONG unknown = 0x80000000UL;
    struct Sana2FullMACCapabilities caps;

    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(NULL), "null capabilities");
    caps = make_caps(0, 0, 0);
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "empty profile set");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    EXPECT_VALID(Sana2FullMACCapabilitiesValid(&caps), "open-only capabilities");
    caps = make_caps(S2FULLMAC_PROFILE_WPA2_CCMP, 0, 0);
    EXPECT_VALID(Sana2FullMACCapabilitiesValid(&caps), "WPA2-only capabilities");
    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_HUNT_AND_PECK, 0);
    EXPECT_VALID(Sana2FullMACCapabilitiesValid(&caps), "SAE one PWE capability");
    caps = make_caps(all_profiles, all_pwe, known_flags);
    EXPECT_VALID(Sana2FullMACCapabilitiesValid(&caps), "all known capabilities");

    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.SizeAvailable = (ULONG)sizeof(caps) - 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "short SizeAvailable");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.SizeAvailable = (ULONG)sizeof(caps) + 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "oversized SizeAvailable");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.SizeSupplied = (ULONG)sizeof(caps) - 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "short SizeSupplied");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.SizeSupplied = (ULONG)sizeof(caps) + 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "oversized SizeSupplied");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.Version = 0;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "zero capabilities version");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.Version = S2FULLMAC_VERSION + 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "unknown capabilities version");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.Flags = known_flags | unknown;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "unknown capability flag");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN | unknown, 0, 0);
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "unknown profile mask bit");
    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF, all_pwe | unknown, 0);
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "unknown PWE mask bit");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.Reserved[0] = 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "reserved capability word 0");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    caps.Reserved[1] = 1;
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "reserved capability word 1");
    caps = make_caps(S2FULLMAC_PROFILE_OPEN, S2FULLMAC_SAE_PWE_H2E, 0);
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "PWE without SAE profile");
    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF, 0, 0);
    EXPECT_INVALID(Sana2FullMACCapabilitiesValid(&caps), "SAE profile without PWE");
}

static void test_positive_associations(void)
{
    static const UBYTE passphrase_8[] = "password";
    static const UBYTE passphrase_63[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0";
    static const UBYTE sae_1[] = { 'x' };
    static const UBYTE sae_63[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0";
    UBYTE psk[S2WIRELESS_PSK_SIZE];
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    const ULONG pwe_methods[] = {
        S2FULLMAC_SAE_PWE_HUNT_AND_PECK,
        S2FULLMAC_SAE_PWE_H2E,
        S2FULLMAC_SAE_PWE_BOTH
    };
    unsigned i;

    for (i = 0; i < sizeof(psk); ++i)
        psk[i] = (UBYTE)(i * 7u);
    psk[3] = 0;

    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0, 0);
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_VALID(association_valid(&caps, &selection, NULL), "open without credential");

    caps = make_caps(S2FULLMAC_PROFILE_WPA2_CCMP, 0, 0);
    selection = make_selection(S2FULLMAC_PROFILE_WPA2_CCMP,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        passphrase_8, 8);
    EXPECT_VALID(association_valid(&caps, &selection, &credential),
        "WPA2 passphrase length 8");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        passphrase_63, 63);
    EXPECT_VALID(association_valid(&caps, &selection, &credential),
        "WPA2 passphrase length 63");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK, psk,
        S2WIRELESS_PSK_SIZE);
    EXPECT_VALID(association_valid(&caps, &selection, &credential),
        "WPA2 binary PSK length 32");

    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_HUNT_AND_PECK | S2FULLMAC_SAE_PWE_H2E |
            S2FULLMAC_SAE_PWE_BOTH, 0);
    selection = make_selection(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_PMF_REQUIRED, 0, 0);
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD, sae_1, 1);
    for (i = 0; i < sizeof(pwe_methods) / sizeof(pwe_methods[0]); ++i) {
        selection.SAEPweMethod = pwe_methods[i];
        EXPECT_VALID(association_valid(&caps, &selection, &credential),
            "SAE password length 1 with one advertised PWE mode");
    }
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD, sae_63, 63);
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_H2E;
    EXPECT_VALID(association_valid(&caps, &selection, &credential),
        "SAE password length 63 and required PMF");

    caps = make_caps(S2FULLMAC_PROFILE_OPEN | S2FULLMAC_PROFILE_WPA2_CCMP |
        S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_H2E, S2FULLMAC_CAPF_DIAGNOSTIC_RISK);
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0,
        S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK);
    EXPECT_VALID(association_valid(&caps, &selection, NULL),
        "explicit diagnostic opt-in on open profile");
}

static void test_association_rejections(void)
{
    static const UBYTE text[] = "correct horse battery staple";
    static const UBYTE printable_8[] = "abcdefgh";
    static const UBYTE nonprintable[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 0x7f };
    static const UBYTE embedded_nul[] = { 's', 'a', 'e', 0, 'p', 'a', 's', 's' };
    UBYTE psk[32] = { 0 };
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    const ULONG all_profiles = S2FULLMAC_PROFILE_OPEN |
        S2FULLMAC_PROFILE_WPA2_CCMP | S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    const ULONG all_pwe = S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
        S2FULLMAC_SAE_PWE_H2E | S2FULLMAC_SAE_PWE_BOTH;

    caps = make_caps(all_profiles, all_pwe, 0);
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "open profile with credential");
    EXPECT_INVALID(association_valid(&caps, NULL, NULL), "null selection");
    EXPECT_INVALID(association_valid(NULL, &selection, NULL), "null capabilities");

    selection.Size = (ULONG)sizeof(selection) - 1;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "short selection size");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    selection.Size = (ULONG)sizeof(selection) + 1;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "oversized selection size");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    selection.Version = 0;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "zero selection version");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    selection.Version = S2FULLMAC_VERSION + 1;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "unknown selection version");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    selection.Reserved = 1;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "nonzero selection reserved");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    selection.Flags = S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK | 0x80000000UL;
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "unknown selection flag");
    selection = make_selection(0, S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "empty selection profile");
    selection = make_selection(S2FULLMAC_PROFILE_OPEN |
        S2FULLMAC_PROFILE_WPA2_CCMP, S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "profile union");
    selection = make_selection(0x80000000UL, S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, NULL), "unknown selection profile");
    selection = make_selection(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0);
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD, text, 8);
    caps = make_caps(S2FULLMAC_PROFILE_WPA2_CCMP, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "unadvertised selection profile");

    caps = make_caps(S2FULLMAC_PROFILE_WPA2_CCMP, 0, 0);
    selection = make_selection(S2FULLMAC_PROFILE_WPA2_CCMP,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, NULL),
        "protected profile without credential");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    credential.Size = (ULONG)sizeof(credential) - 1;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "short credential size");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    credential.Size = (ULONG)sizeof(credential) + 1;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "oversized credential size");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    credential.Version = 0;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "zero credential version");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    credential.Version = S2WIRELESS_CREDENTIAL_VERSION + 1;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "unknown credential version");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        NULL, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "null credential data");
    credential = make_credential(0x80000000UL, printable_8, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "unknown WPA2 credential kind");
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD,
        printable_8, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE credential kind under WPA2 profile");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK, psk, 32);
    selection.PMFPolicy = S2FULLMAC_PMF_REQUIRED;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 profile with required PMF");
    selection.PMFPolicy = S2FULLMAC_PMF_DISABLED;
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_H2E;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 profile with SAE PWE");

    selection = make_selection(S2FULLMAC_PROFILE_WPA2_CCMP,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 7);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 passphrase shorter than 8");
    credential.Length = 64;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 passphrase longer than 63");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK, psk, 31);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 PSK shorter than 32");
    credential.Length = 33;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 PSK longer than 32");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        nonprintable, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 nonprintable byte");

    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF, all_pwe, 0);
    selection = make_selection(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0);
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD,
        embedded_nul, sizeof(embedded_nul));
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE embedded NUL");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK, psk, 32);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 PSK kind under SAE profile");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        printable_8, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "WPA2 passphrase kind under SAE profile");
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD, text, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "empty SAE password");
    credential.Length = 64;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE password longer than 63");
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD, text, 8);
    selection.PMFPolicy = S2FULLMAC_PMF_DISABLED;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE profile without required PMF");
    selection.PMFPolicy = S2FULLMAC_PMF_REQUIRED;
    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_H2E, 0);
    selection.SAEPweMethod = 0;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE profile without selected PWE");
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_H2E |
        S2FULLMAC_SAE_PWE_BOTH;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE selection with multiple PWE methods");
    selection.SAEPweMethod = 0x80000000UL;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE selection with unsupported PWE");
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_HUNT_AND_PECK;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "SAE selection with unadvertised PWE");

    caps = make_caps(S2FULLMAC_PROFILE_OPEN, 0,
        S2FULLMAC_CAPF_DIAGNOSTIC_RISK);
    selection = make_selection(S2FULLMAC_PROFILE_OPEN,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, NULL),
        "missing explicit diagnostic-risk flag");
    selection.Flags = S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK;
    EXPECT_VALID(association_valid(&caps, &selection, NULL),
        "explicit diagnostic-risk flag present");
}

static void test_rejection_precedes_data_reads(void)
{
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE *guard = (UBYTE *)malloc(64);
    const UBYTE *guarded_data;

    if (!guard) {
        fprintf(stderr, "FAIL: could not allocate ASan guard buffer\n");
        exit(2);
    }
    memset(guard, 'G', 64);
#if FULLMAC_TEST_ASAN
    __asan_poison_memory_region(guard, 64);
    guarded_data = guard;
#else
    guarded_data = (const UBYTE *)(uintptr_t)1;
#endif

    caps = make_caps(S2FULLMAC_PROFILE_WPA2_CCMP, 0, 0);
    selection = make_selection(S2FULLMAC_PROFILE_WPA2_CCMP,
        S2FULLMAC_PMF_DISABLED, 0, 0);
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        guarded_data, 7);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: short WPA2 passphrase rejected before data read");
    credential.Length = 64;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: long WPA2 passphrase rejected before data read");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK, guarded_data, 31);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: short raw PSK rejected before data read");
    credential.Length = 33;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: long raw PSK rejected before data read");
    credential = make_credential(0x80000000UL, guarded_data, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: wrong credential kind rejected before data read");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        guarded_data, 8);
    credential.Size = (ULONG)sizeof(credential) - 1;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: malformed credential header rejected before data read");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PASSPHRASE,
        guarded_data, 8);
    credential.Version++;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: unknown credential version rejected before data read");

    caps = make_caps(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_H2E, 0);
    selection = make_selection(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0);
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD,
        guarded_data, 0);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: empty SAE password rejected before data read");
    credential.Length = 64;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: long SAE password rejected before data read");
    credential = make_credential(S2WIRELESS_CRED_WPA2_PSK,
        guarded_data, 32);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: wrong SAE credential kind rejected before data read");
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_H2E |
        S2FULLMAC_SAE_PWE_BOTH;
    credential = make_credential(S2WIRELESS_CRED_SAE_PASSWORD,
        guarded_data, 8);
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: multi-PWE selection rejected before data read");
    selection.Size = (ULONG)sizeof(selection) + 1;
    EXPECT_INVALID(association_valid(&caps, &selection, &credential),
        "guard: malformed selection rejected before data read");

#if FULLMAC_TEST_ASAN
    __asan_unpoison_memory_region(guard, 64);
#endif
    free(guard);
}

int main(void)
{
    test_v1_regression_contract();
    test_capabilities_validity();
    test_positive_associations();
    test_association_rejections();
    test_rejection_precedes_data_reads();
    if (unexpected) {
        fprintf(stderr, "FAIL fullmac-contract checks=%u accepted=%u "
            "rejected=%u unexpected=%u fabricated_ap_facts=0\n",
            checks, accepted, rejected, unexpected);
        return 1;
    }
    printf("PASS fullmac-contract checks=%u accepted=%u rejected=%u "
        "unexpected=0 fabricated_ap_facts=0 ABI=hostmock-not-native-RV32\n",
        checks, accepted, rejected);
    return 0;
}
'''


def main() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="wireless-fullmac-contract-") as temp:
        temp_dir = Path(temp)
        stubs = temp_dir / "include"
        (stubs / "exec").mkdir(parents=True)
        (stubs / "utility").mkdir()
        (stubs / "exec" / "types.h").write_text(TYPES_STUB)
        (stubs / "utility" / "tagitem.h").write_text(TAGITEM_STUB)

        for optimization in (0, 2):
            binary = temp_dir / f"test-wireless-fullmac-contract-O{optimization}"
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
