/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_init.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned int)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static const uint8_t observed_init[] = {
    P4_HOSTED_INIT_EVENT_TYPE, 6,
    P4_HOSTED_INIT_TAG_CAPABILITIES, 1, 0x0d,
    P4_HOSTED_INIT_TAG_CHIP_ID, 1, P4_HOSTED_INIT_CHIP_ESP32C6
};

/* Optional TLVs exercise every field. Chip and capability values match the
 * captured C6 INIT; other values are structurally valid protocol examples. */
static const uint8_t full_init[] = {
    P4_HOSTED_INIT_EVENT_TYPE, 21,
    P4_HOSTED_INIT_TAG_CAPABILITIES, 1, 0x0d,
    P4_HOSTED_INIT_TAG_CHIP_ID, 1, P4_HOSTED_INIT_CHIP_ESP32C6,
    P4_HOSTED_INIT_TAG_TEST_RAW_TP, 1, 0,
    P4_HOSTED_INIT_TAG_RX_QUEUE_SIZE, 1, 20,
    P4_HOSTED_INIT_TAG_TX_QUEUE_SIZE, 1, 20,
    P4_HOSTED_INIT_TAG_EXTENDED_CAPABILITIES, 4, 0x10, 0, 0, 0
};

/* Discovery appears before the required legacy fields in this fixture to
 * verify that capability interpretation is independent of TLV order. */
static const uint8_t discovery_first_init[] = {
    P4_HOSTED_INIT_EVENT_TYPE, 9,
    P4_HOSTED_INIT_TAG_BOARD_DISCOVERY, 1,
    P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT,
    P4_HOSTED_INIT_TAG_CHIP_ID, 1, P4_HOSTED_INIT_CHIP_ESP32C6,
    P4_HOSTED_INIT_TAG_CAPABILITIES, 1, 0x0d
};

static const uint8_t discovery_last_init[] = {
    P4_HOSTED_INIT_EVENT_TYPE, 9,
    P4_HOSTED_INIT_TAG_CAPABILITIES, 1, 0x0d,
    P4_HOSTED_INIT_TAG_CHIP_ID, 1, P4_HOSTED_INIT_CHIP_ESP32C6,
    P4_HOSTED_INIT_TAG_BOARD_DISCOVERY, 1,
    P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT
};

static uint32_t random_state = UINT32_C(0x91e10da5);

static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void expect_refusal(const uint8_t *event, size_t event_size)
{
    struct p4_hosted_init result;
    uint8_t before[sizeof result];

    memset(&result, 0xa7, sizeof result);
    memcpy(before, &result, sizeof result);
    if (p4_hosted_init_parse(event, event_size, &result)) {
        fprintf(stderr, "unexpected parser acceptance, size=%lu\n",
                (unsigned long)event_size);
        exit(1);
    }
    CHECK(memcmp(&result, before, sizeof result) == 0);
}

static void test_observed_c6_event(void)
{
    struct p4_hosted_init result;

    memset(&result, 0xa5, sizeof result);
    CHECK(p4_hosted_init_parse(observed_init, sizeof observed_init, &result));
    CHECK(result.chip_id == P4_HOSTED_INIT_CHIP_ESP32C6);
    CHECK(result.capabilities == 0x0d);
    CHECK((result.capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO) != 0);
    CHECK(result.present == (P4_HOSTED_INIT_HAS_CAPABILITIES |
                             P4_HOSTED_INIT_HAS_CHIP_ID));
    CHECK(!p4_hosted_init_has_board_discovery(&result));
}

