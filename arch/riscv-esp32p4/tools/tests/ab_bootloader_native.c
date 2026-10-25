/* Compile-only API integration probe with the actual IDF/toolchain headers. */
#include "sdkconfig.h"
#undef CONFIG_PARTITION_TABLE_OFFSET
#define CONFIG_PARTITION_TABLE_OFFSET 0x20000
#include "../../bootloader/aros_ab_check.h"

bool p4_ab_native_probe(const esp_partition_pos_t *partition,
                        const esp_image_metadata_t *image)
{
    return bootloader_aros_ab_pair_valid(partition, image);
}
