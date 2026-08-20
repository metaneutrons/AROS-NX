/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The application descriptor an ESP-IDF bootloader insists on.
*/

#include <inttypes.h>

/*
 * When an ESP-IDF second stage bootloader loads an application it reads the
 * first bytes of segment 0 as an esp_app_desc_t and checks the efuse block
 * revision range it finds there. It does that without looking at the magic
 * word first, so an image with no descriptor at all is not treated as
 * "no constraint" - the bootloader reads whatever code happens to be there
 * and rejects the image:
 *
 *     E boot_comm: Image requires efuse blk rev >= v509.47, but chip is v0.3
 *     E boot: OTA app partition slot 0 is not bootable
 *
 * Hence this. The fields the bootloader reads are the two revision bounds;
 * the rest is filled in because esptool and the IDF tools print it, and a
 * descriptor that says AROS is more use than one that says nothing.
 *
 * It has to be the first thing in the image, which the link script arranges,
 * and it costs nothing on the ROM boot path: that loader takes the entry
 * address from the image header rather than assuming the segment starts with
 * code.
 */
struct esp_app_desc
{
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char     version[32];
    char     project_name[32];
    char     time[16];
    char     date[16];
    char     idf_ver[32];
    uint8_t  app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full;
    uint16_t max_efuse_blk_rev_full;
    uint8_t  mmu_page_size;
    uint8_t  reserv3[3];
    uint32_t reserv2[18];
};

_Static_assert(__builtin_offsetof(struct esp_app_desc, min_efuse_blk_rev_full) == 176,
               "the bootloader reads the revision bounds at a fixed offset");
_Static_assert(sizeof(struct esp_app_desc) == 256, "esp_app_desc_t is 256 bytes");

const struct esp_app_desc __attribute__((used, section(".flash.appdesc"))) aros_app_desc =
{
    .magic_word             = 0xABCD5432,
    .version                = "AROS",
    .project_name           = "AROS",
    .idf_ver                = "none",
    /* No constraint: this image does not care which efuse block revision
       the part carries, and saying so is the whole point of the file. */
    .min_efuse_blk_rev_full = 0,
    .max_efuse_blk_rev_full = 0xFFFF,
    /* log2 of the flash MMU page size the chip is configured for */
    .mmu_page_size          = 16,
};