static void test_board_discovery_capability(void)
{
    struct p4_hosted_init result;
    uint8_t event[sizeof discovery_last_init];

    CHECK(p4_hosted_init_parse(discovery_first_init,
                               sizeof discovery_first_init, &result));
    CHECK(result.board_discovery_version ==
          P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT);
    CHECK((result.present & P4_HOSTED_INIT_HAS_BOARD_DISCOVERY) != 0);
    CHECK(p4_hosted_init_has_board_discovery(&result));

    CHECK(p4_hosted_init_parse(discovery_last_init,
                               sizeof discovery_last_init, &result));
    CHECK(p4_hosted_init_has_board_discovery(&result));

    /* Preserve zero and unknown versions as inventory facts without enabling
     * the version-2 capability. */
    memcpy(event, discovery_last_init, sizeof event);
    event[sizeof event - 1u] = 0;
    CHECK(p4_hosted_init_parse(event, sizeof event, &result));
    CHECK(result.board_discovery_version == 0);
    CHECK((result.present & P4_HOSTED_INIT_HAS_BOARD_DISCOVERY) != 0);
    CHECK(!p4_hosted_init_has_board_discovery(&result));

    memcpy(event, discovery_last_init, sizeof event);
    event[sizeof event - 1u] = 1;
    CHECK(p4_hosted_init_parse(event, sizeof event, &result));
    CHECK(result.board_discovery_version == 1);
    CHECK(!p4_hosted_init_has_board_discovery(&result));

    memcpy(event, discovery_last_init, sizeof event);
    event[7] = P4_HOSTED_INIT_CHIP_ESP32C3;
    CHECK(p4_hosted_init_parse(event, sizeof event, &result));
    CHECK(result.board_discovery_version ==
          P4_HOSTED_INIT_BOARD_DISCOVERY_VERSION_CURRENT);
    CHECK(!p4_hosted_init_has_board_discovery(&result));
    CHECK(!p4_hosted_init_has_board_discovery(NULL));
}

static void test_all_known_fields_and_unknown_tag(void)
{
    uint8_t event[P4_HOSTED_INIT_MAX_EVENT_SIZE];
    struct p4_hosted_init result;
    size_t size = sizeof full_init;

    memcpy(event, full_init, size);
    event[size++] = 0x70; /* Future tag, ignored by the reference parser. */
    event[size++] = 2;
    event[size++] = 0xa1;
    event[size++] = 0xb2;
    event[1] = (uint8_t)(size - P4_HOSTED_INIT_EVENT_HEADER_SIZE);

    CHECK(p4_hosted_init_parse(event, size, &result));
    CHECK(result.chip_id == P4_HOSTED_INIT_CHIP_ESP32C6);
    CHECK(result.capabilities == 0x0d);
    CHECK(result.test_raw_tp == 0);
    CHECK(result.rx_queue_size == 20);
    CHECK(result.tx_queue_size == 20);
    CHECK(result.extended_capabilities == UINT32_C(0x10));
    CHECK(result.present == (P4_HOSTED_INIT_HAS_CAPABILITIES |
                             P4_HOSTED_INIT_HAS_CHIP_ID |
                             P4_HOSTED_INIT_HAS_TEST_RAW_TP |
                             P4_HOSTED_INIT_HAS_RX_QUEUE_SIZE |
                             P4_HOSTED_INIT_HAS_TX_QUEUE_SIZE |
                             P4_HOSTED_INIT_HAS_EXTENDED_CAPABILITIES));
}

static void test_maximum_bounded_unknown_tag(void)
{
    uint8_t event[P4_HOSTED_INIT_MAX_EVENT_SIZE + 1u];
    struct p4_hosted_init result;
    size_t size = sizeof observed_init;

    memcpy(event, observed_init, size);
    event[size++] = 0x70;
    event[size++] = 56;
    memset(event + size, 0x5c, 56);
    size += 56;
    event[1] = P4_HOSTED_INIT_MAX_TLV_SIZE;

    CHECK(size == P4_HOSTED_INIT_MAX_EVENT_SIZE);
    CHECK(p4_hosted_init_parse(event, size, &result));
    CHECK(result.chip_id == P4_HOSTED_INIT_CHIP_ESP32C6);

    memcpy(event, observed_init, sizeof observed_init);
    size = sizeof observed_init;
    event[size++] = 0x70;
    event[size++] = 57;
    memset(event + size, 0x5c, 57);
    size += 57;
    event[1] = (uint8_t)(size - P4_HOSTED_INIT_EVENT_HEADER_SIZE);
    CHECK(size == P4_HOSTED_INIT_MAX_EVENT_SIZE + 1u);
    expect_refusal(event, size);
}

