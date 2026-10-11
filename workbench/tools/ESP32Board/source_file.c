/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "source_file.h"

static int source_disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t ab = (uintptr_t)a, bb = (uintptr_t)b;
    if (!a || !b || !an || !bn || ab > UINTPTR_MAX - an || bb > UINTPTR_MAX - bn)
        return 0;
    return ab + an <= bb || bb + bn <= ab;
}

int boardtool_c6_source_load(const char *path, const char *sha256,
    struct boardtool_c6_source *source, char *status, size_t capacity)
{
    struct boardtool_c6_source candidate;
    struct FileInfoBlock *fib = NULL;
    BPTR file = BNULL;
    UBYTE extra;
    LONG got;
    size_t offset = 0, path_size, digest_size;
    enum board_image_error error;
    const char *failure = "Cannot read firmware file";
    int passed = 0;
    /* Refuse aliases before touching status or ownership metadata. */
    if (!path || !sha256 || !source || !status || !capacity) return 0;
    path_size = strlen(path) + 1;
    digest_size = strlen(sha256) + 1;
    if (!source_disjoint(source, sizeof(*source), status, capacity) ||
        !source_disjoint(source, sizeof(*source), path, path_size) ||
        !source_disjoint(source, sizeof(*source), sha256, digest_size) ||
        !source_disjoint(status, capacity, path, path_size) ||
        !source_disjoint(status, capacity, sha256, digest_size)) return 0;
    if (source->image && source->size &&
        !source_disjoint(status, capacity, source->image, source->size)) return 0;
    if (source->image || source->size) {
        snprintf(status, capacity, "Source already owns bytes; no write");
        return 0;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!*path || digest_size != sizeof(candidate.expected_sha256)) {
        failure = "Image path and trusted 64-hex SHA256 are required";
        goto finish;
    }
    /* Copy the original digest before DOS callbacks; retain no input pointer. */
    memcpy(candidate.expected_sha256, sha256, sizeof(candidate.expected_sha256));
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!file) goto finish;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !ExamineFH(file, fib)) goto finish;
    if (fib->fib_DirEntryType >= 0 || fib->fib_Size < 288 ||
        (ULONG)fib->fib_Size > BOARD_C6_IMAGE_MAX) {
        failure = "Not a bounded C6 application file (max 1536 KiB)";
        goto finish;
    }
    candidate.size = (size_t)fib->fib_Size;
    candidate.image = AllocVec(candidate.size, MEMF_PUBLIC);
    if (!candidate.image) { failure = "Cannot allocate owned firmware buffer"; goto finish; }
    while (offset < candidate.size) {
        size_t part = candidate.size - offset;
        if (part > 4096) part = 4096;
        got = Read(file, candidate.image + offset, (LONG)part);
        if (got <= 0 || (size_t)got > part) goto finish;
        offset += (size_t)got;
    }
    if (Read(file, &extra, 1) != 0) {
        failure = "Firmware changed length during read"; goto finish;
    }
    /* Failed close cannot publish a successful owned source. */
    if (!Close(file)) { file = BNULL; failure = "Cannot close firmware file"; goto finish; }
    file = BNULL;
    error = board_c6_image_check(candidate.image, candidate.size,
        candidate.expected_sha256, &candidate.info);
    if (error != BOARD_IMAGE_OK) {
        snprintf(status, capacity, "C6 file check refused (error %u); no write",
            (unsigned)error);
        goto cleanup;
    }
    *source = candidate;
    candidate.image = NULL;
    snprintf(status, capacity,
        "C6 format/hash OK: %lu bytes. Compatibility/recovery NOT verified; no write",
        (unsigned long)source->size);
    passed = 1;
    goto cleanup;
finish:
    snprintf(status, capacity, "%s; no write", failure);
cleanup:
    if (file) Close(file);
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (candidate.image) FreeVec(candidate.image);
    return passed;
}

int boardtool_c6_source_recheck(const struct boardtool_c6_source *source)
{
    struct board_image_info checked;
    if (!source || !source->image || !source->size ||
        source->size > BOARD_C6_IMAGE_MAX || source->info.bytes != source->size ||
        source->expected_sha256[64] != '\0' ||
        !source_disjoint(source, sizeof(*source), source->image, source->size)) return 0;
    if (board_c6_image_check(source->image, source->size,
        source->expected_sha256, &checked) != BOARD_IMAGE_OK) return 0;
    return checked.bytes == source->info.bytes &&
        checked.secure_version == source->info.secure_version &&
        checked.min_revision == source->info.min_revision &&
        checked.max_revision == source->info.max_revision &&
        checked.min_efuse_revision == source->info.min_efuse_revision &&
        checked.max_efuse_revision == source->info.max_efuse_revision &&
        checked.mmu_page_size == source->info.mmu_page_size &&
        memcmp(checked.digest, source->info.digest, sizeof(checked.digest)) == 0;
}

void boardtool_c6_source_dispose(struct boardtool_c6_source *source)
{
    uint8_t *image;
    if (!source) return;
    image = source->image;
    memset(source, 0, sizeof(*source));
    if (image) FreeVec(image);
}
