/*
 * Host tests for the production C6 discovery wire encoder, decoder and
 * structural validator. These tests exercise byte buffers only; they do not
 * call ESP-IDF APIs or access target hardware.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovery_wire.h"

_Static_assert(BOARD_DISCOVERY_WIRE_MAX == 1064u,
    "wire maximum must be one header plus 32 records");

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SENTINEL 0xa5u

static const char *active_case;
static unsigned checks_run;
static unsigned cases_run;
static unsigned failures;

static int fail(const char *message)
{
    fprintf(stderr, "FAIL %s: %s\n", active_case ? active_case : "setup", message);
    ++failures;
    return 0;
}

#define CHECK(condition, message) do { \
    ++checks_run; \
    if (!(condition)) return fail(message); \
} while (0)

static void begin_case(const char *name)
{
    active_case = name;
    ++cases_run;
}

static void put32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void make_snapshot(struct board_discovery_snapshot *snapshot,
    size_t count)
{
    size_t i;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->physical_bytes = 0x01000000u;
    snapshot->layout.flash_bytes = 0x00800000u;
    snapshot->layout.count = count;
    snapshot->running = 0;
    snapshot->boot = count > 1 ? 1 : 0;
    /* A detected running==update role is valid inventory, not update policy. */
    snapshot->update = 0;
    snapshot->running_state = 3;
    snapshot->security_known = BOARD_DISCOVERY_SECURITY_ALL;
    snapshot->security_enabled = BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK;
    snapshot->target_known = BOARD_DISCOVERY_TARGET_ALL;
    snapshot->chip_id = BOARD_DISCOVERY_CHIP_C6;
    snapshot->chip_revision = 0x1234u;
    snapshot->secure_version = 0x89abcdefu;

    for (i = 0; i < count; ++i) {
        struct board_partition *part = &snapshot->layout.partitions[i];
        uint32_t offset;

        if (i == 0) offset = 0x00010000u;
        else if (i == 1) offset = 0x00020000u;
        else if (i == 2) offset = 0x00030000u;
        else offset = 0x00031000u + (uint32_t)(i - 3u) * 0x1000u;

        part->offset = offset;
        part->size = 0x1000u;
        part->flags = (uint32_t)(i & 3u);
        if (i < 3) {
            part->type = 0;
            part->subtype = (uint8_t)(0x10u + i);
        } else {
            part->type = 1;
            part->subtype = 2;
        }
        (void)snprintf(part->label, sizeof(part->label), "part%02u", (unsigned)i);
    }
}

static int same_snapshot(const struct board_discovery_snapshot *a,
    const struct board_discovery_snapshot *b)
{
    size_t i;

    if (a->physical_bytes != b->physical_bytes ||
        a->layout.flash_bytes != b->layout.flash_bytes ||
        a->layout.count != b->layout.count ||
        a->layout.md5_record_present != b->layout.md5_record_present ||
        a->running != b->running || a->boot != b->boot ||
        a->update != b->update || a->running_state != b->running_state ||
        a->security_known != b->security_known ||
        a->security_enabled != b->security_enabled ||
        a->target_known != b->target_known ||
        a->chip_id != b->chip_id ||
        a->chip_revision != b->chip_revision ||
        a->secure_version != b->secure_version)
        return 0;

    for (i = 0; i < a->layout.count; ++i) {
        const struct board_partition *left = &a->layout.partitions[i];
        const struct board_partition *right = &b->layout.partitions[i];
        if (left->offset != right->offset || left->size != right->size ||
            left->flags != right->flags || left->type != right->type ||
            left->subtype != right->subtype ||
            memcmp(left->label, right->label, sizeof(left->label)) != 0)
            return 0;
    }
    return 1;
}

static int roundtrip(size_t count, uint32_t nonce, const char *name)
{
    struct board_discovery_snapshot source, decoded;
    uint8_t bytes[BOARD_DISCOVERY_WIRE_MAX];
    size_t written = SIZE_MAX;
    size_t expected = BOARD_DISCOVERY_WIRE_HEADER +
        count * BOARD_DISCOVERY_WIRE_RECORD;

    begin_case(name);
    make_snapshot(&source, count);
    memset(bytes, SENTINEL, sizeof(bytes));
    CHECK(board_discovery_validate(&source), "test fixture is not a valid inventory");
    CHECK(source.running == source.update,
        "fixture must cover running==update valid inventory");
    CHECK(board_discovery_encode(&source, nonce, bytes, sizeof(bytes), &written),
        "valid inventory did not encode");
    CHECK(written == expected, "encoder returned a non-canonical length");
    CHECK(written == sizeof(bytes) || bytes[written] == SENTINEL,
        "encoder modified bytes beyond the canonical payload length");
    memset(&decoded, 0x5a, sizeof(decoded));
    CHECK(board_discovery_decode(bytes, written, nonce, &decoded),
        "valid encoded inventory did not decode");
    CHECK(same_snapshot(&source, &decoded), "round-trip changed inventory fields");
    return 1;
}

