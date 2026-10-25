/*
 * Host tests for the actual C6 metadata collector, validator and layout parser.
 * All ESP-IDF calls below are deterministic mocks: this test uses no IDF
 * runtime, target hardware, flash device or radio.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "c6-private-firmware/discovery.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "c6_alloc.h"

#define MAX_MOCK_PARTITIONS 40u
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct esp_partition_iterator_opaque_ {
    size_t index;
};

static max_align_t default_flash_storage;
static max_align_t external_flash_storage;
esp_flash_t *esp_flash_default_chip = (esp_flash_t *)(void *)&default_flash_storage;

static esp_partition_t partitions[MAX_MOCK_PARTITIONS];
static size_t partition_count;
static struct esp_partition_iterator_opaque_ iterators[MAX_MOCK_PARTITIONS];
static size_t null_item_index;

static const esp_partition_t *running_first;
static const esp_partition_t *running_second;
static const esp_partition_t *boot_first;
static const esp_partition_t *boot_second;
static const esp_partition_t *update_first;
static const esp_partition_t *update_second;
static unsigned running_calls;
static unsigned boot_calls;
static unsigned update_calls;
static unsigned find_calls;
static unsigned get_calls;
static unsigned next_calls;
static unsigned release_calls;
static size_t released_index;
static unsigned state_calls;
static unsigned encryption_calls;
static unsigned secure_boot_calls;
static unsigned chip_info_calls;
static unsigned efuse_field_count_calls;
static unsigned physical_size_calls;
static unsigned effective_size_calls;
static unsigned bad_api_arguments;
static unsigned mutation_calls;
static unsigned allocation_calls;
static unsigned monitor_allocations;

static esp_err_t physical_size_result;
static esp_err_t effective_size_result;
static uint32_t physical_size_bytes;
static uint32_t effective_size_bytes;
static esp_err_t state_result;
static esp_err_t efuse_field_count_result;
static esp_ota_img_states_t state_value;
static bool encryption_enabled;
static bool secure_boot_enabled;
static esp_chip_model_t chip_model;
static uint16_t chip_revision;
static size_t efuse_field_count;

static const char *active_case;
static unsigned checks_run;
static unsigned cases_run;
static unsigned failures;

static const esp_efuse_desc_t c6_secure_version_descriptor = { 0u, 142u, 16u };
const esp_efuse_desc_t *ESP_EFUSE_SECURE_VERSION[] = {
    &c6_secure_version_descriptor,
    NULL
};

static int fail(const char *message)
{
    fprintf(stderr, "FAIL %s: %s\n", active_case ? active_case : "setup", message);
    ++failures;
    return 0;
}

#define REQUIRE(condition, message) do { \
    ++checks_run; \
    if (!(condition)) return fail(message); \
} while (0)

void *c6_test_malloc(size_t size)
{
    if (monitor_allocations) ++allocation_calls;
    return malloc(size);
}

void *c6_test_calloc(size_t count, size_t size)
{
    if (monitor_allocations) ++allocation_calls;
    return calloc(count, size);
}

void *c6_test_realloc(void *pointer, size_t size)
{
    if (monitor_allocations) ++allocation_calls;
    return realloc(pointer, size);
}

void c6_test_free(void *pointer)
{
    if (monitor_allocations) ++allocation_calls;
    free(pointer);
}

static void reset_mocks(void)
{
    size_t i;
    memset(partitions, 0, sizeof(partitions));
    memset(iterators, 0, sizeof(iterators));
    for (i = 0; i < ARRAY_COUNT(iterators); ++i) iterators[i].index = i;
    partition_count = 0;
    null_item_index = SIZE_MAX;
    running_first = running_second = NULL;
    boot_first = boot_second = NULL;
    update_first = update_second = NULL;
    running_calls = boot_calls = update_calls = 0;
    find_calls = get_calls = next_calls = release_calls = 0;
    released_index = SIZE_MAX;
    state_calls = encryption_calls = secure_boot_calls = 0;
    chip_info_calls = efuse_field_count_calls = 0;
    physical_size_calls = effective_size_calls = 0;
    bad_api_arguments = mutation_calls = allocation_calls = 0;
    monitor_allocations = 0;
    physical_size_result = ESP_OK;
    effective_size_result = ESP_OK;
    physical_size_bytes = 0x01000000u;
    effective_size_bytes = 0x00400000u;
    state_result = ESP_OK;
    efuse_field_count_result = ESP_OK;
    state_value = ESP_OTA_IMG_VALID;
    encryption_enabled = true;
    secure_boot_enabled = false;
    chip_model = CHIP_ESP32C6;
    chip_revision = 204u;
    efuse_field_count = 7u;
    esp_flash_default_chip = (esp_flash_t *)(void *)&default_flash_storage;
}

static void make_partition(esp_partition_t *part, const char *label,
    uint32_t address, uint32_t size, unsigned type, unsigned subtype,
    bool encrypted, bool readonly, esp_flash_t *chip)
{
    memset(part, 0, sizeof(*part));
    part->flash_chip = chip;
    part->type = (esp_partition_type_t)type;
    part->subtype = (esp_partition_subtype_t)subtype;
    part->address = address;
    part->size = size;
    part->erase_size = 4096u;
    (void)snprintf(part->label, sizeof(part->label), "%s", label);
    part->encrypted = encrypted;
    part->readonly = readonly;
}

static void setup_valid_inventory(void)
{
    reset_mocks();
    partition_count = 4;

    /* Deliberately unsorted IDF enumeration; output roles use sorted indexes. */
    make_partition(&partitions[0], "ota_1", 0x00210000u, 0x00100000u,
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, true, false,
        esp_flash_default_chip);
    make_partition(&partitions[1], "nvs", 0x00009000u, 0x00006000u,
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, true, true,
        esp_flash_default_chip);
    make_partition(&partitions[2], "ota_0", 0x00110000u, 0x00100000u,
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, false, true,
        esp_flash_default_chip);
    make_partition(&partitions[3], "factory", 0x00010000u, 0x00100000u,
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, true, false,
        esp_flash_default_chip);

    running_first = running_second = &partitions[0];
    boot_first = boot_second = &partitions[3];
    update_first = update_second = &partitions[2];
}

