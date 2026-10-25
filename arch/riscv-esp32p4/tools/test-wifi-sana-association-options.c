/* Host-only tests of the production association-options header. The actual
 * SANA-II and TagItem headers are included; radio, BeginIO and hardware are
 * outside this fixture. Borrowed tags, strings and IE extents stay readable. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *APTR;
typedef uintptr_t IPTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
#define FALSE ((BOOL)0)
#define TRUE  ((BOOL)1)

/* Let the public headers use these host-shimmed AROS primitive types while
 * retaining their actual TagItem and wireless tag/constant definitions. */
#define EXEC_TYPES_H 1
#include "../network/association_options.h"

typedef char assert_ulong_is_32_bits[sizeof(ULONG) == 4 ? 1 : -1];
typedef char assert_bool_is_16_bits[sizeof(BOOL) == 2 ? 1 : -1];
typedef char assert_tagitem_uses_native_host_iptr[
    sizeof(struct TagItem) == 2 * sizeof(IPTR) ? 1 : -1];

static struct p4_wifi_association_target target;
static unsigned long checks;
static unsigned long cases;

#define CHECK(expression)                                                   \
    do                                                                      \
    {                                                                       \
        ++checks;                                                           \
        if (!(expression))                                                  \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #expression);                                          \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

static IPTR pointer_data(const void *pointer)
{
    return (IPTR)(uintptr_t)pointer;
}

static void set_tag(struct TagItem *item, ULONG tag, IPTR data)
{
    item->ti_Tag = (Tag)tag;
    item->ti_Data = data;
}

static BOOL bytes_are_zero(const UBYTE *bytes, size_t length)
{
    size_t index;
    for (index = 0; index < length; ++index)
        if (bytes[index] != 0u)
            return FALSE;
    return TRUE;
}

static void poison_target(void)
{
    memset(&target, 0xA5, sizeof(target));
}

static void expect_success(const struct TagItem *tags)
{
    ++cases;
    poison_target();
    CHECK(p4wlan_copy_association_options(&target, tags));
}

static void expect_refusal(const struct TagItem *tags)
{
    BOOL accepted;
    ++cases;
    poison_target();
    accepted = p4wlan_copy_association_options(&target, tags);
    if (accepted)
        fprintf(stderr, "unexpected acceptance in case %lu\n", cases);
    CHECK(!accepted);
    CHECK(bytes_are_zero((const UBYTE *)&target, sizeof(target)));
}

static void set_required(struct TagItem *tags, const UBYTE *ssid,
                         const UBYTE *rsn, IPTR wpa_ie_extent)
{
    set_tag(&tags[0], S2INFO_SSID, pointer_data(ssid));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    /* The shared SANA contract passes WPA_AUTH_ALG_OPEN as bit zero. */
    set_tag(&tags[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[3], S2INFO_WPAInfo, pointer_data(rsn));
    set_tag(&tags[4], S2INFO_WPAInfoLength, (IPTR)wpa_ie_extent);
}

static void set_rsn(UBYTE ie[24], UBYTE length)
{
    memset(ie, 0, 24u);
    ie[0] = 48u;
    ie[1] = length;
    ie[2] = 1u;
    ie[3] = 0u;
    ie[4] = 0u;
    ie[5] = 15u;
    ie[6] = 172u;
    ie[7] = 4u;             /* group CCMP */
    ie[8] = 1u;
    ie[9] = 0u;
    ie[10] = 0u;
    ie[11] = 15u;
    ie[12] = 172u;
    ie[13] = 4u;            /* one pairwise CCMP */
    ie[14] = 1u;
    ie[15] = 0u;
    ie[16] = 0u;
    ie[17] = 15u;
    ie[18] = 172u;
    ie[19] = 2u;            /* one AKM PSK */
    if (length >= 20u)
        ie[20] = 0x3Du;     /* only supported RSN capabilities */
    if (length == 22u)
        ie[22] = 0u;        /* no PMKIDs */
}

static void set_protected_with_passphrase(struct TagItem *tags,
                                          const UBYTE *ssid,
                                          const UBYTE *rsn,
                                          const UBYTE *passphrase,
                                          IPTR wpa_ie_extent)
{
    set_required(tags, ssid, rsn, wpa_ie_extent);
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], TAG_DONE, 0u);
}

static void test_open_owned_copy_and_optional_target_fields(void)
{
    UBYTE ssid[33] = "lab-network";
    UBYTE bssid[6] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au };
    struct TagItem tags[8];
    set_tag(&tags[0], S2INFO_SSID, pointer_data(ssid));
    set_tag(&tags[1], S2INFO_BSSID, pointer_data(bssid));
    set_tag(&tags[2], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[3], S2INFO_Channel, (IPTR)14u);
    set_tag(&tags[4], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[5], S2INFO_WPAInfo, 0u); /* explicit open profile */
    set_tag(&tags[6], S2INFO_WPAInfoLength, 0u);
    set_tag(&tags[7], TAG_DONE, 0u);

    expect_success(tags);
    CHECK(target.ssid_length == 11u);
    CHECK(memcmp(target.ssid, "lab-network", 11u) == 0);
    CHECK(target.password_length == 0u && target.authmode == 0u);
    CHECK(target.bssid_set && memcmp(target.bssid, bssid, sizeof(bssid)) == 0);
    CHECK(target.channel == 14u);
    CHECK(bytes_are_zero(target.password, sizeof(target.password)));

    ssid[0] = (UBYTE)'X';
    bssid[0] = 0x04u;
    set_tag(&tags[0], S2INFO_SSID, pointer_data(bssid));
    CHECK(target.ssid[0] == (UBYTE)'l');
    CHECK(target.bssid[0] == 0x02u);
}

static void test_open_profile_may_omit_extent_only(void)
{
    UBYTE rsn[24];
    UBYTE passphrase[] = "correct horse";
    struct TagItem tags[6];

    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"open"));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[3], S2INFO_WPAInfo, 0u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_success(tags);
    CHECK(target.authmode == 0u && target.password_length == 0u);

    set_rsn(rsn, 18u);
    set_tag(&tags[3], S2INFO_WPAInfo, pointer_data(rsn));
    set_tag(&tags[4], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags); /* protected IE still needs an explicit extent */

    set_tag(&tags[3], S2INFO_WPAInfo, 0u);
    set_tag(&tags[4], S2INFO_WPAInfoLength, 1u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags); /* NULL WPAInfo cannot smuggle a nonzero extent */

    set_tag(&tags[4], S2INFO_Passphrase, pointer_data(passphrase));
    expect_refusal(tags); /* open WPAInfo cannot carry a credential */

    set_tag(&tags[3], TAG_DONE, 0u);
    expect_refusal(tags); /* WPAInfo itself remains mandatory */
}