static int test_roundtrip_one(void)
{
    return roundtrip(1u, 0x78563412u, "roundtrip-one-partition");
}

static int test_roundtrip_maximum(void)
{
    int result = roundtrip(BOARD_LAYOUT_MAX_PARTITIONS, UINT32_MAX,
        "roundtrip-32-partitions-max-nonce");
    if (!result) return 0;
    begin_case("maximum-canonical-size");
    CHECK(BOARD_DISCOVERY_WIRE_MAX == 1064u,
        "maximum wire size is not 1064 bytes");
    return 1;
}

static int test_byte_layout_and_label_boundaries(void)
{
    struct board_discovery_snapshot source, decoded;
    uint8_t bytes[BOARD_DISCOVERY_WIRE_MAX];
    const uint8_t first_record = BOARD_DISCOVERY_WIRE_HEADER;
    size_t written = 0, i;

    begin_case("little-endian-byte-layout-and-label-boundaries");
    make_snapshot(&source, 4);
    source.layout.partitions[0].flags = BOARD_DISCOVERY_PART_ENCRYPTED |
        BOARD_DISCOVERY_PART_READONLY;
    memcpy(source.layout.partitions[0].label, "abcdefghijklmnop", 17);
    memcpy(source.layout.partitions[1].label, "x", 2);
    CHECK(board_discovery_validate(&source), "16-byte label fixture is invalid");
    memset(bytes, SENTINEL, sizeof(bytes));
    CHECK(board_discovery_encode(&source, 0x78563412u, bytes, sizeof(bytes), &written),
        "label boundary fixture did not encode");

    CHECK(memcmp(bytes, "C6DI", 4) == 0, "magic bytes are not canonical");
    CHECK(bytes[4] == 2 && bytes[5] == 40 && bytes[6] == 32 && bytes[7] == 4,
        "version/header/record/count bytes are misplaced");
    CHECK(bytes[8] == source.running && bytes[9] == source.boot &&
        bytes[10] == source.update && bytes[11] == source.running_state &&
        bytes[12] == source.security_known && bytes[13] == source.security_enabled,
        "role or policy byte positions are wrong");
    CHECK(bytes[14] == BOARD_DISCOVERY_TARGET_ALL && bytes[15] == 0,
        "target-known or reserved byte is misplaced");

    CHECK(bytes[16] == 0x00 && bytes[17] == 0x00 &&
        bytes[18] == 0x00 && bytes[19] == 0x01,
        "physical capacity is not little-endian at offset 16");
    CHECK(bytes[20] == 0x00 && bytes[21] == 0x00 &&
        bytes[22] == 0x80 && bytes[23] == 0x00,
        "usable capacity is not little-endian at offset 20");
    CHECK(bytes[24] == 0x12 && bytes[25] == 0x34 &&
        bytes[26] == 0x56 && bytes[27] == 0x78,
        "nonce is not little-endian at offset 24");
    CHECK(bytes[28] == 0x0d && bytes[29] == 0x00 &&
        bytes[30] == 0x34 && bytes[31] == 0x12,
        "chip ID or revision is not little-endian at offsets 28 and 30");
    CHECK(bytes[32] == 0xef && bytes[33] == 0xcd &&
        bytes[34] == 0xab && bytes[35] == 0x89,
        "secure version is not little-endian across all four bytes");
    CHECK(bytes[36] == 0 && bytes[37] == 0 &&
        bytes[38] == 0 && bytes[39] == 0,
        "reserved header bytes 36 through 39 are not zero");

    CHECK(bytes[first_record] == 0x00 && bytes[first_record + 1] == 0x00 &&
        bytes[first_record + 2] == 0x01 && bytes[first_record + 3] == 0x00,
        "partition offset is not little-endian at record offset 0");
    CHECK(bytes[first_record + 4] == 0x00 && bytes[first_record + 5] == 0x10 &&
        bytes[first_record + 6] == 0x00 && bytes[first_record + 7] == 0x00,
        "partition size is not little-endian at record offset 4");
    CHECK(bytes[first_record + 8] == 0x03 && bytes[first_record + 9] == 0x00 &&
        bytes[first_record + 10] == 0x00 && bytes[first_record + 11] == 0x00,
        "partition flags are not little-endian at record offset 8");
    CHECK(bytes[first_record + 12] == source.layout.partitions[0].type &&
        bytes[first_record + 13] == source.layout.partitions[0].subtype,
        "partition type/subtype byte positions are wrong");
    CHECK(bytes[first_record + 14] == 16 && bytes[first_record + 15] == 0,
        "16-byte label length or record reserved byte is wrong");
    CHECK(memcmp(bytes + first_record + 16, "abcdefghijklmnop", 16) == 0,
        "16-byte label payload changed");
    CHECK(bytes[first_record + BOARD_DISCOVERY_WIRE_RECORD + 14] == 1,
        "one-byte label length is wrong");
    for (i = 1; i < 16; ++i)
        CHECK(bytes[first_record + BOARD_DISCOVERY_WIRE_RECORD + 16 + i] == 0,
            "short label padding is not zero");

    memset(&decoded, 0x5a, sizeof(decoded));
    CHECK(board_discovery_decode(bytes, written, 0x78563412u, &decoded),
        "labels at supported boundaries did not decode");
    CHECK(memcmp(decoded.layout.partitions[0].label, "abcdefghijklmnop", 16) == 0 &&
        decoded.layout.partitions[0].label[16] == '\0',
        "16-byte label was not terminated in the native output");
    CHECK(strcmp(decoded.layout.partitions[1].label, "x") == 0,
        "one-byte label did not round-trip");
    return 1;
}

