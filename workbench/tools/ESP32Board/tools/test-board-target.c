/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Host tests for the actual ESP32Board target comparison source. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "target.h"

static unsigned long checks;
static unsigned long failures;
static unsigned long cases;
static const char *case_name = "startup";

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "FAIL [%s] line %d: %s\n", case_name, __LINE__, \
            #expression); \
    } \
} while (0)

static void begin_case(const char *name)
{
    case_name = name;
    ++cases;
}

static struct board_image_info image_fixture(void)
{
    struct board_image_info image;
    memset(&image, 0, sizeof(image));
    image.bytes = 0x80000u;
    image.secure_version = 4u;
    image.min_revision = 2u;
    image.max_revision = 6u;
    return image;
}

static struct board_discovery_snapshot target_fixture(void)
{
    struct board_discovery_snapshot target;
    memset(&target, 0, sizeof(target));
    target.physical_bytes = 16u * 1024u * 1024u;
    target.layout.flash_bytes = target.physical_bytes;
    target.layout.count = 2u;
    target.layout.partitions[0].offset = 0x10000u;
    target.layout.partitions[0].size = 0x100000u;
    target.layout.partitions[0].type = 0u;
    target.layout.partitions[0].subtype = 0x10u;
    memcpy(target.layout.partitions[0].label, "ota_0", 6u);
    target.layout.partitions[1].offset = 0x110000u;
    target.layout.partitions[1].size = 0x100000u;
    target.layout.partitions[1].type = 0u;
    target.layout.partitions[1].subtype = 0x11u;
    memcpy(target.layout.partitions[1].label, "ota_1", 6u);
    target.running = 0u;
    target.boot = 0u;
    target.update = 1u;
    target.running_state = 3u;
    target.security_known = BOARD_DISCOVERY_SECURITY_ALL;
    target.target_known = BOARD_DISCOVERY_TARGET_ALL;
    target.chip_id = BOARD_DISCOVERY_CHIP_C6;
    target.chip_revision = 3u;
    target.secure_version = 2u;
    return target;
}

static unsigned int proof_unknown(void)
{
    return BOARD_C6_TARGET_POLICY | BOARD_C6_TARGET_PROVENANCE;
}

/* A return value of 1 means only that a comparison report was produced. */
static void expect_report(const char *name,
    const struct board_image_info *image,
    const struct board_discovery_snapshot *target,
    unsigned int rejected, unsigned int unknown, unsigned int risk)
{
    struct board_c6_target_report report = {0};
    int result;
    begin_case(name);
    result = board_c6_target_compare(image, target, &report);
    CHECK(result == 1);
    if (result == 1) {
        CHECK(report.rejected == rejected);
        CHECK(report.unknown == unknown);
        CHECK(report.risk == risk);
        CHECK((report.unknown & proof_unknown()) == proof_unknown());
    }
}

static void expect_invalid_report_unchanged(const char *name,
    const struct board_image_info *image,
    const struct board_discovery_snapshot *target)
{
    struct board_c6_target_report report, before;
    int result;
    memset(&report, 0xa5, sizeof(report));
    before = report;
    begin_case(name);
    result = board_c6_target_compare(image, target, &report);
    CHECK(result == 0);
    CHECK(memcmp(&report, &before, sizeof(report)) == 0);
}

static void test_valid_and_known_zero(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    expect_report("valid-facts", &image, &target, 0u, proof_unknown(), 0u);

    image.min_revision = 0u;
    image.max_revision = 0u;
    target.chip_revision = 0u;
    target.secure_version = 0u;
    expect_report("known-zero-revision-and-counter", &image, &target,
        0u, proof_unknown(), 0u);
}

static void test_missing_known_bits(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();
    unsigned int unknown = proof_unknown() | BOARD_C6_TARGET_CHIP |
        BOARD_C6_TARGET_REVISION | BOARD_C6_TARGET_COUNTER |
        BOARD_C6_TARGET_SECURITY;

    target.target_known = 0u;
    target.chip_id = 0u;
    target.chip_revision = 0u;
    target.secure_version = 0u;
    target.security_known = BOARD_DISCOVERY_SECURITY_ENCRYPTION;
    expect_report("missing-known-target-and-security-bits", &image, &target,
        0u, unknown, 0u);
}