static int check_no_forbidden_calls(void)
{
    REQUIRE(allocation_calls == 0, "collector/validator/parser allocated or freed heap memory");
    REQUIRE(mutation_calls == 0, "collector wrote, erased, selected OTA, or reset the device");
    REQUIRE(bad_api_arguments == 0, "collector passed unexpected mock API arguments");
    return 1;
}

static int collect_expect(const char *name, int expected,
    struct board_discovery_snapshot *snapshot)
{
    uint8_t before[sizeof(*snapshot)];
    int result;
    active_case = name;
    if (snapshot) {
        memset(snapshot, 0xa5, sizeof(*snapshot));
        memcpy(before, snapshot, sizeof(before));
    }
    monitor_allocations = 1;
    result = c6_board_discovery_collect(snapshot);
    monitor_allocations = 0;
    ++cases_run;
    REQUIRE(result == expected, expected ? "collector unexpectedly refused valid metadata" :
        "collector accepted invalid metadata");
    if (!expected && snapshot)
        REQUIRE(memcmp(before, snapshot, sizeof(before)) == 0,
            "failure changed the caller's output snapshot");
    return check_no_forbidden_calls();
}

static int expect_failure(const char *name)
{
    struct board_discovery_snapshot output;
    return collect_expect(name, 0, &output);
}

static int expect_success(const char *name, struct board_discovery_snapshot *output)
{
    return collect_expect(name, 1, output);
}