static void test_envelope_and_truncation(void)
{
    uint8_t copy[sizeof full_init];
    size_t i;

    for (i = 0; i < sizeof full_init; ++i)
        expect_refusal(full_init, i);

    memcpy(copy, observed_init, sizeof observed_init);
    copy[0] = 0x23;
    expect_refusal(copy, sizeof observed_init);

    memcpy(copy, observed_init, sizeof observed_init);
    copy[1]--;
    expect_refusal(copy, sizeof observed_init);

    memcpy(copy, observed_init, sizeof observed_init);
    copy[1]++;
    expect_refusal(copy, sizeof observed_init);

    expect_refusal(NULL, sizeof observed_init);
    CHECK(!p4_hosted_init_parse(observed_init, sizeof observed_init, NULL));
    expect_refusal(observed_init, P4_HOSTED_INIT_MAX_EVENT_SIZE + 1u);
}

static void test_malformed_tlvs(void)
{
    uint8_t event[P4_HOSTED_INIT_MAX_EVENT_SIZE];
    size_t size;

    /* A dangling tag, a dangling length, and an overlong value. */
    event[0] = P4_HOSTED_INIT_EVENT_TYPE;
    event[1] = 1;
    event[2] = 0x70;
    expect_refusal(event, 3);

    event[1] = 2;
    event[2] = 0x70;
    event[3] = 1;
    expect_refusal(event, 4);

    memcpy(event, observed_init, sizeof observed_init);
    size = sizeof observed_init;
    event[size++] = 0x70;
    event[size++] = 3;
    event[size++] = 0xaa;
    event[1] = (uint8_t)(size - P4_HOSTED_INIT_EVENT_HEADER_SIZE);
    expect_refusal(event, size);

    /* Known fields have fixed lengths and each known field is a singleton. */
    {
        static const uint8_t tags[] = {
            P4_HOSTED_INIT_TAG_CAPABILITIES,
            P4_HOSTED_INIT_TAG_CHIP_ID,
            P4_HOSTED_INIT_TAG_TEST_RAW_TP,
            P4_HOSTED_INIT_TAG_RX_QUEUE_SIZE,
            P4_HOSTED_INIT_TAG_TX_QUEUE_SIZE,
            P4_HOSTED_INIT_TAG_EXTENDED_CAPABILITIES,
            P4_HOSTED_INIT_TAG_BOARD_DISCOVERY
        };
        static const uint8_t lengths[] = { 1, 1, 1, 1, 1, 4, 1 };
        size_t i;

        for (i = 0; i < sizeof tags; ++i) {
            size_t offset = P4_HOSTED_INIT_EVENT_HEADER_SIZE;
            size_t tag_size = (size_t)lengths[i] + 1u;
            size_t j;

            event[0] = P4_HOSTED_INIT_EVENT_TYPE;
            if (tags[i] != P4_HOSTED_INIT_TAG_CAPABILITIES) {
                event[offset++] = P4_HOSTED_INIT_TAG_CAPABILITIES;
                event[offset++] = 1;
                event[offset++] = 0x0d;
            }
            if (tags[i] != P4_HOSTED_INIT_TAG_CHIP_ID) {
                event[offset++] = P4_HOSTED_INIT_TAG_CHIP_ID;
                event[offset++] = 1;
                event[offset++] = P4_HOSTED_INIT_CHIP_ESP32C6;
            }
            event[offset++] = tags[i];
            event[offset++] = (uint8_t)tag_size;
            for (j = 0; j < tag_size; ++j)
                event[offset++] = (uint8_t)(0x20u + j);
            event[1] = (uint8_t)(offset - P4_HOSTED_INIT_EVENT_HEADER_SIZE);
            expect_refusal(event, offset);

            /* Duplicate each known field in an otherwise valid event. */
            memcpy(event, observed_init, sizeof observed_init);
            offset = sizeof observed_init;
            if (tags[i] != P4_HOSTED_INIT_TAG_CAPABILITIES &&
                tags[i] != P4_HOSTED_INIT_TAG_CHIP_ID) {
                event[offset++] = tags[i];
                event[offset++] = lengths[i];
                for (j = 0; j < lengths[i]; ++j)
                    event[offset++] = (uint8_t)(0x20u + j);
            }
            event[offset++] = tags[i];
            event[offset++] = lengths[i];
            for (j = 0; j < lengths[i]; ++j)
                event[offset++] = (uint8_t)(0x30u + j);
            event[1] = (uint8_t)(offset - P4_HOSTED_INIT_EVENT_HEADER_SIZE);
            expect_refusal(event, offset);
        }
    }
}