static int expect_decode_reject(const char *name, const uint8_t *bytes,
    size_t size, uint32_t nonce)
{
    struct board_discovery_snapshot output;
    uint8_t before[sizeof(output)];
    int result;

    begin_case(name);
    memset(&output, SENTINEL, sizeof(output));
    memcpy(before, &output, sizeof(before));
    result = board_discovery_decode(bytes, size, nonce, &output);
    CHECK(!result, "malformed input was accepted");
    CHECK(memcmp(before, &output, sizeof(output)) == 0,
        "decode refusal changed output bytes");
    return 1;
}

static int test_decode_rejections(void)
{
    struct board_discovery_snapshot source;
    uint8_t base[BOARD_DISCOVERY_WIRE_MAX];
    uint8_t mutated[BOARD_DISCOVERY_WIRE_MAX];
    size_t written = 0;

    make_snapshot(&source, 4);
    memset(base, 0, sizeof(base));
    if (!board_discovery_encode(&source, 0x2468ace0u, base, sizeof(base), &written))
        return fail("could not prepare decoder rejection fixture");

#define REJECT_BYTE(case_name, index, value) do { \
    memcpy(mutated, base, written); \
    mutated[(index)] = (uint8_t)(value); \
    if (!expect_decode_reject((case_name), mutated, written, 0x2468ace0u)) return 0; \
} while (0)

    REJECT_BYTE("bad-magic", 0, 'X');
    REJECT_BYTE("legacy-version-1", 4, 1);
    REJECT_BYTE("unsupported-version", 4, 3);
    REJECT_BYTE("bad-header-size", 5, 39);
    REJECT_BYTE("bad-record-size", 6, 31);
    REJECT_BYTE("zero-record-count", 7, 0);
    REJECT_BYTE("record-count-over-maximum", 7, 33);
    REJECT_BYTE("unknown-target-bit", 14, 8);
    REJECT_BYTE("reserved-header-byte-15", 15, 1);
    REJECT_BYTE("reserved-header-byte-36", 36, 1);
    REJECT_BYTE("reserved-header-byte-39", 39, 1);
    REJECT_BYTE("zero-wire-nonce", 24, 0);
    REJECT_BYTE("unknown-policy-bit", 12, 0x10);
    REJECT_BYTE("enabled-policy-not-known", 13, 0x80);
    REJECT_BYTE("wrong-chip-id", 28, BOARD_DISCOVERY_CHIP_C6 - 1u);
    REJECT_BYTE("running-role-out-of-range", 8, 4);
    REJECT_BYTE("boot-role-points-to-data", 9, 3);
    REJECT_BYTE("update-role-points-to-data", 10, 3);
    REJECT_BYTE("invalid-running-state", 11, 7);
    REJECT_BYTE("unknown-running-role-with-known-state", 8,
        BOARD_DISCOVERY_UNKNOWN_ROLE);
    REJECT_BYTE("record-reserved-byte", BOARD_DISCOVERY_WIRE_HEADER + 15, 1);
    REJECT_BYTE("empty-label", BOARD_DISCOVERY_WIRE_HEADER + 14, 0);
    REJECT_BYTE("overlong-label", BOARD_DISCOVERY_WIRE_HEADER + 14, 17);
    REJECT_BYTE("embedded-nul-in-label", BOARD_DISCOVERY_WIRE_HEADER + 16, 0);
    REJECT_BYTE("nonzero-label-padding", BOARD_DISCOVERY_WIRE_HEADER + 22, 1);
    REJECT_BYTE("unknown-partition-flag", BOARD_DISCOVERY_WIRE_HEADER + 8, 4);