static void test_protected_passphrase_and_string_boundaries(void)
{
    UBYTE rsn[24];
    UBYTE ssid32[33];
    UBYTE pass8[] = "password";
    UBYTE pass63[64];
    struct TagItem tags[7];
    size_t index;

    set_rsn(rsn, 18u);
    set_protected_with_passphrase(tags, (const UBYTE *)"secure-net", rsn,
                                  pass8, 20u);
    expect_success(tags);
    CHECK(target.authmode == 3u && target.password_length == 8u);
    CHECK(memcmp(target.password, "password", 8u) == 0);
    pass8[0] = (UBYTE)'X';
    rsn[0] = 0u;
    CHECK(target.password[0] == (UBYTE)'p');
    CHECK(target.authmode == 3u);

    for (index = 0; index < 32u; ++index)
        ssid32[index] = (UBYTE)'s';
    ssid32[32] = 0u;
    for (index = 0; index < 63u; ++index)
        pass63[index] = (UBYTE)'p';
    pass63[63] = 0u;
    set_rsn(rsn, 20u);
    set_protected_with_passphrase(tags, ssid32, rsn, pass63, 22u);
    expect_success(tags);
    CHECK(target.ssid_length == 32u && target.password_length == 63u);
    CHECK(target.channel == 0u && !target.bssid_set);
    CHECK(memcmp(target.ssid, ssid32, 32u) == 0);
    CHECK(memcmp(target.password, pass63, 63u) == 0);
}

static void test_raw_binary_psk_is_owned_lowercase_hex(void)
{
    static const UBYTE hex[] = "0123456789abcdef";
    UBYTE rsn[24];
    UBYTE psk[32];
    struct TagItem tags[7];
    size_t index;
    set_rsn(rsn, 22u);
    for (index = 0; index < sizeof(psk); ++index)
        psk[index] = (UBYTE)(index * 17u);
    psk[0] = 0u;
    psk[1] = 0xFFu;
    psk[31] = 0u;
    set_required(tags, (const UBYTE *)"raw-key", rsn, 24u);
    set_tag(&tags[5], S2INFO_PSK, pointer_data(psk));
    set_tag(&tags[6], TAG_DONE, 0u);

    expect_success(tags);
    CHECK(target.authmode == 3u && target.password_length == 64u);
    for (index = 0; index < sizeof(psk); ++index)
    {
        CHECK(target.password[index * 2u] == hex[psk[index] >> 4]);
        CHECK(target.password[index * 2u + 1u] == hex[psk[index] & 15u]);
    }
    psk[1] = 0u;
    CHECK(target.password[0] == (UBYTE)'0');
    CHECK(target.password[1] == (UBYTE)'0');
    CHECK(target.password[2] == (UBYTE)'f');
    CHECK(target.password[3] == (UBYTE)'f');
}

static void test_invalid_credentials_and_authentication_combinations(void)
{
    UBYTE rsn[24];
    UBYTE passphrase[] = "correct horse";
    UBYTE raw[32];
    struct TagItem tags[8];

    set_required(tags, (const UBYTE *)"open", NULL, 0u);
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* open profile must not carry a secret */

    set_rsn(rsn, 18u);
    set_required(tags, (const UBYTE *)"protected", rsn, 20u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags); /* protected profile needs one credential */

    memset(raw, 0x5A, sizeof(raw));
    set_required(tags, (const UBYTE *)"open", NULL, 0u);
    set_tag(&tags[5], S2INFO_PSK, pointer_data(raw));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"protected", rsn, 20u);
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], S2INFO_PSK, pointer_data(raw));
    set_tag(&tags[7], TAG_DONE, 0u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"protected", rsn, 20u);
    set_tag(&tags[5], S2INFO_PSK, pointer_data(raw));
    set_tag(&tags[6], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[7], TAG_DONE, 0u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"protected", rsn, 20u);
    set_tag(&tags[5], S2INFO_Passphrase, 0u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);
}

static void expect_bad_rsn(UBYTE rsn[24], ULONG wpa_ie_extent)
{
    UBYTE passphrase[] = "correct horse";
    struct TagItem tags[7];
    set_required(tags, (const UBYTE *)"protected", rsn, wpa_ie_extent);
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);
}

static void test_strict_rsn_suite_and_pmf_pmkid_refusals(void)
{
    UBYTE rsn[24];

    set_rsn(rsn, 17u); expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 19u); expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 21u); expect_bad_rsn(rsn, 22u);
    set_rsn(rsn, 23u); expect_bad_rsn(rsn, 24u);
    set_rsn(rsn, 26u); expect_bad_rsn(rsn, 24u);

    set_rsn(rsn, 18u); rsn[0] = 221u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[2] = 2u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[7] = 2u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[8] = 2u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[13] = 2u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[14] = 2u; expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 18u); rsn[19] = 1u; expect_bad_rsn(rsn, 20u);

    set_rsn(rsn, 20u); rsn[20] = 0x02u; expect_bad_rsn(rsn, 22u); /* no-pairwise */
    set_rsn(rsn, 20u); rsn[20] = 0x40u; expect_bad_rsn(rsn, 22u); /* MFPR */
    set_rsn(rsn, 20u); rsn[20] = 0x80u; expect_bad_rsn(rsn, 22u); /* MFPC */
    set_rsn(rsn, 20u); rsn[21] = 1u; expect_bad_rsn(rsn, 22u);
    set_rsn(rsn, 22u); rsn[22] = 1u; expect_bad_rsn(rsn, 24u); /* PMKID count */
    set_rsn(rsn, 22u); rsn[23] = 1u; expect_bad_rsn(rsn, 24u);
}