static int test_positive_inventory(void)
{
    struct board_discovery_snapshot snapshot;
    setup_valid_inventory();
    if (!expect_success("sorted roles, split capacities, security flags", &snapshot)) return 0;
    REQUIRE(snapshot.physical_bytes == 0x01000000u, "physical chip capacity was not reported");
    REQUIRE(snapshot.layout.flash_bytes == 0x00400000u, "usable API capacity was not reported");
    REQUIRE(snapshot.physical_bytes != snapshot.layout.flash_bytes,
        "physical and effective capacities were incorrectly conflated");
    REQUIRE(snapshot.layout.count == 4u, "partition inventory count mismatch");
    REQUIRE(snapshot.layout.partitions[0].offset == 0x00009000u &&
        strcmp(snapshot.layout.partitions[0].label, "nvs") == 0,
        "partition entries were not sorted by offset");
    REQUIRE(snapshot.running == 3u && snapshot.boot == 1u && snapshot.update == 2u,
        "running, boot or update role index did not follow sorted entries");
    REQUIRE(snapshot.layout.partitions[0].flags == 3u,
        "encrypted/read-only data partition flags were not preserved");
    REQUIRE(snapshot.layout.partitions[1].flags == 1u,
        "encrypted app partition flag was not preserved");
    REQUIRE(snapshot.layout.partitions[2].flags == 2u,
        "read-only app partition flag was not preserved");
    REQUIRE(snapshot.layout.partitions[3].flags == 1u,
        "running app encryption flag was not preserved");
    REQUIRE(snapshot.running_state == 3u, "valid OTA image state mapping mismatch");
#ifdef CONFIG_EFUSE_VIRTUAL
    REQUIRE(snapshot.security_known == 0u && snapshot.security_enabled == 0u,
        "virtual-eFuse build claimed security state was known");
    REQUIRE(encryption_calls == 0 && secure_boot_calls == 0,
        "virtual-eFuse build queried security APIs");
    REQUIRE(snapshot.target_known == 0u && snapshot.chip_id == 0u &&
        snapshot.chip_revision == 0u && snapshot.secure_version == 0u,
        "virtual-eFuse build published target facts");
    REQUIRE(chip_info_calls == 0 && efuse_field_count_calls == 0,
        "virtual-eFuse build queried chip/eFuse fact APIs");
#else
    REQUIRE(snapshot.security_known == (BOARD_DISCOVERY_SECURITY_ENCRYPTION |
        BOARD_DISCOVERY_SECURITY_SECURE_BOOT), "encryption/secure-boot known bits mismatch");
    REQUIRE(snapshot.security_enabled == BOARD_DISCOVERY_SECURITY_ENCRYPTION,
        "encryption/secure-boot enabled bits mismatch");
    REQUIRE(encryption_calls == 1 && secure_boot_calls == 1,
        "security query call counts mismatch");
    REQUIRE(snapshot.target_known == BOARD_DISCOVERY_TARGET_ALL &&
        snapshot.chip_id == BOARD_DISCOVERY_CHIP_C6 && snapshot.chip_id == 13u &&
        snapshot.chip_revision == 204u && snapshot.secure_version == 7u,
        "detected C6 target facts or values mismatch");
    REQUIRE(chip_info_calls == 1 && efuse_field_count_calls == 1,
        "chip/eFuse fact API call counts mismatch");
#endif
    REQUIRE((snapshot.security_known & (BOARD_DISCOVERY_SECURITY_ROLLBACK |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK)) == 0u,
        "rollback or anti-rollback enforcement was claimed known");
    REQUIRE((snapshot.security_enabled & (BOARD_DISCOVERY_SECURITY_ROLLBACK |
        BOARD_DISCOVERY_SECURITY_ANTI_ROLLBACK)) == 0u,
        "rollback or anti-rollback enforcement was claimed enabled");
    REQUIRE(find_calls == 1 && get_calls == 4 && next_calls == 4 && release_calls == 0,
        "partition iterator API call counts mismatch");
    REQUIRE(running_calls == 2 && boot_calls == 2 && update_calls == 2,
        "role getter APIs were not rechecked exactly once");
    REQUIRE(physical_size_calls == 1 && effective_size_calls == 1,
        "capacity APIs were not queried exactly once");
    return 1;
}

static int test_wrong_chip_model_refusal(void)
{
#ifdef CONFIG_EFUSE_VIRTUAL
    return 1;
#else
    setup_valid_inventory();
    chip_model = CHIP_ESP32P4;
    if (!expect_failure("wrong chip model refuses discovery")) return 0;
    REQUIRE(chip_info_calls == 1, "wrong-model refusal did not query chip model exactly once");
    REQUIRE(efuse_field_count_calls == 0,
        "wrong-model refusal read the secure-version field");
    REQUIRE(physical_size_calls == 0 && effective_size_calls == 0 && find_calls == 0,
        "wrong-model refusal continued into flash discovery");
    return 1;
#endif
}