static void test_required_fields_and_values(void)
{
    uint8_t event[sizeof observed_init];

    memcpy(event, observed_init, sizeof event);
    event[1] = 3;
    expect_refusal(event, 5); /* Capabilities without chip ID. */

    event[1] = 3;
    event[2] = P4_HOSTED_INIT_TAG_CHIP_ID;
    event[4] = P4_HOSTED_INIT_CHIP_ESP32C6;
    expect_refusal(event, 5); /* Chip ID without capabilities. */

    memcpy(event, observed_init, sizeof event);
    event[7] = 0x03;
    expect_refusal(event, sizeof event); /* Unknown chip. */

    memcpy(event, observed_init, sizeof event);
    event[4] = 0x0c;
    expect_refusal(event, sizeof event); /* WLAN-over-SDIO absent. */
}

static void test_random_inputs(void)
{
    uint8_t bytes[P4_HOSTED_INIT_MAX_EVENT_SIZE + 16u];
    unsigned iteration;

    for (iteration = 0; iteration < 50000u; ++iteration) {
        struct p4_hosted_init result;
        uint8_t before[sizeof result];
        size_t size = next_random() % (sizeof bytes + 1u);
        size_t i;
        int accepted;

        for (i = 0; i < sizeof bytes; ++i)
            bytes[i] = (uint8_t)next_random();
        memset(&result, 0x6b, sizeof result);
        memcpy(before, &result, sizeof result);
        accepted = p4_hosted_init_parse(bytes, size, &result);
        if (accepted) {
            CHECK(size >= P4_HOSTED_INIT_EVENT_HEADER_SIZE);
            CHECK(size <= P4_HOSTED_INIT_MAX_EVENT_SIZE);
            CHECK(bytes[0] == P4_HOSTED_INIT_EVENT_TYPE);
            CHECK(bytes[1] == size - P4_HOSTED_INIT_EVENT_HEADER_SIZE);
            CHECK(result.present & P4_HOSTED_INIT_HAS_CAPABILITIES);
            CHECK(result.present & P4_HOSTED_INIT_HAS_CHIP_ID);
            CHECK(result.capabilities & P4_HOSTED_INIT_CAP_WLAN_SDIO);
        } else {
            CHECK(memcmp(&result, before, sizeof result) == 0);
        }
        ++checks;
    }
}

int main(void)
{
    test_observed_c6_event();
    test_board_discovery_capability();
    test_all_known_fields_and_unknown_tag();
    test_maximum_bounded_unknown_tag();
    test_envelope_and_truncation();
    test_malformed_tlvs();
    test_required_fields_and_values();
    test_random_inputs();
    printf("hosted INIT parser: %lu checks passed\n", checks);
    return 0;
}