static void test_explicit_ie_extent_validation(void)
{
    static const ULONG short_extents[] = { 0u, 1u, 2u, 19u };
    UBYTE rsn[24];
    UBYTE passphrase[] = "correct horse";
    struct TagItem tags[7];
    size_t index;

    /* The allocation is readable for exactly each declared short span (and
     * one byte for extent zero so WPAInfo remains a non-NULL pointer). */
    for (index = 0; index < sizeof(short_extents) / sizeof(short_extents[0]);
         ++index)
    {
        ULONG extent = short_extents[index];
        size_t allocation_size = extent ? (size_t)extent : 1u;
        UBYTE *short_ie = (UBYTE *)malloc(allocation_size);
        CHECK(short_ie != NULL);
        memset(short_ie, 0xA5, allocation_size);
        set_protected_with_passphrase(tags, (const UBYTE *)"protected",
                                      short_ie, passphrase, extent);
        expect_refusal(tags);
        free(short_ie);
    }

    /* An over-limit declaration still points at only one readable byte. */
    {
        UBYTE one_byte_ie = 0xA5u;
        set_protected_with_passphrase(tags, (const UBYTE *)"protected",
                                      &one_byte_ie, passphrase, 258u);
        expect_refusal(tags);
    }

    /* The legal span and IE-internal length are independent inputs. */
    set_rsn(rsn, 18u);
    expect_bad_rsn(rsn, 22u);
    set_rsn(rsn, 20u);
    expect_bad_rsn(rsn, 20u);
    set_rsn(rsn, 22u);
    expect_bad_rsn(rsn, 22u);
    set_rsn(rsn, 18u);
    expect_bad_rsn(rsn, 0u);

    /* A NULL WPAInfo denotes open, so it cannot smuggle in an IE span. */
    set_required(tags, (const UBYTE *)"open", NULL, 1u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags);

    /* Extents are rejected before narrowing to the 32-bit SANA ULONG. */
#if UINTPTR_MAX > UINT32_MAX
    set_required(tags, (const UBYTE *)"open", NULL,
                 (IPTR)UINT32_MAX + 1u);
    CHECK(tags[4].ti_Data > (IPTR)UINT32_MAX);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags);
#endif
}

static void test_wpa_info_length_tag_order(void)
{
    UBYTE rsn[24];
    UBYTE passphrase[] = "correct horse";
    struct TagItem tags[7];

    set_rsn(rsn, 18u);
    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"before"));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[3], S2INFO_WPAInfoLength, (IPTR)20u);
    set_tag(&tags[4], S2INFO_WPAInfo, pointer_data(rsn));
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_success(tags);
    CHECK(target.authmode == 3u && target.password_length == 13u);

    /* The common helper order puts WPAInfo before WPAInfoLength. */
    set_protected_with_passphrase(tags, (const UBYTE *)"after", rsn,
                                  passphrase, 20u);
    expect_success(tags);
    CHECK(target.authmode == 3u && target.password_length == 13u);
}

static void test_ssid_bssid_channel_auth_and_port_validation(void)
{
    UBYTE rsn[24];
    UBYTE long_ssid[34];
    UBYTE zero_bssid[6] = { 0 };
    UBYTE multicast_bssid[6] = { 0x01u, 2u, 3u, 4u, 5u, 6u };
    UBYTE unicast_bssid[6] = { 0x02u, 2u, 3u, 4u, 5u, 6u };
    struct TagItem tags[8];
    size_t index;

    set_rsn(rsn, 18u);
    memset(long_ssid, 'a', 33u);
    long_ssid[33] = 0u;
    set_required(tags, long_ssid, NULL, 0u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags); /* 33-byte name; the entire input remains readable */

    set_required(tags, NULL, NULL, 0u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags);
    set_required(tags, (const UBYTE *)"", NULL, 0u);
    set_tag(&tags[5], TAG_DONE, 0u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_BSSID, 0u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);
    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_BSSID, pointer_data(zero_bssid));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);
    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_BSSID, pointer_data(multicast_bssid));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_BSSID, pointer_data(unicast_bssid));
    set_tag(&tags[6], S2INFO_Channel, (IPTR)0u);
    set_tag(&tags[7], TAG_DONE, 0u);
    expect_success(tags);
    CHECK(target.bssid_set && target.channel == 0u);

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_Channel, (IPTR)15u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);

    for (index = 0; index < 2u; ++index)
    {
        set_required(tags, (const UBYTE *)"valid", NULL, 0u);
        set_tag(&tags[1], S2INFO_PortType,
                (IPTR)(index ? S2PORT_ADHOC : 0u));
        set_tag(&tags[5], TAG_DONE, 0u);
        expect_refusal(tags);
    }
    for (index = 0; index < 2u; ++index)
    {
        IPTR auth_types = index ? 2u : 0u;
        set_required(tags, (const UBYTE *)"valid", NULL, 0u);
        set_tag(&tags[2], S2INFO_AuthTypes, auth_types);
        set_tag(&tags[5], TAG_DONE, 0u);
        expect_refusal(tags);
    }
}

static void test_passphrase_length_and_character_rules(void)
{
    UBYTE rsn[24];
    UBYTE pass7[] = "1234567";
    UBYTE pass64[65];
    UBYTE pass_control[10] = { '1', '2', '3', '4', '5', '6', '7', 1, '9', 0 };
    UBYTE pass_non_ascii[10] = { '1', '2', '3', '4', '5', '6', '7', 127, '9', 0 };
    struct TagItem tags[7];
    size_t index;

    set_rsn(rsn, 18u);
    set_protected_with_passphrase(tags, (const UBYTE *)"protected", rsn,
                                  pass7, 20u);
    expect_refusal(tags);

    memset(pass64, 'x', 64u);
    pass64[64] = 0u;
    set_protected_with_passphrase(tags, (const UBYTE *)"protected", rsn,
                                  pass64, 20u);
    expect_refusal(tags);
    set_protected_with_passphrase(tags, (const UBYTE *)"protected", rsn,
                                  pass_control, 20u);
    expect_refusal(tags);
    set_protected_with_passphrase(tags, (const UBYTE *)"protected", rsn,
                                  pass_non_ascii, 20u);
    expect_refusal(tags);

    set_required(tags, (const UBYTE *)"protected", rsn, 20u);
    set_tag(&tags[5], S2INFO_PSK, 0u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags);

    for (index = 0; index < 8u; ++index)
        CHECK(target.password[index] == 0u);
}