static void test_each_target_known_bit(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    target.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_CHIP;
    target.chip_id = 0u;
    expect_report("chip-known-bit-clear", &image, &target, 0u,
        proof_unknown() | BOARD_C6_TARGET_CHIP, 0u);

    target = target_fixture();
    target.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_REVISION;
    target.chip_revision = 0u;
    expect_report("revision-known-bit-clear", &image, &target, 0u,
        proof_unknown() | BOARD_C6_TARGET_REVISION, 0u);

    target = target_fixture();
    target.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_SECURE_VERSION;
    target.secure_version = 0u;
    expect_report("counter-known-bit-clear", &image, &target, 0u,
        proof_unknown() | BOARD_C6_TARGET_COUNTER, 0u);
}

static void test_revision_bounds_and_sentinels(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    target.chip_revision = 1u;
    expect_report("revision-below-image-minimum", &image, &target,
        BOARD_C6_TARGET_REVISION, proof_unknown(), 0u);

    target.chip_revision = 7u;
    expect_report("revision-above-image-maximum", &image, &target,
        BOARD_C6_TARGET_REVISION, proof_unknown(), 0u);

    target.chip_revision = image.min_revision;
    expect_report("revision-at-image-minimum", &image, &target,
        0u, proof_unknown(), 0u);

    target.chip_revision = image.max_revision;
    expect_report("revision-at-image-maximum", &image, &target,
        0u, proof_unknown(), 0u);

    target.chip_revision = UINT16_MAX;
    image.max_revision = 0u;
    expect_report("zero-image-maximum-is-unbounded", &image, &target,
        0u, proof_unknown(), 0u);

    image.max_revision = UINT16_MAX;
    expect_report("ffff-image-maximum-is-unbounded", &image, &target,
        0u, proof_unknown(), 0u);

    image.min_efuse_revision = 1u;
    expect_report("finite-efuse-revision-is-unknown", &image, &target,
        0u, proof_unknown() | BOARD_C6_TARGET_BLOCK_REV, 0u);

    image.min_efuse_revision = UINT16_MAX;
    image.max_efuse_revision = 0u;
    expect_report("efuse-revision-sentinels-add-no-constraint", &image,
        &target, 0u, proof_unknown(), 0u);

    image.min_efuse_revision = 0u;
    image.max_efuse_revision = UINT16_MAX;
    expect_report("ffff-maximum-efuse-revision-is-unbounded", &image,
        &target, 0u, proof_unknown(), 0u);
}

static void test_counter_behavior(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    target.secure_version = 5u;
    expect_report("lower-image-counter-is-risk-not-rejection", &image,
        &target, 0u, proof_unknown(), BOARD_C6_TARGET_COUNTER);

    target.secure_version = 16u;
    expect_report("maximum-physical-counter-is-valid-risk", &image,
        &target, 0u, proof_unknown(), BOARD_C6_TARGET_COUNTER);
}

