/* Host harness for the actual ESP32Board image checker and reused SHA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
int main(int argc, char **argv)
{
    FILE *file;
    uint8_t *image, digest[32];
    struct board_image_info info, before;
    size_t bytes;
    long length;
    enum board_image_error result;
    static const uint8_t abc_hash[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    if (argc != 4 || !board_sha256((const uint8_t *)"abc", 3, digest) ||
        memcmp(digest, abc_hash, 32)) return 90;
    file = fopen(argv[1], "rb");
    if (!file) return 91;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (unsigned long)length > BOARD_C6_IMAGE_MAX + 1 || fseek(file, 0, SEEK_SET))
        return 92;
    bytes = (size_t)length;
    image = malloc(bytes ? bytes : 1);
    if (!image || fread(image, 1, bytes, file) != bytes || fclose(file)) return 93;
    memset(&info, 0xa5, sizeof(info));
    before = info;
    result = board_c6_image_check(image, bytes, argv[2], &info);
    if (result != (enum board_image_error)atoi(argv[3])) return 94;
    if (result != BOARD_IMAGE_OK && memcmp(&info, &before, sizeof(info))) return 95;
    if (result == BOARD_IMAGE_OK && (info.bytes != bytes ||
        !board_sha256(image, bytes, digest) || memcmp(info.digest, digest, 32))) return 96;
    /* Aliased output refusal must not modify the image. */
    if (bytes >= sizeof(info) && board_c6_image_check(image, bytes, argv[2],
            (struct board_image_info *)image) != BOARD_IMAGE_ARGUMENT) return 97;
    free(image);
    return 0;
}