static void test_missing_duplicate_and_unknown_tags(void)
{
    UBYTE rsn[24];
    UBYTE short_ie = 0xA5u;
    UBYTE passphrase[] = "correct horse";
    struct TagItem tags[10];

    set_tag(&tags[0], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[1], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[2], S2INFO_WPAInfo, 0u);
    set_tag(&tags[3], S2INFO_WPAInfoLength, 0u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_refusal(tags); /* no SSID */

    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"valid"));
    set_tag(&tags[1], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[2], S2INFO_WPAInfo, 0u);
    set_tag(&tags[3], S2INFO_WPAInfoLength, 0u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_refusal(tags); /* no port */

    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"valid"));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_WPAInfo, 0u);
    set_tag(&tags[3], S2INFO_WPAInfoLength, 0u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_refusal(tags); /* no auth */

    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"valid"));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[3], S2INFO_WPAInfoLength, 0u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_refusal(tags); /* no WPAInfo */

    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"valid"));
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&tags[3], S2INFO_WPAInfo, pointer_data(&short_ie));
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_refusal(tags); /* missing WPAInfoLength */

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_Disassociate, 0u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* unknown tag */

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_SSID, pointer_data((const UBYTE *)"again"));
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* duplicate SSID */

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_WPAInfo, 0u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* duplicate WPAInfo */

    set_required(tags, (const UBYTE *)"valid", &short_ie, 20u);
    set_tag(&tags[5], S2INFO_WPAInfoLength, 20u);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* duplicate WPAInfoLength */

    set_required(tags, (const UBYTE *)"valid", NULL, 0u);
    set_tag(&tags[5], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[6], TAG_DONE, 0u);
    expect_refusal(tags); /* duplicate port */

    set_rsn(rsn, 18u);
    set_required(tags, (const UBYTE *)"valid", rsn, 20u);
    set_tag(&tags[5], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[6], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&tags[7], TAG_DONE, 0u);
    expect_refusal(tags); /* duplicate credential */
}