static int test_dirty_trailing_label(void)
{
    struct board_discovery_snapshot snapshot;
    setup_valid_inventory();
    memset(partitions[0].label + 6, 'x', sizeof(partitions[0].label) - 6u);
    if (!expect_success("IDF label with nonzero trailing bytes", &snapshot)) return 0;
    REQUIRE(snapshot.running == 3u, "bounded label match did not find the running role");
    REQUIRE(strcmp(snapshot.layout.partitions[3].label, "ota_1") == 0,
        "normalized label unexpectedly included trailing bytes");
    return 1;
}

static int test_secure_version_field_count_cases(void)
{
#ifdef CONFIG_EFUSE_VIRTUAL
    return 1;
#else
    struct board_discovery_snapshot snapshot;

    setup_valid_inventory();
    efuse_field_count = 0u;
    if (!expect_success("zero secure-version field count is known", &snapshot)) return 0;
    REQUIRE(snapshot.target_known == BOARD_DISCOVERY_TARGET_ALL &&
        snapshot.secure_version == 0u,
        "zero secure-version count was left unknown or changed");
    REQUIRE(efuse_field_count_calls == 1,
        "zero secure-version count was not read exactly once");

    setup_valid_inventory();
    efuse_field_count = 16u;
    if (!expect_success("full-width secure-version field count", &snapshot)) return 0;
    REQUIRE(snapshot.target_known == BOARD_DISCOVERY_TARGET_ALL &&
        snapshot.secure_version == 16u,
        "16-bit secure-version count was rejected or changed");
    REQUIRE(efuse_field_count_calls == 1,
        "full-width secure-version count was not read exactly once");

    setup_valid_inventory();
    efuse_field_count = 17u;
    if (!expect_success("out-of-range secure-version field count stays unknown", &snapshot)) return 0;
    REQUIRE(snapshot.target_known == (BOARD_DISCOVERY_TARGET_CHIP |
        BOARD_DISCOVERY_TARGET_REVISION) && snapshot.chip_id == BOARD_DISCOVERY_CHIP_C6 &&
        snapshot.chip_revision == 204u && snapshot.secure_version == 0u,
        "out-of-range count damaged independent chip facts or became known");
    REQUIRE(efuse_field_count_calls == 1,
        "out-of-range secure-version count was not read exactly once");

    setup_valid_inventory();
    efuse_field_count_result = ESP_FAIL;
    if (!expect_success("secure-version field read error stays unknown", &snapshot)) return 0;
    REQUIRE(snapshot.target_known == (BOARD_DISCOVERY_TARGET_CHIP |
        BOARD_DISCOVERY_TARGET_REVISION) && snapshot.chip_id == BOARD_DISCOVERY_CHIP_C6 &&
        snapshot.chip_revision == 204u && snapshot.secure_version == 0u,
        "field read error damaged independent chip facts or became known");
    REQUIRE(efuse_field_count_calls == 1,
        "secure-version field API failure was not observed exactly once");
    return 1;
#endif
}

static int test_supported_states(void)
{
    static const struct {
        esp_ota_img_states_t idf_state;
        uint8_t normalized;
    } states[] = {
        { ESP_OTA_IMG_NEW, 1u },
        { ESP_OTA_IMG_PENDING_VERIFY, 2u },
        { ESP_OTA_IMG_VALID, 3u },
        { ESP_OTA_IMG_INVALID, 4u },
        { ESP_OTA_IMG_ABORTED, 5u },
        { ESP_OTA_IMG_UNDEFINED, 6u }
    };
    size_t i;
    for (i = 0; i < ARRAY_COUNT(states); ++i) {
        struct board_discovery_snapshot snapshot;
        char name[64];
        setup_valid_inventory();
        state_value = states[i].idf_state;
        (void)snprintf(name, sizeof(name), "supported OTA state %u", (unsigned)i);
        if (!expect_success(name, &snapshot)) return 0;
        REQUIRE(snapshot.running_state == states[i].normalized,
            "supported OTA state mapped to the wrong normalized value");
    }
    return 1;
}

static int test_unknown_state_api_results(void)
{
    struct board_discovery_snapshot snapshot;
    setup_valid_inventory();
    state_result = ESP_ERR_NOT_FOUND;
    if (!expect_success("state API not found means unknown", &snapshot)) return 0;
    REQUIRE(snapshot.running_state == 0u, "not-found state was not left unknown");

    setup_valid_inventory();
    state_result = ESP_ERR_NOT_SUPPORTED;
    if (!expect_success("unsupported state API means unknown", &snapshot)) return 0;
    REQUIRE(snapshot.running_state == 0u, "unsupported state was not left unknown");
    return 1;
}