static void test_security_and_slots(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    target.security_enabled = BOARD_DISCOVERY_SECURITY_SECURE_BOOT;
    expect_report("enabled-secure-boot-is-refused", &image, &target,
        BOARD_C6_TARGET_SECURITY, proof_unknown(), 0u);

    target = target_fixture();
    target.security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION;
    expect_report("encryption-only-is-refused", &image, &target,
        BOARD_C6_TARGET_SECURITY, proof_unknown(), 0u);

    target = target_fixture();
    target.update = target.running;
    expect_report("update-slot-equals-running-slot-is-refused", &image,
        &target, BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target = target_fixture();
    target.boot = 1u;
    target.update = 1u;
    expect_report("update-slot-equals-distinct-boot-slot-is-refused", &image,
        &target, BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target = target_fixture();
    target.layout.partitions[target.update].flags =
        BOARD_DISCOVERY_PART_ENCRYPTED | BOARD_DISCOVERY_PART_READONLY;
    expect_report("flagged-update-slot-is-refused", &image, &target,
        BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    image.bytes = 0x100001u;
    target = target_fixture();
    expect_report("image-larger-than-update-slot-is-refused", &image,
        &target, BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    image = image_fixture();
    target = target_fixture();
    target.running_state = 1u;
    expect_report("new-running-state-is-refused", &image, &target,
        BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target.running_state = 2u;
    expect_report("pending-running-state-is-refused", &image, &target,
        BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target.running_state = 4u;
    expect_report("invalid-running-state-is-refused", &image, &target,
        BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target.running_state = 5u;
    expect_report("aborted-running-state-is-refused", &image, &target,
        BOARD_C6_TARGET_SLOT, proof_unknown(), 0u);

    target.running_state = 6u;
    expect_report("undefined-running-state-currently-has-no-slot-flag",
        &image, &target, 0u, proof_unknown(), 0u);
}

static void test_missing_roles_and_state(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();
    unsigned int unknown = proof_unknown() | BOARD_C6_TARGET_SLOT;

    target.update = BOARD_DISCOVERY_UNKNOWN_ROLE;
    expect_report("unknown-update-role", &image, &target, 0u, unknown, 0u);

    target = target_fixture();
    target.running = BOARD_DISCOVERY_UNKNOWN_ROLE;
    target.running_state = 0u;
    expect_report("unknown-running-role", &image, &target, 0u, unknown, 0u);

    target = target_fixture();
    target.boot = BOARD_DISCOVERY_UNKNOWN_ROLE;
    expect_report("unknown-boot-role", &image, &target, 0u, unknown, 0u);

    target = target_fixture();
    target.running_state = 0u;
    expect_report("unknown-running-state", &image, &target, 0u, unknown, 0u);
}

static void test_unknown_block_and_mmu(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();

    image.max_efuse_revision = 3u;
    expect_report("block-revision-constraint-unknown", &image, &target,
        0u, proof_unknown() | BOARD_C6_TARGET_BLOCK_REV, 0u);

    image = image_fixture();
    image.mmu_page_size = 1u;
    expect_report("mmu-page-constraint-unknown", &image, &target,
        0u, proof_unknown() | BOARD_C6_TARGET_MMU, 0u);
}

static void test_invalid_inputs_and_aliases(void)
{
    struct board_image_info image = image_fixture();
    struct board_discovery_snapshot target = target_fixture();
    int result;

    expect_invalid_report_unchanged("null-image", NULL, &target);
    expect_invalid_report_unchanged("null-target", &image, NULL);

    begin_case("null-report");
    CHECK(board_c6_target_compare(&image, &target, NULL) == 0);

    image.bytes = 287u;
    expect_invalid_report_unchanged("image-too-short", &image, &target);

    image = image_fixture();
    image.bytes = (size_t)BOARD_C6_IMAGE_MAX + 1u;
    expect_invalid_report_unchanged("image-too-large", &image, &target);

    image = image_fixture();
    target = target_fixture();
    target.layout.flash_bytes = 0u;
    expect_invalid_report_unchanged("invalid-discovery-snapshot", &image,
        &target);

    target = target_fixture();
    target.secure_version = 17u;
    expect_invalid_report_unchanged("impossible-full-physical-counter",
        &image, &target);

    target = target_fixture();
    {
        union image_report {
            struct board_image_info image;
            struct board_c6_target_report report;
        } overlapping;
        unsigned char before[sizeof(overlapping)];
        overlapping.image = image_fixture();
        memcpy(before, &overlapping, sizeof(overlapping));
        begin_case("report-aliases-image");
        result = board_c6_target_compare(&overlapping.image, &target,
            &overlapping.report);
        CHECK(result == 0);
        CHECK(memcmp(before, &overlapping, sizeof(overlapping)) == 0);
    }

    {
        union target_report {
            struct board_discovery_snapshot target;
            struct board_c6_target_report report;
        } overlapping;
        unsigned char before[sizeof(overlapping)];
        overlapping.target = target_fixture();
        memcpy(before, &overlapping, sizeof(overlapping));
        begin_case("report-aliases-discovery-snapshot");
        result = board_c6_target_compare(&image, &overlapping.target,
            &overlapping.report);
        CHECK(result == 0);
        CHECK(memcmp(before, &overlapping, sizeof(overlapping)) == 0);
    }

}

int main(void)
{
    test_valid_and_known_zero();
    test_missing_known_bits();
    test_each_target_known_bit();
    test_revision_bounds_and_sentinels();
    test_counter_behavior();
    test_security_and_slots();
    test_missing_roles_and_state();
    test_unknown_block_and_mmu();
    test_invalid_inputs_and_aliases();

    if (failures) {
        fprintf(stderr, "RESULT cases=%lu checks=%lu failures=%lu\n",
            cases, checks, failures);
        return 1;
    }
    printf("RESULT cases=%lu checks=%lu failures=0\n", cases, checks);
    return 0;
}