static void test_tag_list_traversal_and_bounded_failure(void)
{
    UBYTE rsn[24];
    UBYTE passphrase[] = "correct horse";
    struct TagItem first[5];
    struct TagItem second[3];
    struct TagItem more_null[6];
    struct TagItem skip_valid[8];
    struct TagItem exact_budget[128];
    struct TagItem beyond_budget[129];
    struct TagItem cycle[1];
    struct TagItem skip_overflow[1];
    struct TagItem skip_budget[129];
    size_t index;

    set_rsn(rsn, 18u);
    set_tag(&first[0], S2INFO_SSID, pointer_data((const UBYTE *)"linked"));
    set_tag(&first[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&first[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&first[3], S2INFO_WPAInfo, pointer_data(rsn));
    set_tag(&first[4], TAG_MORE, pointer_data(second));
    set_tag(&second[0], S2INFO_WPAInfoLength, (IPTR)20u);
    set_tag(&second[1], S2INFO_Passphrase, pointer_data(passphrase));
    set_tag(&second[2], TAG_DONE, 0u);
    expect_success(first);
    CHECK(target.ssid_length == 6u && target.authmode == 3u);

    set_tag(&more_null[0], S2INFO_SSID, pointer_data((const UBYTE *)"null-more"));
    set_tag(&more_null[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&more_null[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&more_null[3], S2INFO_WPAInfo, 0u);
    set_tag(&more_null[4], S2INFO_WPAInfoLength, 0u);
    set_tag(&more_null[5], TAG_MORE, 0u);
    expect_success(more_null);

    set_tag(&skip_valid[0], S2INFO_SSID, pointer_data((const UBYTE *)"skip"));
    set_tag(&skip_valid[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&skip_valid[2], S2INFO_AuthTypes, (IPTR)1u);
    set_tag(&skip_valid[3], S2INFO_WPAInfoLength, 0u);
    set_tag(&skip_valid[4], TAG_SKIP, (IPTR)1u);
    set_tag(&skip_valid[5], S2INFO_Disassociate, 0u);
    set_tag(&skip_valid[6], S2INFO_WPAInfo, 0u);
    set_tag(&skip_valid[7], TAG_DONE, 0u);
    expect_success(skip_valid);

    set_tag(&cycle[0], TAG_MORE, pointer_data(cycle));
    expect_refusal(cycle);

    set_tag(&skip_overflow[0], TAG_SKIP, (IPTR)~(IPTR)0);
    expect_refusal(skip_overflow);

    for (index = 0; index < sizeof(skip_budget) / sizeof(skip_budget[0]);
         ++index)
        set_tag(&skip_budget[index], TAG_DONE, 0u);
    set_tag(&skip_budget[0], TAG_SKIP, (IPTR)127u);
    expect_refusal(skip_budget); /* consumes all 128 traversal slots */

    for (index = 0; index < sizeof(exact_budget) / sizeof(exact_budget[0]);
         ++index)
        set_tag(&exact_budget[index], TAG_IGNORE, 0u);
    set_required(exact_budget, (const UBYTE *)"exact", NULL, 0u);
    set_tag(&exact_budget[127], TAG_DONE, 0u);
    expect_success(exact_budget);
    CHECK(target.ssid_length == 5u && target.authmode == 0u);

    for (index = 0; index < sizeof(beyond_budget) / sizeof(beyond_budget[0]);
         ++index)
        set_tag(&beyond_budget[index], TAG_IGNORE, 0u);
    set_required(beyond_budget, (const UBYTE *)"beyond", NULL, 0u);
    set_tag(&beyond_budget[128], TAG_DONE, 0u);
    expect_refusal(beyond_budget);
}

static void test_null_target_refusal(void)
{
    struct TagItem done[1];
    set_tag(&done[0], TAG_DONE, 0u);
    ++cases;
    CHECK(!p4wlan_copy_association_options(NULL, done));
}

static void expect_disassociate_success(const struct TagItem *tags)
{
    ++cases;
    CHECK(p4wlan_disassociate_options(tags));
}

static void expect_disassociate_refusal(const struct TagItem *tags)
{
    ++cases;
    CHECK(!p4wlan_disassociate_options(tags));
}

static void test_disassociate_option_contract(void)
{
    struct TagItem plain[2];
    struct TagItem ignored[3];
    struct TagItem first[1];
    struct TagItem second[2];
    struct TagItem skip_with_done[4];
    struct TagItem mixed[3];
    struct TagItem duplicate[3];
    struct TagItem false_value[2];
    struct TagItem done_only[1];
    struct TagItem cycle[1];
    struct TagItem traversal_limit[128];
    size_t index;

    set_tag(&plain[0], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&plain[1], TAG_DONE, 0u);
    expect_disassociate_success(plain);

    set_tag(&ignored[0], TAG_IGNORE, 0u);
    set_tag(&ignored[1], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&ignored[2], TAG_DONE, 0u);
    expect_disassociate_success(ignored);

    set_tag(&first[0], TAG_MORE, pointer_data(second));
    set_tag(&second[0], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&second[1], TAG_DONE, 0u);
    expect_disassociate_success(first);

    set_tag(&skip_with_done[0], TAG_SKIP, (IPTR)1u);
    set_tag(&skip_with_done[1], S2INFO_SSID,
            pointer_data((const UBYTE *)"must-be-skipped"));
    set_tag(&skip_with_done[2], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&skip_with_done[3], TAG_DONE, 0u);
    expect_disassociate_success(skip_with_done);

    set_tag(&mixed[0], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&mixed[1], S2INFO_SSID,
            pointer_data((const UBYTE *)"new-profile"));
    set_tag(&mixed[2], TAG_DONE, 0u);
    expect_disassociate_refusal(mixed);

    set_tag(&duplicate[0], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&duplicate[1], S2INFO_Disassociate, (IPTR)TRUE);
    set_tag(&duplicate[2], TAG_DONE, 0u);
    expect_disassociate_refusal(duplicate);

    set_tag(&false_value[0], S2INFO_Disassociate, 0u);
    set_tag(&false_value[1], TAG_DONE, 0u);
    expect_disassociate_refusal(false_value);

    set_tag(&done_only[0], TAG_DONE, 0u);
    expect_disassociate_refusal(done_only);
    expect_disassociate_refusal(NULL);

    set_tag(&cycle[0], TAG_MORE, pointer_data(cycle));
    expect_disassociate_refusal(cycle);

    for (index = 0u; index < sizeof traversal_limit / sizeof traversal_limit[0];
         ++index)
        set_tag(&traversal_limit[index], TAG_IGNORE, 0u);
    expect_disassociate_refusal(traversal_limit);
}

static void test_association_cookie_metadata(void)
{
    struct TagItem tags[8];
    ULONG cookie = 123;
    set_required(tags, (const UBYTE *)"cookie-test", NULL, 0);
    set_tag(&tags[5], S2INFO_AssociationCookie, 1);
    set_tag(&tags[6], TAG_DONE, 0);
    ++cases;
    CHECK(p4wlan_copy_association_options_cookie(&target, &cookie, tags));
    CHECK(cookie == 1);
    CHECK(target.ssid_length == 11);
    set_tag(&tags[5], S2INFO_AssociationCookie, (IPTR)~(ULONG)0);
    CHECK(p4wlan_copy_association_options_cookie(&target, &cookie, tags));
    CHECK(cookie == ~(ULONG)0);
    expect_refusal(tags); /* legacy path cannot drop correlation metadata */
    set_tag(&tags[5], S2INFO_AssociationCookie, 0);
    CHECK(!p4wlan_copy_association_options_cookie(&target, &cookie, tags));
    CHECK(cookie == 0 && bytes_are_zero((const UBYTE *)&target, sizeof(target)));
    set_tag(&tags[5], S2INFO_AssociationCookie, 17);
    set_tag(&tags[6], S2INFO_AssociationCookie, 18);
    set_tag(&tags[7], TAG_DONE, 0);
    CHECK(!p4wlan_copy_association_options_cookie(&target, &cookie, tags));
    CHECK(cookie == 0 && bytes_are_zero((const UBYTE *)&target, sizeof(target)));
    if (sizeof(IPTR) > sizeof(ULONG)) {
        set_tag(&tags[5], S2INFO_AssociationCookie, (IPTR)~(ULONG)0 + (IPTR)1);
        set_tag(&tags[6], TAG_DONE, 0);
        CHECK(!p4wlan_copy_association_options_cookie(&target, &cookie, tags));
        CHECK(cookie == 0);
    }
    set_tag(&tags[5], TAG_DONE, 0);
    CHECK(p4wlan_copy_association_options_cookie(&target, &cookie, tags));
    CHECK(cookie == 0); /* optional only until capability negotiation */
    cookie = 123;
    CHECK(!p4wlan_copy_association_options_cookie(NULL, &cookie, tags));
    CHECK(cookie == 0);
}

static void set_fullmac_caps(struct Sana2FullMACCapabilities *caps,
                             ULONG profiles, ULONG pwe_methods, ULONG flags)
{
    memset(caps, 0, sizeof(*caps));
    caps->SizeAvailable = sizeof(*caps);
    caps->SizeSupplied = sizeof(*caps);
    caps->Version = S2FULLMAC_VERSION;
    caps->Flags = flags;
    caps->Profiles = profiles;
    caps->SAEPweMethods = pwe_methods;
}

static void set_fullmac_selection(struct Sana2FullMACAssociation *selection,
                                  ULONG profile, ULONG pmf, ULONG pwe,
                                  ULONG flags)
{
    memset(selection, 0, sizeof(*selection));
    selection->Size = sizeof(*selection);
    selection->Version = S2FULLMAC_VERSION;
    selection->Profile = profile;
    selection->Flags = flags;
    selection->PMFPolicy = pmf;
    selection->SAEPweMethod = pwe;
}

static void set_wireless_credential(struct Sana2WirelessCredential *credential,
                                    ULONG kind, const UBYTE *data,
                                    ULONG length)
{
    memset(credential, 0, sizeof(*credential));
    credential->Size = sizeof(*credential);
    credential->Version = S2WIRELESS_CREDENTIAL_VERSION;
    credential->Kind = kind;
    credential->Length = length;
    credential->Data = data;
}

static size_t set_fullmac_tags(struct TagItem *tags, const UBYTE *ssid,
                               const struct Sana2FullMACAssociation *selection,
                               const struct Sana2WirelessCredential *credential,
                               IPTR cookie)
{
    size_t count = 0;
    set_tag(&tags[count++], S2INFO_SSID, pointer_data(ssid));
    set_tag(&tags[count++], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[count++], S2INFO_FullMACAssociation,
            pointer_data(selection));
    if (credential)
        set_tag(&tags[count++], S2INFO_WirelessCredential,
                pointer_data(credential));
    set_tag(&tags[count++], S2INFO_AssociationCookie, cookie);
    set_tag(&tags[count++], TAG_DONE, 0u);
    return count;
}

static void expect_fullmac_success(const struct TagItem *tags,
                                   const struct Sana2FullMACCapabilities *caps,
                                   ULONG *cookie)
{
    BOOL accepted;
    ++cases;
    poison_target();
    *cookie = 0xA5A5A5A5u;
    accepted = p4wlan_copy_fullmac_association_options(&target, cookie,
                                                       tags, caps);
    if (!accepted)
        fprintf(stderr, "unexpected FullMAC refusal in case %lu\n", cases);
    CHECK(accepted);
}

static void expect_fullmac_refusal(const struct TagItem *tags,
                                   const struct Sana2FullMACCapabilities *caps,
                                   ULONG *cookie)
{
    ++cases;
    poison_target();
    *cookie = 0xA5A5A5A5u;
    CHECK(!p4wlan_copy_fullmac_association_options(&target, cookie,
                                                    tags, caps));
    CHECK(*cookie == 0u);
    CHECK(bytes_are_zero((const UBYTE *)&target, sizeof(target)));
}

static void test_fullmac_open_psk_and_owned_copy(void)
{
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE ssid[] = "typed-open";
    UBYTE bssid[6] = { 0x02u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au };
    UBYTE passphrase[] = "password";
    UBYTE passphrase63[64];
    UBYTE psk[32];
    struct TagItem tags[8];
    ULONG cookie;
    size_t count, index;
    static const UBYTE hex[] = "0123456789abcdef";

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_OPEN |
                     S2FULLMAC_PROFILE_WPA2_CCMP |
                     S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
                     S2FULLMAC_SAE_PWE_H2E |
                     S2FULLMAC_SAE_PWE_BOTH, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
                          S2FULLMAC_PMF_DISABLED, 0u, 0u);
    count = set_fullmac_tags(tags, ssid, &selection, NULL, 0x10203040u);
    set_tag(&tags[1], S2INFO_BSSID, pointer_data(bssid));
    set_tag(&tags[2], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[3], S2INFO_Channel, 11u);
    set_tag(&tags[4], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&tags[5], S2INFO_AssociationCookie, 0x10203040u);
    set_tag(&tags[6], TAG_DONE, 0u);
    (void)count;
    expect_fullmac_success(tags, &caps, &cookie);
    CHECK(cookie == 0x10203040u);
    CHECK(target.ssid_length == 10u &&
          memcmp(target.ssid, "typed-open", 10u) == 0);
    CHECK(target.bssid_set && memcmp(target.bssid, bssid, sizeof(bssid)) == 0);
    CHECK(target.channel == 11u && target.authmode == 0u);
    CHECK(target.fullmac_profile == S2FULLMAC_PROFILE_OPEN);
    CHECK(!target.pmf_required && target.sae_pwe_h2e == 0u);
    CHECK(!target.diagnostic_risk_accepted && target.password_length == 0u);
    CHECK(bytes_are_zero(target.password, sizeof(target.password)));
    ssid[0] = (UBYTE)'X';
    bssid[0] = 0x04u;
    selection.Profile = S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    CHECK(target.ssid[0] == (UBYTE)'t' && target.bssid[0] == 0x02u);
    CHECK(target.fullmac_profile == S2FULLMAC_PROFILE_OPEN);

    /* WPA2 passphrase and raw binary PSK retain their legacy target encoding. */
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_WPA2_CCMP,
                          S2FULLMAC_PMF_DISABLED, 0u, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_WPA2_PASSPHRASE,
                            passphrase, 8u);
    count = set_fullmac_tags(tags, (const UBYTE *)"typed-psk", &selection,
                             &credential, 9u);
    expect_fullmac_success(tags, &caps, &cookie);
    CHECK(cookie == 9u && target.authmode == 3u);
    CHECK(target.fullmac_profile == S2FULLMAC_PROFILE_WPA2_CCMP);
    CHECK(!target.pmf_required && target.sae_pwe_h2e == 0u);
    CHECK(target.password_length == 8u &&
          memcmp(target.password, "password", 8u) == 0);
    passphrase[0] = (UBYTE)'X';
    CHECK(target.password[0] == (UBYTE)'p');

    memset(passphrase63, 'q', 63u);
    passphrase63[63] = 0u;
    set_wireless_credential(&credential, S2WIRELESS_CRED_WPA2_PASSPHRASE,
                            passphrase63, 63u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"typed-psk-maximum", &selection,
                           &credential, 11u);
    expect_fullmac_success(tags, &caps, &cookie);
    CHECK(cookie == 11u && target.password_length == 63u);
    CHECK(memcmp(target.password, passphrase63, 63u) == 0);
    passphrase63[0] = (UBYTE)'X';
    CHECK(target.password[0] == (UBYTE)'q');

    for (index = 0; index < sizeof(psk); ++index)
        psk[index] = (UBYTE)(index * 17u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_WPA2_PSK,
                            psk, sizeof(psk));
    (void)set_fullmac_tags(tags, (const UBYTE *)"typed-raw-psk", &selection,
                           &credential, 10u);
    expect_fullmac_success(tags, &caps, &cookie);
    CHECK(cookie == 10u && target.authmode == 3u);
    CHECK(target.password_length == 64u);
    for (index = 0; index < sizeof(psk); ++index)
    {
        CHECK(target.password[index * 2u] == hex[psk[index] >> 4]);
        CHECK(target.password[index * 2u + 1u] == hex[psk[index] & 15u]);
    }
}

static void test_fullmac_sae_password_and_pwe_mapping(void)
{
    static const ULONG methods[] = {
        S2FULLMAC_SAE_PWE_HUNT_AND_PECK,
        S2FULLMAC_SAE_PWE_H2E,
        S2FULLMAC_SAE_PWE_BOTH
    };
    static const ULONG idf_modes[] = { 1u, 2u, 3u };
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE password63[63];
    UBYTE password1[] = { 'x' };
    struct TagItem tags[8];
    ULONG cookie;
    size_t i;

    memset(password63, 's', sizeof(password63));
    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
                     S2FULLMAC_SAE_PWE_H2E |
                     S2FULLMAC_SAE_PWE_BOTH, 0u);
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
    {
        UBYTE *password = i == 0u ? password1 : password63;
        ULONG length = i == 0u ? sizeof(password1) : sizeof(password63);
        UBYTE first_byte = password[0];
        set_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                              S2FULLMAC_PMF_REQUIRED, methods[i], 0u);
        set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                                password, length);
        (void)set_fullmac_tags(tags, (const UBYTE *)"typed-sae", &selection,
                               &credential, (IPTR)(0x20u + i));
        expect_fullmac_success(tags, &caps, &cookie);
        CHECK(cookie == 0x20u + i && target.authmode == 6u);
        CHECK(target.fullmac_profile == S2FULLMAC_PROFILE_SAE_CCMP_PMF);
        CHECK(target.pmf_required == 1u &&
              target.sae_pwe_h2e == idf_modes[i]);
        CHECK(!target.diagnostic_risk_accepted);
        CHECK(target.password_length == length);
        CHECK(memcmp(target.password, password, length) == 0);
        password[0] = (UBYTE)'X';
        CHECK(target.password[0] == first_byte);
        password[0] = first_byte;
    }
}