static int test_bad_state_results(void)
{
    setup_valid_inventory();
    state_result = ESP_FAIL;
    if (!expect_failure("unexpected state API error")) return 0;

    setup_valid_inventory();
    state_value = (esp_ota_img_states_t)0x1234u;
    if (!expect_failure("unsupported OTA state value")) return 0;

    setup_valid_inventory();
    state_result = ESP_ERR_INVALID_ARG;
    if (!expect_failure("invalid-argument state API error")) return 0;
    return 1;
}

static int test_null_and_capacity_failures(void)
{
    int result;
    setup_valid_inventory();
    active_case = "NULL output";
    monitor_allocations = 1;
    result = c6_board_discovery_collect(NULL);
    monitor_allocations = 0;
    ++cases_run;
    REQUIRE(result == 0, "NULL output was accepted");
    REQUIRE(physical_size_calls == 0 && effective_size_calls == 0 && find_calls == 0,
        "NULL output caused metadata API calls");
    if (!check_no_forbidden_calls()) return 0;

    setup_valid_inventory();
    esp_flash_default_chip = NULL;
    if (!expect_failure("missing default flash chip")) return 0;
    REQUIRE(physical_size_calls == 0, "missing default chip queried capacity");
    REQUIRE(chip_info_calls == 0 && efuse_field_count_calls == 0,
        "missing default chip queried C6 target facts");

    setup_valid_inventory();
    physical_size_result = ESP_FAIL;
    if (!expect_failure("physical-size API failure")) return 0;
    REQUIRE(effective_size_calls == 0, "effective-size query followed physical-size failure");

    setup_valid_inventory();
    effective_size_result = ESP_FAIL;
    if (!expect_failure("effective-size API failure")) return 0;

    setup_valid_inventory();
    physical_size_bytes = 0x00800000u;
    effective_size_bytes = 0x01000000u;
    if (!expect_failure("effective size exceeds physical size")) return 0;
    return 1;
}

static int test_iterator_failures(void)
{
    size_t i;
    setup_valid_inventory();
    partition_count = BOARD_LAYOUT_MAX_PARTITIONS + 1u;
    for (i = 4; i < partition_count; ++i) {
        char label[17];
        (void)snprintf(label, sizeof(label), "data%02zu", i);
        make_partition(&partitions[i], label, 0x00300000u + (uint32_t)(i - 4u) * 0x1000u,
            0x1000u, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
            false, false, esp_flash_default_chip);
    }
    if (!expect_failure("33-partition overflow releases live iterator")) return 0;
    REQUIRE(release_calls == 1 && released_index == BOARD_LAYOUT_MAX_PARTITIONS,
        "33-partition overflow did not release the current iterator");
    REQUIRE(next_calls == BOARD_LAYOUT_MAX_PARTITIONS,
        "33-partition overflow advanced past the refused iterator");

    setup_valid_inventory();
    null_item_index = 1;
    if (!expect_failure("null partition iterator item")) return 0;
    REQUIRE(release_calls == 1 && released_index == 1u,
        "null iterator item did not release the current iterator");
    return 1;
}

static int test_missing_or_unmatched_roles(void)
{
    esp_partition_t unmatched;
    setup_valid_inventory();
    running_first = running_second = NULL;
    if (!expect_failure("missing running partition")) return 0;
    REQUIRE(find_calls == 0, "missing running partition still enumerated the table");

    setup_valid_inventory();
    unmatched = partitions[0];
    unmatched.address += 0x1000u;
    running_first = running_second = &unmatched;
    if (!expect_failure("running role does not match inventory")) return 0;

    setup_valid_inventory();
    running_first = running_second = &partitions[1];
    if (!expect_failure("running role points to data partition")) return 0;

    setup_valid_inventory();
    partitions[3].flash_chip = (esp_flash_t *)(void *)&external_flash_storage;
    if (!expect_failure("external-flash entry in inventory")) return 0;

    setup_valid_inventory();
    unmatched = partitions[0];
    unmatched.flash_chip = (esp_flash_t *)(void *)&external_flash_storage;
    running_first = running_second = &unmatched;
    if (!expect_failure("external-flash running role")) return 0;
    return 1;
}

