/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "image.h"
#include "layout.h"
int boardtool_load_layout(const char *path, const char *sha256,
    uint32_t flash_bytes, struct board_layout *layout, char *status, size_t capacity)
{
    BPTR file = BNULL;
    uint8_t *table = NULL, digest[32];
    char actual[65];
    LONG count, part;
    size_t used = 0, i;
    int passed = 0;
    const char *failure = "Cannot read imported partition table";
    if (!status || !capacity) return 0;
    if (!path || !*path || !sha256 || strlen(sha256) != 64 || !layout) goto finish;
    table = AllocVec(BOARD_LAYOUT_TABLE_MAX + 1, MEMF_PUBLIC);
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!table || !file) goto finish;
    do {
        part = (LONG)(BOARD_LAYOUT_TABLE_MAX + 1 - used);
        count = Read(file, table + used, part);
        if (count < 0 || count > part) goto finish;
        used += (size_t)count;
    } while (count && used < BOARD_LAYOUT_TABLE_MAX + 1);
    if (used > BOARD_LAYOUT_TABLE_MAX || !board_sha256(table, used, digest)) goto finish;
    for (i = 0; i < 32; ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    for (i = 0; i < 64; ++i) {
        char expected = sha256[i];
        if (expected >= 'A' && expected <= 'F') expected += 'a' - 'A';
        if (expected != actual[i]) {
            failure = "Imported table SHA256 mismatch"; goto finish;
        }
    }
    if (!board_layout_parse(table, used, flash_bytes, layout)) {
        failure = "Invalid imported table/capacity, overlap or bounds"; goto finish;
    }
    snprintf(status, capacity,
        "Imported preview: %lu partitions; capacity supplied, active slot UNKNOWN; no write",
        (unsigned long)layout->count);
    passed = 1;
    goto cleanup;
finish:
    snprintf(status, capacity, "%s; no write", failure);
cleanup:
    if (file) Close(file);
    if (table) FreeVec(table);
    return passed;
}