static void test_fullmac_diagnostic_optin_and_profile_refusals(void)
{
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE password[] = "sae-pass";
    struct TagItem tags[8];
    ULONG cookie;
    size_t count;

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E,
                     S2FULLMAC_CAPF_DIAGNOSTIC_RISK);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                          S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, sizeof(password) - 1u);
    count = set_fullmac_tags(tags, (const UBYTE *)"diagnostic", &selection,
                             &credential, 77u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* no implicit opt-in */

    selection.Flags = S2FULLMAC_ASSOCF_ALLOW_DIAGNOSTIC_RISK;
    expect_fullmac_success(tags, &caps, &cookie);
    CHECK(cookie == 77u && target.diagnostic_risk_accepted == 1u);

    caps.Flags = 0u;
    selection.Flags = 0x80000000u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.Flags = 0u;
    selection.Profile = 8u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.Profile = S2FULLMAC_PROFILE_SAE_CCMP_PMF |
                        S2FULLMAC_PROFILE_WPA2_CCMP;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.Profile = S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    selection.PMFPolicy = S2FULLMAC_PMF_DISABLED;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.PMFPolicy = S2FULLMAC_PMF_REQUIRED;
    selection.PMFPolicy = 2u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.PMFPolicy = S2FULLMAC_PMF_REQUIRED;
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_HUNT_AND_PECK |
                             S2FULLMAC_SAE_PWE_H2E;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_HUNT_AND_PECK;
    expect_fullmac_refusal(tags, &caps, &cookie); /* unadvertised PWE */
    selection.SAEPweMethod = S2FULLMAC_SAE_PWE_H2E;
    caps.Profiles = S2FULLMAC_PROFILE_WPA2_CCMP;
    caps.SAEPweMethods = 0u;
    expect_fullmac_refusal(tags, &caps, &cookie); /* profile not advertised */
    caps.Profiles = S2FULLMAC_PROFILE_SAE_CCMP_PMF;
    caps.SAEPweMethods = S2FULLMAC_SAE_PWE_H2E;
    selection.Size--;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.Size = sizeof(selection);
    selection.Version++;
    expect_fullmac_refusal(tags, &caps, &cookie);
    selection.Version = S2FULLMAC_VERSION;
    selection.Reserved = 1u;
    expect_fullmac_refusal(tags, &caps, &cookie);

    (void)count;
}