static int test_role_changes_and_refusal(void)
{
    esp_partition_t changed;
    setup_valid_inventory();
    changed = partitions[0];
    changed.size -= 0x1000u;
    running_second = &changed;
    if (!expect_failure("running role changed during collection")) return 0;

    setup_valid_inventory();
    changed = partitions[3];
    changed.label[0] = 'F';
    boot_second = &changed;
    if (!expect_failure("boot role changed during collection")) return 0;

    setup_valid_inventory();
    changed = partitions[2];
    changed.readonly = false;
    update_second = &changed;
    if (!expect_failure("update role changed during collection")) return 0;

    setup_valid_inventory();
    update_first = update_second = &partitions[0];
    {
        struct board_discovery_snapshot snapshot;
        if (!expect_success("single OTA slot is also the next update slot", &snapshot)) return 0;
        REQUIRE(snapshot.update == snapshot.running,
            "single-slot next-update alias was not preserved as metadata");
    }
    return 1;
}

static int test_malformed_inventory(void)
{
    setup_valid_inventory();
    partitions[3].address = 0x003f0000u;
    partitions[3].size = 0x00020000u;
    if (!expect_failure("partition extends past effective capacity")) return 0;

    setup_valid_inventory();
    partitions[1].address = 0x00010000u;
    if (!expect_failure("overlapping partition entries")) return 0;

    setup_valid_inventory();
    memset(partitions[0].label, 'x', sizeof(partitions[0].label));
    if (!expect_failure("unterminated 17-byte IDF label")) return 0;
    REQUIRE(release_calls == 1 && released_index == 0u,
        "unterminated label did not release current iterator");

    setup_valid_inventory();
    partitions[0].label[0] = '\n';
    if (!expect_failure("non-printable partition label")) return 0;
    return 1;
}

static int test_optional_unknown_roles(void)
{
    struct board_discovery_snapshot snapshot;
    setup_valid_inventory();
    boot_first = boot_second = NULL;
    update_first = update_second = NULL;
    if (!expect_success("unknown boot and update roles", &snapshot)) return 0;
    REQUIRE(snapshot.boot == BOARD_DISCOVERY_UNKNOWN_ROLE &&
        snapshot.update == BOARD_DISCOVERY_UNKNOWN_ROLE,
        "NULL optional roles were not normalized as unknown");
    return 1;
}

static int test_validator_rejects_unknown_flags(void)
{
    struct board_discovery_snapshot snapshot;
    size_t i;
    setup_valid_inventory();
    if (!expect_success("baseline for unknown-flag validator counterprobe", &snapshot)) return 0;
    for (i = 0; i < snapshot.layout.count; ++i)
        REQUIRE(snapshot.layout.partitions[i].flags <= 3u,
            "collector emitted a flag outside encryption/read-only bits");
    snapshot.layout.partitions[0].flags |= 4u;
    REQUIRE(!board_discovery_validate(&snapshot),
        "validator accepted an unknown normalized partition flag");
    return 1;
}

/* Every mutating IDF entry point is a hard failure sentinel if reached. */
static esp_err_t mutation(void)
{
    ++mutation_calls;
    return ESP_FAIL;
}

esp_err_t esp_flash_get_physical_size(esp_flash_t *chip, uint32_t *flash_size)
{
    ++physical_size_calls;
    if (chip != NULL) ++bad_api_arguments;
    if (physical_size_result == ESP_OK) *flash_size = physical_size_bytes;
    return physical_size_result;
}

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *flash_size)
{
    ++effective_size_calls;
    if (chip != NULL) ++bad_api_arguments;
    if (effective_size_result == ESP_OK) *flash_size = effective_size_bytes;
    return effective_size_result;
}

esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer,
    uint32_t address, uint32_t length)
{
    (void)chip; (void)buffer; (void)address; (void)length;
    return mutation();
}

esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t length)
{
    (void)chip; (void)start; (void)length;
    return mutation();
}

