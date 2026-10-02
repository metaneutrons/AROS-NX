#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../framebuffer_rotate.h"

int main(void)
{
    const unsigned long width = 1280, height = 800, pitch = 2563;
    unsigned char *src = malloc(pitch * height);
    uint16_t *dst = malloc((width * height + 2) * sizeof(*dst));
    uint16_t *mirror = malloc((width * height + 2) * sizeof(*mirror));
    unsigned long phase, trial, x, y, i;
    assert(src && dst && mirror);
    for (i = 0; i < pitch * height; i++) src[i] = (unsigned char)(i * 37 + i / 257);
    for (phase = 0; phase <= 525; phase += 525)
        for (trial = 0; trial < 70; trial++)
        {
            unsigned long rx = trial ? (trial * 137) % width : 0;
            unsigned long ry = trial ? (trial * 53) % height : 0;
            unsigned long w = trial ? 1 + (trial * 29) % (width - rx) : width;
            unsigned long h = trial ? 1 + (trial * 19) % (height - ry) : height;
            for (i = 0; i < width * height + 2; i++) dst[i] = 0xa55a;
            p4_rotate_rect(dst + 1, src, pitch, width, height, phase, rx, ry, w, h);
            for (i = 0; i < width * height + 2; i++) mirror[i] = 0xa55a;
            p4_mirror_rect(mirror + 1, dst + 1, width, height, phase, rx, ry, w, h);
            assert(memcmp(dst, mirror, (width * height + 2) * sizeof(*dst)) == 0);
            assert(dst[0] == 0xa55a && dst[width * height + 1] == 0xa55a);
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++)
                {
                    unsigned long index = (width - 1 - x) * height + (y + phase) % height;
                    uint16_t expected = 0xa55a;
                    if (x >= rx && x < rx + w && y >= ry && y < ry + h)
                        expected = src[y * pitch + x * 2]
                            | ((uint16_t)src[y * pitch + x * 2 + 1] << 8);
                    assert(dst[index + 1] == expected);
                }
        }
    free(dst);
    free(mirror);
    free(src);
    puts("140 rotation + mirror cases passed (full frame, odd pitch, phase wrap, untouched pixels, guards)");
    return 0;
}