static void test_fullmac_capability_and_credential_refusals(void)
{
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE password[64];
    UBYTE psk[32];
    struct TagItem tags[8];
    ULONG cookie;

    memset(password, 's', sizeof(password));
    memset(psk, 0x5a, sizeof(psk));
    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                          S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, 1u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"invalid-caps", &selection,
                           &credential, 12u);

#define BAD_CAPS(change) do { \
    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF, \
                     S2FULLMAC_SAE_PWE_H2E, 0u); \
    change; \
    expect_fullmac_refusal(tags, &caps, &cookie); \
} while (0)
    BAD_CAPS(caps.SizeAvailable--);
    BAD_CAPS(caps.SizeSupplied--);
    BAD_CAPS(caps.Version++);
    BAD_CAPS(caps.Flags = 0x80000000u);
    BAD_CAPS(caps.Profiles = 0u);
    BAD_CAPS(caps.Profiles |= 0x80000000u);
    BAD_CAPS(caps.SAEPweMethods = 0u);
    BAD_CAPS(caps.SAEPweMethods |= 0x80000000u);
    BAD_CAPS(caps.Reserved[0] = 1u);
    BAD_CAPS(caps.Reserved[1] = 1u);
#undef BAD_CAPS

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, 0u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"bad-len", &selection,
                           &credential, 13u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    (void)set_fullmac_tags(tags, (const UBYTE *)"missing-secret", &selection,
                           NULL, 13u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Length = 64u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Length = 1u;
    password[0] = 0u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    password[0] = 'a';
    password[1] = 0u;
    password[2] = 'b';
    credential.Length = 3u;
    expect_fullmac_refusal(tags, &caps, &cookie); /* embedded NUL */
    password[0] = 's';
    password[1] = 's';
    password[2] = 's';
    credential.Length = 1u;
    credential.Kind = S2WIRELESS_CRED_WPA2_PASSPHRASE;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Kind = S2WIRELESS_CRED_WPA2_PSK;
    credential.Length = sizeof(psk);
    credential.Data = psk;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Kind = S2WIRELESS_CRED_SAE_PASSWORD;
    credential.Length = 1u;
    credential.Data = password;
    credential.Size--;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Size = sizeof(credential);
    credential.Version++;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Version = S2WIRELESS_CREDENTIAL_VERSION;
    credential.Data = NULL;
    expect_fullmac_refusal(tags, &caps, &cookie);

    /* OPEN rejects any credential. */
    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_OPEN |
                     S2FULLMAC_PROFILE_WPA2_CCMP |
                     S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
                          S2FULLMAC_PMF_DISABLED, 0u, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, 1u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"open-with-secret", &selection,
                           &credential, 15u);
    expect_fullmac_refusal(tags, &caps, &cookie);

    /* WPA2 accepts printable passphrases or exactly 32 raw PSK bytes only. */
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_WPA2_CCMP,
                          S2FULLMAC_PMF_DISABLED, 0u, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, 8u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"wpa2-wrong-kind", &selection,
                           &credential, 14u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_wireless_credential(&credential, S2WIRELESS_CRED_WPA2_PASSPHRASE,
                            password, 7u);
    (void)set_fullmac_tags(tags, (const UBYTE *)"wpa2-len", &selection,
                           &credential, 14u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Length = 64u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Length = 8u;
    password[7] = 1u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    password[7] = 's';
    credential.Kind = S2WIRELESS_CRED_WPA2_PSK;
    credential.Data = psk;
    credential.Length = 31u;
    expect_fullmac_refusal(tags, &caps, &cookie);
    credential.Length = 33u;
    expect_fullmac_refusal(tags, &caps, &cookie);
}