esp_err_t esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address,
    const void *buffer, uint32_t length)
{
    (void)chip; (void)address; (void)buffer; (void)length;
    return mutation();
}

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
    esp_partition_subtype_t subtype, const char *label)
{
    ++find_calls;
    if (type != ESP_PARTITION_TYPE_ANY || subtype != ESP_PARTITION_SUBTYPE_ANY || label != NULL)
        ++bad_api_arguments;
    if (!partition_count) return NULL;
    return (esp_partition_iterator_t)&iterators[0];
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    size_t index;
    ++get_calls;
    if (!iterator) {
        ++bad_api_arguments;
        return NULL;
    }
    index = iterator->index;
    if (index >= partition_count) {
        ++bad_api_arguments;
        return NULL;
    }
    if (index == null_item_index) return NULL;
    return &partitions[index];
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    size_t next;
    ++next_calls;
    if (!iterator || iterator->index >= partition_count) {
        ++bad_api_arguments;
        return NULL;
    }
    next = iterator->index + 1u;
    if (next >= partition_count) return NULL;
    return (esp_partition_iterator_t)&iterators[next];
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    ++release_calls;
    if (!iterator) {
        ++bad_api_arguments;
        return;
    }
    released_index = iterator->index;
}

esp_err_t esp_partition_write(const esp_partition_t *partition,
    size_t dst_offset, const void *src, size_t size)
{
    (void)partition; (void)dst_offset; (void)src; (void)size;
    return mutation();
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
    size_t offset, size_t size)
{
    (void)partition; (void)offset; (void)size;
    return mutation();
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    ++running_calls;
    return running_calls == 1u ? running_first : running_second;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    ++boot_calls;
    return boot_calls == 1u ? boot_first : boot_second;
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    ++update_calls;
    if (start_from != NULL) ++bad_api_arguments;
    return update_calls == 1u ? update_first : update_second;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
    esp_ota_img_states_t *ota_state)
{
    ++state_calls;
    if (!partition || partition != running_first || !ota_state) ++bad_api_arguments;
    if (state_result == ESP_OK) *ota_state = state_value;
    return state_result;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    (void)partition;
    return mutation();
}

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size,
    esp_ota_handle_t *out_handle)
{
    (void)partition; (void)image_size; (void)out_handle;
    return mutation();
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    (void)handle; (void)data; (void)size;
    return mutation();
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    (void)handle;
    return mutation();
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    (void)handle;
    return mutation();
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    return mutation();
}

esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    return mutation();
}

bool esp_flash_encryption_enabled(void)
{
    ++encryption_calls;
    return encryption_enabled;
}

bool esp_secure_boot_enabled(void)
{
    ++secure_boot_calls;
    return secure_boot_enabled;
}

void esp_chip_info(esp_chip_info_t *out_info)
{
    ++chip_info_calls;
    if (!out_info) {
        ++bad_api_arguments;
        return;
    }
    out_info->model = chip_model;
    out_info->features = 0;
    out_info->revision = chip_revision;
    out_info->cores = 1;
}

esp_err_t esp_efuse_read_field_cnt(const esp_efuse_desc_t *field[], size_t *out_cnt)
{
    ++efuse_field_count_calls;
    if (field != ESP_EFUSE_SECURE_VERSION || !out_cnt) {
        ++bad_api_arguments;
        return ESP_ERR_INVALID_ARG;
    }
    if (efuse_field_count_result == ESP_OK) *out_cnt = efuse_field_count;
    return efuse_field_count_result;
}

void esp_restart(void)
{
    ++mutation_calls;
    abort();
}

int main(void)
{
    if (!test_positive_inventory() || !test_wrong_chip_model_refusal() ||
        !test_secure_version_field_count_cases() ||
        !test_dirty_trailing_label() ||
        !test_supported_states() || !test_unknown_state_api_results() ||
        !test_bad_state_results() || !test_null_and_capacity_failures() ||
        !test_iterator_failures() || !test_missing_or_unmatched_roles() ||
        !test_role_changes_and_refusal() || !test_malformed_inventory() ||
        !test_optional_unknown_roles() || !test_validator_rejects_unknown_flags()) return 1;

#ifdef CONFIG_EFUSE_VIRTUAL
    printf("C6 discovery host mocks: virtual-eFuse build, %u cases, %u checks; no IDF/radio.\n",
        cases_run, checks_run);
#else
    printf("C6 discovery host mocks: normal build, %u cases, %u checks; no IDF/radio.\n",
        cases_run, checks_run);
#endif
    return failures ? 1 : 0;
}
