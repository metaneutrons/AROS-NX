/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "image.h"

int boardtool_check_c6(const char *path, const char *sha256,
                      char *status, size_t capacity)
{
    struct FileInfoBlock *fib = NULL;
    struct board_image_info info;
    enum board_image_error error;
    BPTR file = BNULL;
    UBYTE *owned = NULL, extra;
    LONG got;
    size_t bytes, offset = 0;
    int passed = 0;
    const char *failure = "Cannot read firmware file";
    if (!status || !capacity) return 0;
    if (!path || !*path || !sha256 || strlen(sha256) != 64) {
        failure = "Image path and trusted 64-hex SHA256 are required";
        goto finish;
    }
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!file || !fib || !ExamineFH(file, fib)) goto finish;
    if (fib->fib_DirEntryType >= 0 || fib->fib_Size < 288 ||
        (ULONG)fib->fib_Size > BOARD_C6_IMAGE_MAX) {
        failure = "Not a bounded C6 application file (max 1536 KiB)";
        goto finish;
    }
    bytes = (size_t)fib->fib_Size;
    owned = AllocVec(bytes, MEMF_PUBLIC);
    if (!owned) { failure = "Cannot allocate owned firmware buffer"; goto finish; }
    /* Freeze once; no pathname is reopened during verification. */
    while (offset < bytes) {
        size_t part = bytes - offset;
        if (part > 4096) part = 4096;
        got = Read(file, owned + offset, (LONG)part);
        if (got <= 0 || (size_t)got > part) goto finish;
        offset += (size_t)got;
    }
    if (Read(file, &extra, 1) != 0) {
        failure = "Firmware changed length during read"; goto finish;
    }
    error = board_c6_image_check(owned, bytes, sha256, &info);
    if (error != BOARD_IMAGE_OK) {
        snprintf(status, capacity, "C6 file check refused (error %u); no write",
                 (unsigned)error);
        goto cleanup;
    }
    snprintf(status, capacity,
        "C6 format/hash OK: %lu bytes. Compatibility/recovery NOT verified; no write",
        (unsigned long)info.bytes);
    passed = 1;
    goto cleanup;
finish:
    snprintf(status, capacity, "%s; no write", failure);
cleanup:
    if (owned) FreeVec(owned);
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (file) Close(file);
    return passed;
}
