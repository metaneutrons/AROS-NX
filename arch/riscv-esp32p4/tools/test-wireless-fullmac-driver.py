#!/usr/bin/env python3
"""Host-test the source-extracted SANA-II FullMAC association and query paths."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
WM_SRC = ROOT / "workbench/network/WirelessManager/src"
DRIVER_SOURCE = WM_SRC / "drivers/driver_sana2.c"

TYPES_STUB = """\
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t UWORD;
typedef int16_t WORD;
typedef uint8_t UBYTE;
typedef int8_t BYTE;
typedef void VOID;
typedef char TEXT;
typedef void *APTR;
typedef uintptr_t IPTR;
typedef intptr_t SIPTR;
#endif
"""

IO_STUB = """\
#ifndef EXEC_IO_H
#define EXEC_IO_H
#include <exec/types.h>
#define CMD_NONSTD 0x4000
struct IORequest {
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};
#endif
"""

TIMER_STUB = """\
#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H
#endif
"""

TEST_SOURCE = r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT(value) (1U << (value))
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define ETH_ALEN 6
#define os_memset memset
struct wpabuf;

/* These are the real WirelessManager public parameter and enum definitions. */
#include "drivers/driver.h"
#include <devices/sana2.h>
#include <devices/sana2wireless_fullmac.h>

#define MAX_SSID_LEN 32
#define MLME_REQ_COUNT 5
#define UPINT uintptr_t

static void CopyMem(const void *source, APTR destination, size_t size);
static int DoIO(APTR request_arg);

/* Extracted from the production source, including every negotiated field. */
/* DRIVER_DATA */

/* EXTRACTED_FUNCTIONS */

struct query_response {
    ULONG size_available;
    ULONG size_supplied;
    ULONG version;
    ULONG flags;
    ULONG protocols;
    ULONG key_management;
    ULONG pairwise_ciphers;
    ULONG group_ciphers;
    ULONG profiles;
    ULONG sae_pwe_methods;
    ULONG reserved0;
    ULONG reserved1;
};

struct association_observation {
    ULONG tags[10];
    IPTR values[10];
    size_t tag_count;
    char ssid[MAX_SSID_LEN + 1];
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    int saw_selection;
    int saw_credential;
    int statdata_was_null;
};

static unsigned checks;
static unsigned scenarios;
static unsigned failures;
static unsigned doio_calls;
static unsigned v1_queries;
static unsigned fullmac_queries;
static unsigned setoptions_calls;
static unsigned unexpected_io_calls;
static unsigned supplicant_events;
static int mock_v1_return;
static int mock_v1_io_error;
static int mock_fullmac_return;
static int mock_fullmac_io_error;
static int mock_options_return;
static int mock_options_io_error;
static struct query_response v1_reply;
static struct query_response fullmac_reply;
static ULONG observed_commands[4];
static ULONG v1_input_size_available;
static ULONG v1_input_version;
static int v1_input_data_null;
static ULONG fullmac_input_size_available;
static ULONG fullmac_input_version;
static struct association_observation observed;

static void check_value(const char *label, int actual, int expected, int line)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "FAIL line %d: %s got %d, expected %d\n",
            line, label, actual, expected);
    }
}

#define CHECK(label, actual, expected) \
    check_value((label), (int)(actual), (int)(expected), __LINE__)
#define SCENARIO() (++scenarios)

static void set_v1_reply(ULONG flags)
{
    memset(&v1_reply, 0, sizeof(v1_reply));
    v1_reply.size_available = 0xffffffffUL;
    v1_reply.size_supplied = sizeof(struct Sana2WirelessCapabilities);
    v1_reply.version = S2WIRELESS_CAPABILITIES_VERSION;
    v1_reply.flags = flags;
}

static void set_fullmac_reply(ULONG profiles, ULONG pwe, ULONG flags)
{
    memset(&fullmac_reply, 0, sizeof(fullmac_reply));
    fullmac_reply.size_available = 0xffffffffUL;
    fullmac_reply.size_supplied = sizeof(struct Sana2FullMACCapabilities);
    fullmac_reply.version = S2FULLMAC_VERSION;
    fullmac_reply.flags = flags;
    fullmac_reply.profiles = profiles;
    fullmac_reply.sae_pwe_methods = pwe;
}

static void reset_mocks(void)
{
    doio_calls = 0;
    v1_queries = 0;
    fullmac_queries = 0;
    setoptions_calls = 0;
    unexpected_io_calls = 0;
    supplicant_events = 0;
    mock_v1_return = 0;
    mock_v1_io_error = 0;
    mock_fullmac_return = 0;
    mock_fullmac_io_error = 0;
    mock_options_return = 0;
    mock_options_io_error = 0;
    memset(observed_commands, 0, sizeof(observed_commands));
    v1_input_size_available = 0;
    v1_input_version = 0;
    v1_input_data_null = 0;
    fullmac_input_size_available = 0;
    fullmac_input_version = 0;
    memset(&observed, 0, sizeof(observed));
    set_v1_reply(S2WIRELESS_CAPF_SECURITY_SUMMARY |
        S2WIRELESS_CAPF_CONNECT_FAILED);
    set_fullmac_reply(S2FULLMAC_PROFILE_SAE_CCMP_PMF,
        S2FULLMAC_SAE_PWE_H2E, 0);
}

static void CopyMem(const void *source, APTR destination, size_t size)
{
    memcpy(destination, source, size);
}

static void copy_v1_response(struct Sana2WirelessCapabilities *caps,
    const struct query_response *reply)
{
    if (reply->size_available != 0xffffffffUL)
        caps->SizeAvailable = reply->size_available;
    caps->SizeSupplied = reply->size_supplied;
    caps->Version = reply->version;
    caps->Flags = reply->flags;
    caps->Protocols = reply->protocols;
    caps->KeyManagement = reply->key_management;
    caps->PairwiseCiphers = reply->pairwise_ciphers;
    caps->GroupCiphers = reply->group_ciphers;
}

static void copy_fullmac_response(struct Sana2FullMACCapabilities *caps,
    const struct query_response *reply)
{
    if (reply->size_available != 0xffffffffUL)
        caps->SizeAvailable = reply->size_available;
    caps->SizeSupplied = reply->size_supplied;
    caps->Version = reply->version;
    caps->Flags = reply->flags;
    caps->Profiles = reply->profiles;
    caps->SAEPweMethods = reply->sae_pwe_methods;
    caps->Reserved[0] = reply->reserved0;
    caps->Reserved[1] = reply->reserved1;
}

static size_t bounded_string_length(const char *value, size_t limit)
{
    size_t length;
    for (length = 0; length < limit && value[length]; ++length) {}
    return length;
}

static void observe_association_tags(struct IOSana2Req *request)
{
    const struct TagItem *items = request->ios2_Data;
    size_t i;

    observed.statdata_was_null = request->ios2_StatData == NULL;
    if (!items) {
        ++failures;
        fprintf(stderr, "FAIL: SETOPTIONS had no tag list\n");
        return;
    }
    for (i = 0; i < sizeof(observed.tags) / sizeof(observed.tags[0]); ++i) {
        observed.tags[i] = items[i].ti_Tag;
        observed.values[i] = items[i].ti_Data;
        if (items[i].ti_Tag == S2INFO_SSID) {
            const char *ssid = (const char *)(uintptr_t)items[i].ti_Data;
            size_t length = bounded_string_length(ssid, sizeof(observed.ssid));
            if (length < sizeof(observed.ssid))
                memcpy(observed.ssid, ssid, length + 1);
        } else if (items[i].ti_Tag == S2INFO_FullMACAssociation) {
            const struct Sana2FullMACAssociation *selection =
                (const struct Sana2FullMACAssociation *)(uintptr_t)items[i].ti_Data;
            observed.selection = *selection;
            observed.saw_selection = 1;
        } else if (items[i].ti_Tag == S2INFO_WirelessCredential) {
            const struct Sana2WirelessCredential *credential =
                (const struct Sana2WirelessCredential *)(uintptr_t)items[i].ti_Data;
            observed.credential = *credential;
            observed.saw_credential = 1;
        }
        if (items[i].ti_Tag == TAG_END) {
            observed.tag_count = i + 1;
            return;
        }
    }
    ++failures;
    fprintf(stderr, "FAIL: SETOPTIONS did not terminate within the tag bound\n");
}

static int DoIO(APTR request_arg)
{
    struct IOSana2Req *request = request_arg;
    unsigned slot = doio_calls++;

    if (slot < sizeof(observed_commands) / sizeof(observed_commands[0]))
        observed_commands[slot] = request->ios2_Req.io_Command;
    if (request->ios2_Req.io_Command == S2_GETWIRELESSCAPABILITIES) {
        struct Sana2WirelessCapabilities *caps = request->ios2_StatData;
        ++v1_queries;
        if (!caps) {
            ++unexpected_io_calls;
            return -1;
        }
        v1_input_size_available = caps->SizeAvailable;
        v1_input_version = caps->Version;
        v1_input_data_null = request->ios2_Data == NULL &&
            request->ios2_DataLength == 0;
        copy_v1_response(caps, &v1_reply);
        request->ios2_Req.io_Error = mock_v1_io_error;
        return mock_v1_return;
    }
    if (request->ios2_Req.io_Command == S2_GETFULLMACCAPABILITIES) {
        struct Sana2FullMACCapabilities *caps = request->ios2_StatData;
        ++fullmac_queries;
        if (!caps) {
            ++unexpected_io_calls;
            return -1;
        }
        fullmac_input_size_available = caps->SizeAvailable;
        fullmac_input_version = caps->Version;
        copy_fullmac_response(caps, &fullmac_reply);
        request->ios2_Req.io_Error = mock_fullmac_io_error;
        return mock_fullmac_return;
    }
    if (request->ios2_Req.io_Command == S2_SETOPTIONS) {
        ++setoptions_calls;
        observe_association_tags(request);
        request->ios2_Req.io_Error = mock_options_io_error;
        return mock_options_return;
    }
    ++unexpected_io_calls;
    request->ios2_Req.io_Error = S2ERR_NOT_SUPPORTED;
    return -1;
}

struct association_fixture {
    struct wpa_driver_sana2_data driver;
    struct IOSana2Req request;
    struct wpa_driver_associate_params params;
    u8 ssid[40];
    u8 password[64];
    u8 psk[S2WIRELESS_PSK_SIZE];
    u8 bssid[ETH_ALEN];
};

static void init_association_fixture(struct association_fixture *fixture)
{
    size_t i;
    memset(fixture, 0, sizeof(*fixture));
    reset_mocks();
    fixture->driver.request = &fixture->request;
    fixture->driver.hard_mac = 1;
    fixture->driver.wireless_caps_valid = 1;
    fixture->driver.wireless_caps.Flags =
        S2WIRELESS_CAPF_SECURITY_SUMMARY | S2WIRELESS_CAPF_CONNECT_FAILED;
    fixture->driver.fullmac_caps_valid = 1;
    fixture->driver.fullmac_caps.SizeAvailable =
        sizeof(fixture->driver.fullmac_caps);
    fixture->driver.fullmac_caps.SizeSupplied =
        sizeof(fixture->driver.fullmac_caps);
    fixture->driver.fullmac_caps.Version = S2FULLMAC_VERSION;
    fixture->driver.fullmac_caps.Profiles =
        S2FULLMAC_PROFILE_SAE_CCMP_PMF | S2FULLMAC_PROFILE_WPA2_CCMP |
        S2FULLMAC_PROFILE_OPEN;
    fixture->driver.fullmac_caps.SAEPweMethods = S2FULLMAC_SAE_PWE_H2E;
    fixture->driver.event_mask = S2EVENT_CONNECT_FAILED;
    fixture->driver.next_association_cookie = 0x1020;
    fixture->driver.active_association_cookie = 0x55;
    fixture->driver.online = 0;
    memcpy(fixture->driver.ssid, "before", sizeof("before"));
    for (i = 0; i < sizeof(fixture->driver.bssid); ++i)
        fixture->driver.bssid[i] = (u8)(0xa0u + i);
    for (i = 0; i < sizeof(fixture->ssid); ++i)
        fixture->ssid[i] = (u8)'x';
    memcpy(fixture->ssid, "sae-network", sizeof("sae-network"));
    memcpy(fixture->password, "distinct-sae", sizeof("distinct-sae"));
    fixture->params.ssid = fixture->ssid;
    fixture->params.ssid_len = strlen((const char *)fixture->ssid);
    fixture->params.bssid = fixture->bssid;
    fixture->params.mode = IEEE80211_MODE_INFRA;
    fixture->params.freq = 2437;
    fixture->params.auth_alg = WPA_AUTH_ALG_OPEN;
    fixture->params.pairwise_suite = CIPHER_CCMP;
    fixture->params.group_suite = CIPHER_CCMP;
    fixture->params.key_mgmt_suite = KEY_MGMT_SAE;
    fixture->params.fullmac_profile = S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    fixture->params.fullmac_sae_pwe_mode = S2FULLMAC_SAE_PWE_H2E;
    fixture->params.sae_password = fixture->password;
    fixture->params.sae_password_len = strlen((const char *)fixture->password);
    fixture->request.ios2_Data = (APTR)(uintptr_t)0x1111;
    fixture->request.ios2_StatData = (APTR)(uintptr_t)0x2222;
}

static void check_no_association_io(const char *label,
    struct association_fixture *fixture, int result)
{
    CHECK(label, result, -1);
    CHECK(label, doio_calls, 0);
    CHECK(label, setoptions_calls, 0);
    CHECK(label, unexpected_io_calls, 0);
    CHECK(label, strcmp(fixture->driver.ssid, "before") == 0, 1);
    CHECK(label, fixture->driver.active_association_cookie, 0x55);
}

static void test_sae_typed_association_and_ack_boundary(void)
{
    struct association_fixture fixture;
    struct Sana2WirelessCredential captured;
    struct Sana2FullMACAssociation selection;
    unsigned i;
    int result;
    const ULONG exact_tags[] = {
        S2INFO_SSID, S2INFO_BSSID, S2INFO_PortType, S2INFO_Channel,
        S2INFO_FullMACAssociation, S2INFO_WirelessCredential,
        S2INFO_AssociationCookie, TAG_END
    };

    init_association_fixture(&fixture);
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
        &fixture.params);
    CHECK("SAE typed association", result, 0);
    CHECK("SAE typed association", doio_calls, 1);
    CHECK("SAE typed association", setoptions_calls, 1);
    CHECK("SAE typed association", unexpected_io_calls, 0);
    CHECK("SAE typed association", observed.tag_count,
        sizeof(exact_tags) / sizeof(exact_tags[0]));
    for (i = 0; i < sizeof(exact_tags) / sizeof(exact_tags[0]); ++i)
        CHECK("SAE exact typed tag list", observed.tags[i], exact_tags[i]);
    CHECK("SAE typed association", strcmp(observed.ssid, "sae-network") == 0, 1);
    CHECK("SAE typed association", observed.values[2], S2PORT_MANAGED);
    CHECK("SAE typed association", observed.values[3], 6);
    CHECK("SAE typed association", observed.values[6], 0x1021);
    CHECK("SAE typed association", observed.statdata_was_null, 1);
    CHECK("SAE typed association", observed.saw_selection, 1);
    selection = observed.selection;
    CHECK("SAE selection", selection.Size, sizeof(selection));
    CHECK("SAE selection", selection.Version, S2FULLMAC_VERSION);
    CHECK("SAE selection", selection.Profile, S2FULLMAC_PROFILE_SAE_CCMP_PMF);
    CHECK("SAE selection", selection.PMFPolicy, S2FULLMAC_PMF_REQUIRED);
    CHECK("SAE selection", selection.SAEPweMethod, S2FULLMAC_SAE_PWE_H2E);
    CHECK("SAE selection", selection.Flags, 0);
    CHECK("SAE typed association", observed.saw_credential, 1);
    captured = observed.credential;
    CHECK("SAE credential", captured.Size, sizeof(captured));
    CHECK("SAE credential", captured.Version, S2WIRELESS_CREDENTIAL_VERSION);
    CHECK("SAE credential", captured.Kind, S2WIRELESS_CRED_SAE_PASSWORD);
    CHECK("SAE credential", captured.Length, fixture.params.sae_password_len);
    CHECK("SAE credential", captured.Data == fixture.password, 1);
    CHECK("SAE legacy auth tags absent", observed.tag_count, 8);
    CHECK("SAE request cleanup", fixture.request.ios2_Data == NULL, 1);
    CHECK("SAE request cleanup", fixture.request.ios2_StatData == NULL, 1);
    CHECK("SAE ACK is not link-up", fixture.driver.online, 0);
    CHECK("SAE ACK is not link-up", supplicant_events, 0);
    CHECK("SAE ACK is not link-up", fixture.driver.active_association_cookie, 0x1021);
    CHECK("SAE prior BSSID unchanged",
        fixture.driver.bssid[0] == 0xa0 && fixture.driver.bssid[5] == 0xa5, 1);
    CHECK("SAE copied target SSID after ACK",
        strcmp(fixture.driver.ssid, "sae-network") == 0, 1);
}

static void test_pwe_both_is_a_single_selected_mode(void)
{
    struct association_fixture fixture;
    int result;

    init_association_fixture(&fixture);
    fixture.driver.fullmac_caps.SAEPweMethods = S2FULLMAC_SAE_PWE_BOTH;
    fixture.params.fullmac_sae_pwe_mode = S2FULLMAC_SAE_PWE_BOTH;
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
        &fixture.params);
    CHECK("PWE_BOTH selectable mode", result, 0);
    CHECK("PWE_BOTH selectable mode", doio_calls, 1);
    CHECK("PWE_BOTH selectable mode", observed.selection.SAEPweMethod,
        S2FULLMAC_SAE_PWE_BOTH);
}

static void test_setoptions_error_releases_borrowed_pointers(void)
{
    struct association_fixture fixture;
    int kind;
    int result;

    for (kind = 0; kind < 2; ++kind) {
        init_association_fixture(&fixture);
        mock_options_return = kind == 0 ? 7 : 0;
        mock_options_io_error = kind == 0 ? 0 : S2ERR_BAD_STATE;
        SCENARIO();
        result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
            &fixture.params);
        CHECK("SETOPTIONS failure", result, -1);
        CHECK("SETOPTIONS failure", doio_calls, 1);
        CHECK("SETOPTIONS failure", setoptions_calls, 1);
        CHECK("SETOPTIONS failure clears request Data",
            fixture.request.ios2_Data == NULL, 1);
        CHECK("SETOPTIONS failure clears request StatData",
            fixture.request.ios2_StatData == NULL, 1);
        CHECK("SETOPTIONS failure preserves active cookie",
            fixture.driver.active_association_cookie, 0x55);
        CHECK("SETOPTIONS failure does not publish online",
            fixture.driver.online, 0);
        CHECK("SETOPTIONS failure does not publish a link event",
            supplicant_events, 0);
        CHECK("SETOPTIONS failure leaves cached SSID unchanged",
            strcmp(fixture.driver.ssid, "before") == 0, 1);
        CHECK("SETOPTIONS failure leaves event mask armed",
            fixture.driver.event_mask, S2EVENT_CONNECT_FAILED);
    }
}

static void test_diagnostic_risk_opt_in(void)
{
    struct association_fixture fixture;
    int result;

    init_association_fixture(&fixture);
    fixture.driver.fullmac_caps.Flags = S2FULLMAC_CAPF_DIAGNOSTIC_RISK;
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
        &fixture.params);
    check_no_association_io("diagnostic risk requires opt-in", &fixture, result);

    init_association_fixture(&fixture);
    fixture.driver.fullmac_caps.Flags = S2FULLMAC_CAPF_DIAGNOSTIC_RISK;
    fixture.params.fullmac_allow_diagnostic = 1;
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
        &fixture.params);
    CHECK("explicit diagnostic opt-in", result, 0);
    CHECK("explicit diagnostic opt-in",
        observed.selection.Flags, S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK);
    CHECK("explicit diagnostic opt-in", setoptions_calls, 1);
}

static void test_invalid_association_inputs_have_no_io(void)
{
    struct association_fixture fixture;
    int result;

#define REJECT_CASE(name, statement) do { \
    init_association_fixture(&fixture); \
    statement; \
    SCENARIO(); \
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver, &fixture.params); \
    check_no_association_io((name), &fixture, result); \
} while (0)

    init_association_fixture(&fixture);
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(NULL, &fixture.params);
    CHECK("null driver returned from helper", result, -1);
    CHECK("null driver performs no I/O", doio_calls, 0);
    CHECK("null driver keeps fixture state", strcmp(fixture.driver.ssid, "before") == 0, 1);
    init_association_fixture(&fixture);
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver, NULL);
    CHECK("null params returned from helper", result, -1);
    CHECK("null params performs no I/O", doio_calls, 0);
    REJECT_CASE("null request", fixture.driver.request = NULL);
    REJECT_CASE("not a hard MAC", fixture.driver.hard_mac = 0);
    REJECT_CASE("FullMAC caps not negotiated", fixture.driver.fullmac_caps_valid = 0);
    REJECT_CASE("v1 caps not negotiated", fixture.driver.wireless_caps_valid = 0);
    REJECT_CASE("v1 failure capability missing",
        fixture.driver.wireless_caps.Flags = S2WIRELESS_CAPF_SECURITY_SUMMARY);
    REJECT_CASE("failure event not armed", fixture.driver.event_mask = 0);
    REJECT_CASE("diagnostic flag value above one",
        fixture.params.fullmac_allow_diagnostic = 2);
    REJECT_CASE("non-infrastructure mode",
        fixture.params.mode = IEEE80211_MODE_IBSS);
    REJECT_CASE("wrong authentication algorithm",
        fixture.params.auth_alg = WPA_AUTH_ALG_SHARED);
    REJECT_CASE("null SSID", fixture.params.ssid = NULL);
    REJECT_CASE("empty SSID", fixture.params.ssid_len = 0);
    REJECT_CASE("SSID longer than 32", fixture.params.ssid_len = 33);
    REJECT_CASE("embedded NUL in SSID", fixture.ssid[2] = 0);
    REJECT_CASE("invalid channel frequency", fixture.params.freq = 2413);
    REJECT_CASE("frequency outside supported band", fixture.params.freq = 5180);
    REJECT_CASE("WEP key length supplied", fixture.params.wep_key_len[0] = 5);
    REJECT_CASE("WEP key pointer supplied", fixture.params.wep_key[0] = fixture.password);
    REJECT_CASE("WPA IE length supplied", fixture.params.wpa_ie_len = 2);
    REJECT_CASE("WPA IE pointer supplied", fixture.params.wpa_ie = fixture.password);
    REJECT_CASE("legacy passphrase supplied",
        fixture.params.passphrase = (const char *)fixture.password);
    REJECT_CASE("legacy PSK supplied", fixture.params.psk = fixture.psk);
    REJECT_CASE("PSK and passphrase supplied",
        fixture.params.passphrase = (const char *)fixture.password;
        fixture.params.psk = fixture.psk);
    REJECT_CASE("wrong key management suite",
        fixture.params.key_mgmt_suite = KEY_MGMT_PSK);
    REJECT_CASE("non-CCMP pairwise suite",
        fixture.params.pairwise_suite = CIPHER_TKIP);
    REJECT_CASE("non-CCMP group suite",
        fixture.params.group_suite = CIPHER_TKIP);
    REJECT_CASE("unknown FullMAC profile",
        fixture.params.fullmac_profile = 0x80000000U);
    REJECT_CASE("profile union",
        fixture.params.fullmac_profile = S2FULLMAC_PROFILE_SAE_CCMP_PMF |
            S2FULLMAC_PROFILE_WPA2_CCMP);
    REJECT_CASE("unadvertised SAE PWE",
        fixture.params.fullmac_sae_pwe_mode = S2FULLMAC_SAE_PWE_HUNT_AND_PECK);
    REJECT_CASE("multiple SAE PWE modes",
        fixture.params.fullmac_sae_pwe_mode = S2FULLMAC_SAE_PWE_H2E |
            S2FULLMAC_SAE_PWE_BOTH);
    REJECT_CASE("missing SAE PWE",
        fixture.params.fullmac_sae_pwe_mode = 0);
    REJECT_CASE("null SAE password",
        fixture.params.sae_password = NULL);
    REJECT_CASE("empty SAE password", fixture.params.sae_password_len = 0);
    REJECT_CASE("SAE password length 64", fixture.params.sae_password_len = 64);
    REJECT_CASE("SAE length overflow", fixture.params.sae_password_len = (size_t)-1);
    REJECT_CASE("embedded NUL in SAE password", fixture.password[2] = 0);
    REJECT_CASE("diagnostic-risk caps reserved word",
        fixture.driver.fullmac_caps.Reserved[0] = 1);
    REJECT_CASE("diagnostic-risk caps malformed size",
        fixture.driver.fullmac_caps.SizeSupplied--);
    REJECT_CASE("diagnostic-risk caps short caller size",
        fixture.driver.fullmac_caps.SizeAvailable--);
    REJECT_CASE("diagnostic-risk caps unknown version",
        fixture.driver.fullmac_caps.Version++);
    REJECT_CASE("caps do not advertise SAE profile",
        fixture.driver.fullmac_caps.Profiles = S2FULLMAC_PROFILE_OPEN);
    REJECT_CASE("association cookie exhausted",
        fixture.driver.next_association_cookie = 0xffffffffUL);

#undef REJECT_CASE

    init_association_fixture(&fixture);
    fixture.params.sae_password = (const u8 *)(uintptr_t)1;
    fixture.params.sae_password_len = 64;
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&fixture.driver,
        &fixture.params);
    check_no_association_io("long poisoned SAE input rejected before data read",
        &fixture, result);
}

static void init_query_fixture(struct wpa_driver_sana2_data *driver,
    struct IOSana2Req *request, int hard_mac)
{
    memset(driver, 0, sizeof(*driver));
    memset(request, 0, sizeof(*request));
    reset_mocks();
    driver->request = request;
    driver->hard_mac = hard_mac;
}

static void test_v1_to_fullmac_query_and_event_arm(void)
{
    struct wpa_driver_sana2_data driver;
    struct IOSana2Req request;
    struct association_fixture association;
    int result;
    ULONG mask;

    init_query_fixture(&driver, &request, 1);
    SCENARIO();
    mask = wpa_driver_sana2_initial_event_mask(&driver);
    CHECK("v1 then FullMAC query", mask,
        S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT |
            S2EVENT_CONNECT_FAILED);
    CHECK("v1 then FullMAC query", v1_queries, 1);
    CHECK("v1 then FullMAC query", fullmac_queries, 1);
    CHECK("v1 then FullMAC query", observed_commands[0],
        S2_GETWIRELESSCAPABILITIES);
    CHECK("v1 then FullMAC query", observed_commands[1],
        S2_GETFULLMACCAPABILITIES);
    CHECK("v1 query initialized full span", v1_input_size_available,
        sizeof(struct Sana2WirelessCapabilities));
    CHECK("v1 query selected version", v1_input_version,
        S2WIRELESS_CAPABILITIES_VERSION);
    CHECK("v1 query has no data tag", v1_input_data_null, 1);
    CHECK("FullMAC query initialized full span", fullmac_input_size_available,
        sizeof(struct Sana2FullMACCapabilities));
    CHECK("FullMAC query selected version", fullmac_input_version,
        S2FULLMAC_VERSION);
    CHECK("v1 capability retained", driver.wireless_caps_valid, 1);
    CHECK("FullMAC capability retained", driver.fullmac_caps_valid, 1);
    CHECK("failure event armed before association", (mask & S2EVENT_CONNECT_FAILED) != 0, 1);
    CHECK("query cleans borrowed statdata", request.ios2_StatData == NULL, 1);
    CHECK("query uses only two requests", doio_calls, 2);
    CHECK("query generated no unexpected I/O", unexpected_io_calls, 0);

    init_association_fixture(&association);
    association.driver = driver;
    association.request = request;
    association.driver.request = &association.request;
    association.driver.event_mask = mask;
    association.driver.next_association_cookie = 0x30;
    association.driver.active_association_cookie = 0;
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&association.driver,
        &association.params);
    CHECK("queried caps authorize typed association", result, 0);
    CHECK("queried caps authorize exactly one SETOPTIONS",
        setoptions_calls, 1);
    CHECK("query precedes armed FullMAC association",
        association.driver.event_mask & S2EVENT_CONNECT_FAILED,
        S2EVENT_CONNECT_FAILED);
}

static void test_legacy_v1_responses_do_not_expand_contract(void)
{
    struct wpa_driver_sana2_data driver;
    struct IOSana2Req request;
    ULONG mask;

    init_query_fixture(&driver, &request, 1);
    set_v1_reply(S2WIRELESS_CAPF_FOURWAY);
    v1_reply.protocols = S2WIRELESS_PROTO_RSN;
    v1_reply.key_management = S2WIRELESS_KEY_PSK;
    v1_reply.pairwise_ciphers = S2WIRELESS_CIPHER_CCMP;
    v1_reply.group_ciphers = S2WIRELESS_CIPHER_CCMP;
    SCENARIO();
    mask = wpa_driver_sana2_initial_event_mask(&driver);
    CHECK("legacy v1 FOURWAY remains valid", driver.wireless_caps_valid, 1);
    CHECK("legacy v1 FOURWAY retained", driver.wireless_caps.Flags,
        S2WIRELESS_CAPF_FOURWAY);
    CHECK("legacy v1 protocol mask retained", driver.wireless_caps.Protocols,
        S2WIRELESS_PROTO_RSN);
    CHECK("legacy v1 query does not request FullMAC", fullmac_queries, 0);
    CHECK("legacy v1 event mask unchanged", mask,
        S2EVENT_OFFLINE | S2EVENT_CONNECT | S2EVENT_DISCONNECT);
    CHECK("legacy v1 performs one query", doio_calls, 1);
    CHECK("legacy v1 no FullMAC grant", driver.fullmac_caps_valid, 0);
}

static void test_unsupported_or_invalid_fullmac_query_fails_closed(void)
{
    struct wpa_driver_sana2_data driver;
    struct IOSana2Req request;
    struct association_fixture association;
    int kind;
    ULONG mask;
    int result;

    for (kind = 0; kind < 2; ++kind) {
        init_query_fixture(&driver, &request, 1);
        if (kind == 0) {
            mock_fullmac_return = -1;
        } else {
            mock_fullmac_io_error = S2ERR_NOT_SUPPORTED;
        }
        SCENARIO();
        mask = wpa_driver_sana2_initial_event_mask(&driver);
        CHECK("unsupported FullMAC query keeps valid v1", driver.wireless_caps_valid, 1);
        CHECK("unsupported FullMAC query grants no new caps", driver.fullmac_caps_valid, 0);
        CHECK("v1 failure event still armed", (mask & S2EVENT_CONNECT_FAILED) != 0, 1);
        CHECK("unsupported FullMAC response cleans statdata",
            request.ios2_StatData == NULL, 1);
        CHECK("unsupported FullMAC query count", fullmac_queries, 1);
    }

    for (kind = 0; kind < 8; ++kind) {
        init_query_fixture(&driver, &request, 1);
        switch (kind) {
        case 0: fullmac_reply.size_supplied = sizeof(struct Sana2FullMACCapabilities) - 1; break;
        case 1: fullmac_reply.version = S2FULLMAC_VERSION + 1; break;
        case 2: fullmac_reply.flags = 0x80000000UL; break;
        case 3: fullmac_reply.profiles = 0; break;
        case 4: fullmac_reply.profiles |= 0x80000000UL; break;
        case 5: fullmac_reply.reserved0 = 1; break;
        case 6: fullmac_reply.sae_pwe_methods = 0; break;
        default: fullmac_reply.size_available = sizeof(struct Sana2FullMACCapabilities) - 1; break;
        }
        SCENARIO();
        mask = wpa_driver_sana2_initial_event_mask(&driver);
        CHECK("invalid FullMAC response preserves v1", driver.wireless_caps_valid, 1);
        CHECK("invalid FullMAC response refused", driver.fullmac_caps_valid, 0);
        CHECK("invalid FullMAC response keeps v1 failure event",
            (mask & S2EVENT_CONNECT_FAILED) != 0, 1);
        CHECK("invalid FullMAC response attempted after v1", fullmac_queries, 1);
        CHECK("invalid FullMAC response cleans statdata",
            request.ios2_StatData == NULL, 1);
    }

    /* A refused extension must also refuse an association without S2_SETOPTIONS. */
    init_query_fixture(&driver, &request, 1);
    fullmac_reply.profiles = 0;
    SCENARIO();
    mask = wpa_driver_sana2_initial_event_mask(&driver);
    CHECK("invalid FullMAC profile mask", driver.fullmac_caps_valid, 0);
    init_association_fixture(&association);
    association.driver = driver;
    association.request = request;
    association.driver.request = &association.request;
    association.driver.event_mask = mask;
    association.driver.active_association_cookie = 0x55;
    memcpy(association.driver.ssid, "before", sizeof("before"));
    SCENARIO();
    result = wpa_driver_sana2_associate_fullmac(&association.driver,
        &association.params);
    check_no_association_io("invalid query cannot authorize association",
        &association, result);
}

int main(void)
{
    test_sae_typed_association_and_ack_boundary();
    test_pwe_both_is_a_single_selected_mode();
    test_setoptions_error_releases_borrowed_pointers();
    test_diagnostic_risk_opt_in();
    test_invalid_association_inputs_have_no_io();
    test_v1_to_fullmac_query_and_event_arm();
    test_legacy_v1_responses_do_not_expand_contract();
    test_unsupported_or_invalid_fullmac_query_fails_closed();
    if (failures) {
        fprintf(stderr, "FAIL fullmac-driver scenarios=%u checks=%u failures=%u "
            "doio=%u fabricated_ap_facts=0\n", scenarios, checks, failures,
            doio_calls);
        return 1;
    }
    printf("PASS fullmac-driver scenarios=%u checks=%u failures=0 "
        "fabricated_ap_facts=0 ABI=host-mock-not-native-RV32\n",
        scenarios, checks);
    return 0;
}
'''


def extract_braced(source: str, start: int, opening: int) -> str:
    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line-comment"
            index += 1
        elif char == "/" and following == "*":
            state = "block-comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise ValueError("unterminated C block while extracting production code")


def extract_named_block(source: str, marker: str, *, declaration: bool = False) -> str:
    start = source.find(marker)
    if start < 0:
        raise ValueError(f"missing production source marker: {marker}")
    if source.find(marker, start + len(marker)) >= 0:
        raise ValueError(f"ambiguous production source marker: {marker}")
    opening = (
        start + marker.rfind("{")
        if "{" in marker
        else source.find("{", start + len(marker))
    )
    if opening < 0:
        raise ValueError(f"missing opening brace after {marker}")
    block = extract_braced(source, start, opening)
    return block + ";" if declaration else block


def make_source() -> str:
    driver_source = DRIVER_SOURCE.read_text(encoding="utf-8")
    driver_struct = extract_named_block(
        driver_source, "struct wpa_driver_sana2_data {", declaration=True
    )
    signatures = (
        "static int wpa_driver_sana2_associate_fullmac(",
        "static int wpa_driver_sana2_wireless_caps_valid(",
        "static int wpa_driver_sana2_query_wireless_caps(",
        "static ULONG wpa_driver_sana2_initial_event_mask(",
    )
    extracted = []
    for signature in signatures:
        start = driver_source.find(signature)
        if start < 0 or driver_source.find(signature, start + len(signature)) >= 0:
            raise ValueError(f"expected one production definition: {signature}")
        opening = driver_source.find("{", start + len(signature))
        if opening < 0:
            raise ValueError(f"missing body for production definition: {signature}")
        extracted.append(extract_braced(driver_source, start, opening))
    output = TEST_SOURCE.replace("/* DRIVER_DATA */", driver_struct)
    output = output.replace("/* EXTRACTED_FUNCTIONS */", "\n\n".join(extracted))
    if "/* DRIVER_DATA */" in output or "/* EXTRACTED_FUNCTIONS */" in output:
        raise ValueError("failed to insert all production source blocks")
    return output


def main() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    source = make_source()
    with tempfile.TemporaryDirectory(prefix="wireless-fullmac-driver-") as temp:
        temp_dir = Path(temp)
        stubs = temp_dir / "include"
        (stubs / "exec").mkdir(parents=True)
        (stubs / "devices").mkdir()
        (stubs / "exec" / "types.h").write_text(TYPES_STUB, encoding="utf-8")
        (stubs / "exec" / "io.h").write_text(IO_STUB, encoding="utf-8")
        (stubs / "devices" / "timer.h").write_text(TIMER_STUB, encoding="utf-8")
        test_source = temp_dir / "test_wireless_fullmac_driver.c"
        test_source.write_text(source, encoding="utf-8")

        for optimization in (0, 2):
            binary = temp_dir / f"test-wireless-fullmac-driver-O{optimization}"
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
                str(WM_SRC),
                "-I",
                str(ROOT / "compiler/include"),
                str(test_source),
                "-o",
                str(binary),
            ]
            print(f"== O{optimization} ASan/UBSan ==", flush=True)
            subprocess.run(command, check=True)
            run_env = os.environ.copy()
            run_env["ASAN_OPTIONS"] = "halt_on_error=1:detect_stack_use_after_return=1"
            run_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            subprocess.run([str(binary)], env=run_env, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
