#include "flash_ab_layout.h"
#include <string.h>

static uint16_t ab_le16(const unsigned char *p)
{ return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t ab_le32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int ab_label(const unsigned char *p, const char *name)
{
    size_t n = strlen(name), i;
    if (n >= 16 || memcmp(p, name, n)) return 0;
    for (i = n; i < 16; ++i) if (p[i]) return 0;
    return 1;
}

int p4_ab_layout_resolve(const unsigned char *table, size_t bytes,
    uint32_t table_offset, uint32_t flash_bytes, uint32_t mapped_bytes,
    uint32_t running_kernel_offset, uint32_t kernel_bytes,
    uint32_t bsp_bytes, uint32_t develop_bytes, struct P4ABLayout *out)
{
    static const char *const labels[5] =
        { "KERNEL_A", "KERNEL_B", "BSP_A", "BSP_B", "DEVELOP" };
    static const unsigned char types[5] = { 0, 0, 0x40, 0x40, 0x41 };
    static const unsigned char subtypes[5] = { 0x10, 0x11, 0, 1, 0 };
    struct P4ABLayout result;
    struct P4ABExtent extents[5];
    unsigned seen = 0, apps = 0;
    size_t i, j;
    int ended = 0;
    if (!out) return P4_AB_LAYOUT_BAD_TABLE;
    memset(out, 0, sizeof(*out));
    memset(&result, 0, sizeof(result));
    memset(extents, 0, sizeof(extents));
    if (!table || bytes < 32 || bytes > 0xc00 || bytes % 32 ||
        !flash_bytes || !mapped_bytes || mapped_bytes > flash_bytes ||
        table_offset > flash_bytes || 0x1000 > flash_bytes - table_offset ||
        (table_offset & 0xfff) || !kernel_bytes || !bsp_bytes || !develop_bytes)
        return P4_AB_LAYOUT_BAD_TABLE;
    for (i = 0; i < bytes / 32; ++i)
    {
        const unsigned char *e = table + i * 32;
        uint16_t magic = ab_le16(e);
        uint32_t off, size;
        unsigned role;
        if (magic == 0xebeb ||
            (magic == 0xffff && e[2] == 0xff && e[3] == 0xff))
        { ended = 1; break; }
        if (magic != 0x50aa) return P4_AB_LAYOUT_BAD_TABLE;
        off = ab_le32(e + 4); size = ab_le32(e + 8);
        if (!size || off < table_offset + 0x1000 || (off & 0xfff) ||
            (size & 0xfff) || off >= flash_bytes || size > flash_bytes - off)
            return P4_AB_LAYOUT_BAD_RANGE;
        if (e[2] == 0 && ((off & 0xffff) || ++apps > 2))
            return P4_AB_LAYOUT_BAD_RANGE;
        for (j = 0; j < i; ++j)
        {
            const unsigned char *old = table + j * 32;
            uint32_t old_off = ab_le32(old + 4), old_size = ab_le32(old + 8);
            if (!memcmp(e + 12, old + 12, 16))
                return P4_AB_LAYOUT_AMBIGUOUS;
            if (off < old_off + old_size && old_off < off + size)
                return P4_AB_LAYOUT_BAD_RANGE;
        }
        for (role = 0; role < 5; ++role)
        {
            if (!ab_label(e + 12, labels[role])) continue;
            if ((seen & (1U << role)) || e[2] != types[role] ||
                e[3] != subtypes[role]) return P4_AB_LAYOUT_AMBIGUOUS;
            if (off >= mapped_bytes || size > mapped_bytes - off ||
                size != (role < 2 ? kernel_bytes : role < 4 ? bsp_bytes : develop_bytes))
                return P4_AB_LAYOUT_BAD_RANGE;
            /* An encrypted data partition cannot be treated as an ordinary
             * raw PKG/volume until a matching encrypted-data path exists. */
            if (ab_le32(e + 28) & ~2U) return P4_AB_LAYOUT_BAD_TABLE;
            extents[role].offset = off; extents[role].size = size;
            seen |= 1U << role;
            break;
        }
    }
    if (!ended) return P4_AB_LAYOUT_BAD_TABLE;
    if (seen != 31U || apps != 2) return P4_AB_LAYOUT_MISSING;
    if (running_kernel_offset == extents[0].offset) result.active_slot = 0;
    else if (running_kernel_offset == extents[1].offset) result.active_slot = 1;
    else return P4_AB_LAYOUT_WRONG_SLOT;
    result.kernel[0] = extents[0]; result.kernel[1] = extents[1];
    result.bsp[0] = extents[2]; result.bsp[1] = extents[3];
    result.develop = extents[4];
    *out = result;
    return P4_AB_LAYOUT_OK;
}
