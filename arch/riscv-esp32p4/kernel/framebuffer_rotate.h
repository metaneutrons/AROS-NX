#ifndef P4_FRAMEBUFFER_ROTATE_H
#define P4_FRAMEBUFFER_ROTATE_H

#include <stdint.h>

static inline void p4_mirror_rect(volatile uint16_t *dst,
    const volatile uint16_t *src, unsigned long screen_width,
    unsigned long screen_height, unsigned long phase,
    unsigned long x, unsigned long y, unsigned long w, unsigned long h)
{
    unsigned long xx, yy;
    for (xx = x; xx < x + w; xx++)
    {
        unsigned long column = (y + phase) % screen_height;
        unsigned long row = (screen_width - 1 - xx) * screen_height;
        for (yy = y; yy < y + h; yy++)
        {
            dst[row + column] = src[row + column];
            if (++column == screen_height) column = 0;
        }
    }
}

/* Snapshot small source-row runs, then transpose into contiguous physical
 * rows. No unaligned halfword reads and no retained logical-buffer pointer.
 * phase is the existing scanout workaround, not a geometry correction. */
static inline void p4_rotate_rect(volatile uint16_t *physical,
    const unsigned char *logical, unsigned long pitch,
    unsigned long screen_width, unsigned long screen_height,
    unsigned long phase, unsigned long x, unsigned long y,
    unsigned long w, unsigned long h)
{
    uint16_t tile[16][16];
    unsigned long tx, ty, ix, iy;

    phase %= screen_height;
    for (ty = y; ty < y + h; ty += 16)
        for (tx = x; tx < x + w; tx += 16)
        {
            unsigned long tw = x + w - tx;
            unsigned long th = y + h - ty;
            if (tw > 16) tw = 16;
            if (th > 16) th = 16;
            for (iy = 0; iy < th; iy++)
            {
                const unsigned char *src = logical + (ty + iy) * pitch + tx * 2;
                for (ix = 0; ix < tw; ix++, src += 2)
                    tile[iy][ix] = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
            }
            for (ix = 0; ix < tw; ix++)
            {
                unsigned long column = (ty + phase) % screen_height;
                volatile uint16_t *row = physical
                    + (screen_width - 1 - tx - ix) * screen_height;
                for (iy = 0; iy < th; iy++)
                {
                    row[column] = tile[iy][ix];
                    if (++column == screen_height) column = 0;
                }
            }
        }
}
#endif
