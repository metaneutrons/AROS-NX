#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
int main(int argc, char **argv)
{
    FILE *file;
    uint8_t table[BOARD_LAYOUT_TABLE_MAX + 32];
    struct board_layout layout, before;
    size_t bytes, i;
    int result;
    if (argc != 5 || !(file = fopen(argv[1], "rb"))) return 90;
    bytes = fread(table, 1, sizeof(table), file);
    if (ferror(file) || fclose(file)) return 91;
    memset(&layout, 0xa5, sizeof(layout));
    before = layout;
    result = board_layout_parse(table, bytes, (uint32_t)strtoul(argv[2], NULL, 0), &layout);
    if (result != atoi(argv[3])) return 92;
    if (!result && memcmp(&layout, &before, sizeof(layout))) return 93;
    if (result && layout.count != (size_t)atoi(argv[4])) return 94;
    if (result) for (i = 1; i < layout.count; ++i)
        if (layout.partitions[i - 1].offset + layout.partitions[i - 1].size >
            layout.partitions[i].offset) return 95;
    /* Misaligned overlapping output isn't dereferenced on refusal. */
    if (board_layout_parse(table, bytes, 0x1000000,
            (struct board_layout *)(void *)table)) return 96;
    return 0;
}