static void test_fullmac_tag_contract_and_failure_wipe(void)
{
    struct Sana2FullMACCapabilities caps;
    struct Sana2FullMACAssociation selection;
    struct Sana2WirelessCredential credential;
    UBYTE password[] = "sae-pass";
    struct TagItem tags[10];
    struct TagItem cycle[1];
    struct TagItem more_first[4];
    struct TagItem more_second[4];
    struct TagItem optional_tags[8];
    UBYTE valid_bssid[6] = { 0x02u, 1u, 2u, 3u, 4u, 5u };
    ULONG cookie;
    size_t count;
    static const ULONG legacy_tags[] = {
        S2INFO_AuthTypes, S2INFO_WPAInfo, S2INFO_WPAInfoLength,
        S2INFO_Passphrase, S2INFO_PSK
    };
    size_t i;

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                          S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, sizeof(password) - 1u);
    count = set_fullmac_tags(tags, (const UBYTE *)"tags", &selection,
                             &credential, 21u);
    expect_refusal(tags); /* legacy entry rejects typed FullMAC tags */
    for (i = 0; i < sizeof(legacy_tags) / sizeof(legacy_tags[0]); ++i)
    {
        set_tag(&tags[count - 1u], legacy_tags[i], 0u);
        set_tag(&tags[count], TAG_DONE, 0u);
        expect_fullmac_refusal(tags, &caps, &cookie);
        set_tag(&tags[count - 1u], TAG_DONE, 0u);
    }

    set_tag(&tags[count - 1u], S2INFO_SSID,
            pointer_data((const UBYTE *)"duplicate"));
    set_tag(&tags[count], TAG_DONE, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_tag(&tags[count - 1u], S2INFO_WirelessCredential,
            pointer_data(&credential));
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_tag(&tags[count - 1u], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_tag(&tags[count - 1u], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_tag(&tags[count - 1u], S2INFO_AssociationCookie, 22u);
    expect_fullmac_refusal(tags, &caps, &cookie);
    set_tag(&tags[count - 1u], S2INFO_Disassociate, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* unknown tag */

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_OPEN |
                     S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_OPEN,
                          S2FULLMAC_PMF_DISABLED, 0u, 0u);
    set_tag(&optional_tags[0], S2INFO_SSID,
            pointer_data((const UBYTE *)"optional"));
    set_tag(&optional_tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&optional_tags[2], S2INFO_BSSID, pointer_data(valid_bssid));
    set_tag(&optional_tags[3], S2INFO_Channel, 6u);
    set_tag(&optional_tags[4], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&optional_tags[5], S2INFO_AssociationCookie, 31u);
    set_tag(&optional_tags[6], S2INFO_BSSID, pointer_data(valid_bssid));
    set_tag(&optional_tags[7], TAG_DONE, 0u);
    expect_fullmac_refusal(optional_tags, &caps, &cookie); /* duplicate BSSID */
    set_tag(&optional_tags[6], S2INFO_Channel, 6u);
    expect_fullmac_refusal(optional_tags, &caps, &cookie); /* duplicate channel */

    set_tag(&more_first[0], S2INFO_SSID,
            pointer_data((const UBYTE *)"more"));
    set_tag(&more_first[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&more_first[2], TAG_MORE, pointer_data(more_second));
    set_tag(&more_second[0], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&more_second[1], S2INFO_AssociationCookie, 32u);
    set_tag(&more_second[2], TAG_DONE, 0u);
    expect_fullmac_success(more_first, &caps, &cookie);
    CHECK(cookie == 32u && target.ssid_length == 4u);

    set_tag(&tags[0], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[1], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&tags[2], S2INFO_WirelessCredential,
            pointer_data(&credential));
    set_tag(&tags[3], S2INFO_AssociationCookie, 1u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* no SSID */
    set_tag(&tags[0], S2INFO_SSID, pointer_data((const UBYTE *)"missing"));
    set_tag(&tags[1], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&tags[2], S2INFO_WirelessCredential,
            pointer_data(&credential));
    set_tag(&tags[3], S2INFO_AssociationCookie, 1u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* no managed port */
    set_tag(&tags[1], S2INFO_PortType, (IPTR)S2PORT_MANAGED);
    set_tag(&tags[2], S2INFO_WirelessCredential,
            pointer_data(&credential));
    set_tag(&tags[3], S2INFO_AssociationCookie, 1u);
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* no profile */
    set_tag(&tags[2], S2INFO_FullMACAssociation,
            pointer_data(&selection));
    set_tag(&tags[3], S2INFO_WirelessCredential,
            pointer_data(&credential));
    set_tag(&tags[4], TAG_DONE, 0u);
    expect_fullmac_refusal(tags, &caps, &cookie); /* no cookie */

    set_fullmac_caps(&caps, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                     S2FULLMAC_SAE_PWE_H2E, 0u);
    set_fullmac_selection(&selection, S2FULLMAC_PROFILE_SAE_CCMP_PMF,
                          S2FULLMAC_PMF_REQUIRED, S2FULLMAC_SAE_PWE_H2E, 0u);
    set_wireless_credential(&credential, S2WIRELESS_CRED_SAE_PASSWORD,
                            password, sizeof(password) - 1u);
    count = set_fullmac_tags(tags, (const UBYTE *)"zero-cookie", &selection,
                             &credential, 0u);
    (void)count;
    expect_fullmac_refusal(tags, &caps, &cookie);
#if UINTPTR_MAX > UINT32_MAX
    count = set_fullmac_tags(tags, (const UBYTE *)"wide-cookie", &selection,
                             &credential, (IPTR)UINT32_MAX + 1u);
    (void)count;
    expect_fullmac_refusal(tags, &caps, &cookie);
#endif

    set_tag(&cycle[0], TAG_MORE, pointer_data(cycle));
    expect_fullmac_refusal(cycle, &caps, &cookie);

    ++cases;
    poison_target();
    cookie = 99u;
    CHECK(!p4wlan_copy_fullmac_association_options(&target, NULL, tags, &caps));
    CHECK(bytes_are_zero((const UBYTE *)&target, sizeof(target)));
    ++cases;
    CHECK(!p4wlan_copy_fullmac_association_options(NULL, &cookie, tags, &caps));
    CHECK(cookie == 0u);
}

int main(void)
{
    test_open_owned_copy_and_optional_target_fields();
    test_open_profile_may_omit_extent_only();
    test_protected_passphrase_and_string_boundaries();
    test_raw_binary_psk_is_owned_lowercase_hex();
    test_invalid_credentials_and_authentication_combinations();
    test_strict_rsn_suite_and_pmf_pmkid_refusals();
    test_explicit_ie_extent_validation();
    test_wpa_info_length_tag_order();
    test_ssid_bssid_channel_auth_and_port_validation();
    test_passphrase_length_and_character_rules();
    test_missing_duplicate_and_unknown_tags();
    test_tag_list_traversal_and_bounded_failure();
    test_null_target_refusal();
    test_disassociate_option_contract();
    test_association_cookie_metadata();
    test_fullmac_open_psk_and_owned_copy();
    test_fullmac_sae_password_and_pwe_mapping();
    test_fullmac_diagnostic_optin_and_profile_refusals();
    test_fullmac_capability_and_credential_refusals();
    test_fullmac_tag_contract_and_failure_wipe();
    printf("PASS: %lu cases, %lu checks\n", cases, checks);
    return 0;
}