#undef REJECT_BYTE

    memcpy(mutated, base, written);
    mutated[4] = 1u;
    mutated[5] = 32u;
    if (!expect_decode_reject("truncated-version-1-payload", mutated,
            32u + 4u * BOARD_DISCOVERY_WIRE_RECORD, 0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    mutated[14] = (uint8_t)(BOARD_DISCOVERY_TARGET_ALL &
        (uint8_t)~BOARD_DISCOVERY_TARGET_CHIP);
    if (!expect_decode_reject("unknown-chip-value-nonzero", mutated, written,
            0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    mutated[14] = (uint8_t)(BOARD_DISCOVERY_TARGET_ALL &
        (uint8_t)~BOARD_DISCOVERY_TARGET_REVISION);
    if (!expect_decode_reject("unknown-revision-value-nonzero", mutated, written,
            0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    mutated[14] = (uint8_t)(BOARD_DISCOVERY_TARGET_ALL &
        (uint8_t)~BOARD_DISCOVERY_TARGET_SECURE_VERSION);
    if (!expect_decode_reject("unknown-secure-version-nonzero", mutated, written,
            0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    put32(mutated + BOARD_DISCOVERY_WIRE_HEADER + BOARD_DISCOVERY_WIRE_RECORD,
        source.layout.partitions[0].offset);
    if (!expect_decode_reject("overlapping-partitions", mutated, written,
            0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    put32(mutated + BOARD_DISCOVERY_WIRE_HEADER + 4, 0x00800000u);
    if (!expect_decode_reject("partition-outside-layout", mutated, written,
            0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    put32(mutated + 16, 0x01000001u);
    if (!expect_decode_reject("non-power-of-two-physical-capacity", mutated,
            written, 0x2468ace0u)) return 0;

    memcpy(mutated, base, written);
    put32(mutated + 16, 0x00400000u);
    if (!expect_decode_reject("usable-capacity-exceeds-physical", mutated,
            written, 0x2468ace0u)) return 0;

    if (!expect_decode_reject("truncated-payload", base, written - 1u,
            0x2468ace0u)) return 0;
    memcpy(mutated, base, written);
    mutated[written] = 0;
    if (!expect_decode_reject("extra-trailing-byte", mutated, written + 1u,
            0x2468ace0u)) return 0;
    if (!expect_decode_reject("mismatched-expected-nonce", base, written,
            0x2468ace1u)) return 0;
    if (!expect_decode_reject("zero-expected-nonce", base, written, 0)) return 0;
    if (!expect_decode_reject("shorter-than-header", base,
            BOARD_DISCOVERY_WIRE_HEADER - 1u, 0x2468ace0u)) return 0;
    if (!expect_decode_reject("larger-than-maximum", base,
            BOARD_DISCOVERY_WIRE_MAX + 1u, 0x2468ace0u)) return 0;
    return 1;
}

static int target_metadata_roundtrip(const char *name,
    uint8_t target_known, uint16_t chip_id, uint16_t chip_revision,
    uint32_t secure_version)
{
    struct board_discovery_snapshot source, decoded;
    uint8_t bytes[BOARD_DISCOVERY_WIRE_MAX];
    size_t written = 0;

    begin_case(name);
    make_snapshot(&source, 1u);
    source.target_known = target_known;
    source.chip_id = chip_id;
    source.chip_revision = chip_revision;
    source.secure_version = secure_version;
    CHECK(board_discovery_validate(&source),
        "target metadata fixture is invalid");
    CHECK(board_discovery_encode(&source, 0x10203040u, bytes, sizeof(bytes),
            &written), "target metadata fixture did not encode");
    CHECK(bytes[14] == target_known, "target-known mask changed on wire");
    CHECK(bytes[28] == (uint8_t)chip_id &&
        bytes[29] == (uint8_t)(chip_id >> 8), "chip ID bytes changed on wire");
    CHECK(bytes[30] == (uint8_t)chip_revision &&
        bytes[31] == (uint8_t)(chip_revision >> 8),
        "chip revision bytes changed on wire");
    CHECK(bytes[32] == (uint8_t)secure_version &&
        bytes[33] == (uint8_t)(secure_version >> 8) &&
        bytes[34] == (uint8_t)(secure_version >> 16) &&
        bytes[35] == (uint8_t)(secure_version >> 24),
        "secure-version bytes changed on wire");
    CHECK(board_discovery_decode(bytes, written, 0x10203040u, &decoded),
        "target metadata fixture did not decode");
    CHECK(same_snapshot(&source, &decoded),
        "target metadata changed across wire roundtrip");
    return 1;
}

static int test_target_metadata_unknown_and_partial(void)
{
    if (!target_metadata_roundtrip("all-target-fields-unknown-zero", 0u,
            0u, 0u, 0u)) return 0;
    return target_metadata_roundtrip("partial-target-known-fields-zero-unknown",
        BOARD_DISCOVERY_TARGET_CHIP, BOARD_DISCOVERY_CHIP_C6, 0u, 0u);
}

static int expect_encode_reject(const char *name,
    const struct board_discovery_snapshot *snapshot, uint32_t nonce,
    uint8_t *bytes, size_t capacity, size_t extent, size_t *written)
{
    uint8_t before[BOARD_DISCOVERY_WIRE_MAX];
    size_t written_before = *written;
    int result;

    begin_case(name);
    memcpy(before, bytes, extent);
    result = board_discovery_encode(snapshot, nonce, bytes, capacity, written);
    CHECK(!result, "invalid encode request was accepted");
    CHECK(memcmp(before, bytes, extent) == 0,
        "encode refusal changed output bytes");
    CHECK(*written == written_before, "encode refusal changed written");
    return 1;
}

static int test_encode_refusals(void)
{
    struct board_discovery_snapshot source, invalid;
    uint8_t bytes[BOARD_DISCOVERY_WIRE_MAX];
    size_t written;

    make_snapshot(&source, 4);
    memset(bytes, SENTINEL, sizeof(bytes));
    written = 0x12345678u;
    invalid = source;
    invalid.layout.partitions[0].flags = 4;
    if (!expect_encode_reject("invalid-inventory", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    invalid = source;
    invalid.target_known = 8u;
    written = 0x23456789u;
    if (!expect_encode_reject("unknown-target-mask", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    invalid = source;
    invalid.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_CHIP;
    written = 0x3456789au;
    if (!expect_encode_reject("unknown-chip-value", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    invalid = source;
    invalid.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_REVISION;
    written = 0x456789abu;
    if (!expect_encode_reject("unknown-revision-value", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    invalid = source;
    invalid.target_known &= (uint8_t)~BOARD_DISCOVERY_TARGET_SECURE_VERSION;
    written = 0x56789abcu;
    if (!expect_encode_reject("unknown-secure-version-value", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    invalid = source;
    invalid.chip_id = BOARD_DISCOVERY_CHIP_C6 - 1u;
    written = 0x6789abcdu;
    if (!expect_encode_reject("wrong-chip-id", &invalid, 0x1234u,
            bytes, sizeof(bytes), sizeof(bytes), &written)) return 0;

    written = 0x87654321u;
    if (!expect_encode_reject("zero-encode-nonce", &source, 0, bytes,
            sizeof(bytes), sizeof(bytes), &written)) return 0;

    written = 0xabcdef01u;
    if (!expect_encode_reject("insufficient-output-capacity", &source, 0x1234u,
            bytes, BOARD_DISCOVERY_WIRE_HEADER + 4u * BOARD_DISCOVERY_WIRE_RECORD - 1u,
            sizeof(bytes), &written)) return 0;
    return 1;
}

static int test_encode_alias_refusals(void)
{
    struct board_discovery_snapshot source, snapshot_before;
    union {
        max_align_t alignment;
        uint8_t bytes[BOARD_DISCOVERY_WIRE_MAX + sizeof(size_t)];
    } storage;
    union {
        max_align_t alignment;
        uint8_t bytes[sizeof(struct board_discovery_snapshot)];
    } snapshot_storage;
    uint8_t output_before[BOARD_DISCOVERY_WIRE_MAX + sizeof(size_t)];
    size_t written, count_before;

    make_snapshot(&source, 4);
    memset(snapshot_storage.bytes, SENTINEL, sizeof(snapshot_storage.bytes));
    memcpy(snapshot_storage.bytes, &source, sizeof(source));
    memcpy(&snapshot_before, snapshot_storage.bytes, sizeof(snapshot_before));
    written = 0x11223344u;
    begin_case("encode-snapshot-output-alias");
    CHECK(!board_discovery_encode((const struct board_discovery_snapshot *)(const void *)snapshot_storage.bytes,
            0x1234u, snapshot_storage.bytes, sizeof(snapshot_storage.bytes), &written),
        "encode accepted overlapping snapshot/output ranges");
    CHECK(memcmp(snapshot_storage.bytes, &snapshot_before, sizeof(snapshot_before)) == 0,
        "snapshot/output alias refusal modified aliased bytes");
    CHECK(written == 0x11223344u, "snapshot/output alias refusal changed written");

    make_snapshot(&source, 4);
    count_before = source.layout.count;
    memset(storage.bytes, SENTINEL, sizeof(storage.bytes));
    begin_case("encode-snapshot-written-alias");
    CHECK(!board_discovery_encode(&source, 0x1234u, storage.bytes,
            BOARD_DISCOVERY_WIRE_MAX, &source.layout.count),
        "encode accepted overlapping snapshot/written ranges");
    CHECK(source.layout.count == count_before,
        "snapshot/written alias refusal modified the snapshot");
    CHECK(storage.bytes[0] == SENTINEL,
        "snapshot/written alias refusal modified output bytes");

    memset(storage.bytes, SENTINEL, sizeof(storage.bytes));
    memcpy(output_before, storage.bytes, sizeof(output_before));
    begin_case("encode-output-written-alias");
    CHECK(!board_discovery_encode(&source, 0x1234u, storage.bytes,
            BOARD_DISCOVERY_WIRE_MAX, (size_t *)(void *)storage.bytes),
        "encode accepted overlapping output/written ranges");
    CHECK(memcmp(output_before, storage.bytes, sizeof(output_before)) == 0,
        "output/written alias refusal modified aliased bytes");
    return 1;
}

static int test_decode_alias_refusal(void)
{
    union {
        max_align_t alignment;
        struct board_discovery_snapshot snapshot;
        uint8_t bytes[sizeof(struct board_discovery_snapshot)];
    } storage;
    uint8_t before[sizeof(storage)];

    memset(&storage, SENTINEL, sizeof(storage));
    memcpy(before, &storage, sizeof(before));
    begin_case("decode-input-output-alias");
    CHECK(!board_discovery_decode(storage.bytes, BOARD_DISCOVERY_WIRE_HEADER,
            0x1234u, &storage.snapshot),
        "decode accepted overlapping input/output ranges");
    CHECK(memcmp(before, &storage, sizeof(storage)) == 0,
        "input/output alias refusal modified aliased bytes");
    return 1;
}

int main(int argc, char **argv)
{
    const char *optimization = argc > 1 ? argv[1] : "unknown";
    int ok = 1;

    if (!test_roundtrip_one()) ok = 0;
    if (!test_roundtrip_maximum()) ok = 0;
    if (!test_byte_layout_and_label_boundaries()) ok = 0;
    if (!test_target_metadata_unknown_and_partial()) ok = 0;
    if (!test_decode_rejections()) ok = 0;
    if (!test_encode_refusals()) ok = 0;
    if (!test_encode_alias_refusals()) ok = 0;
    if (!test_decode_alias_refusal()) ok = 0;

    if (!ok || failures) {
        fprintf(stderr, "%s: %u checks, %u cases, %u failures\n",
            optimization, checks_run, cases_run, failures);
        return 1;
    }
    printf("PASS C6 discovery wire %s: %u checks, %u cases\n",
        optimization, checks_run, cases_run);
    return 0;
}
