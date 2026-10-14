/* Copyright (C) 2026, The AROS Development Team. */
#include "backend.h"

#ifdef RD_FILES_TEST
#include "files_test_platform.h"
#else
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <limits.h>
#include <string.h>

#define RD_FILE_ENUM_MAX 128u
#define RD_FILE_READ_CHUNK 4096u

struct rd_files {
    BPTR root;
};

struct rd_walk {
    BPTR previous;
    BPTR current;
    bool active;
};

static bool rd_is_link_type(LONG type)
{
    return type == ST_SOFTLINK || type == ST_LINKDIR || type == ST_LINKFILE;
}

static bool rd_is_directory_type(LONG type)
{
    return type == ST_ROOT || type == ST_USERDIR;
}

static bool rd_is_regular_type(LONG type)
{
    return type == ST_FILE;
}

static bool rd_field_length(const UBYTE *field, size_t capacity, size_t *length)
{
    size_t i;

    for (i = 0; i < capacity; ++i) {
        if (field[i] == '\0') {
            *length = i;
            return true;
        }
    }
    return false;
}

static bool rd_fields_equal(const UBYTE *a, const UBYTE *b, size_t capacity)
{
    size_t a_length, b_length;

    if (!rd_field_length(a, capacity, &a_length) ||
        !rd_field_length(b, capacity, &b_length) || a_length != b_length)
        return false;
    return memcmp(a, b, a_length) == 0;
}

static bool rd_fib_size_long(const struct FileInfoBlock *fib,
    uint32_t *size)
{
    uint64_t value = (uint64_t)fib->fib_Size;

    if (value > (uint64_t)INT32_MAX)
        return false;
    *size = (uint32_t)value;
    return true;
}

static bool rd_fib_equal(const struct FileInfoBlock *a,
    const struct FileInfoBlock *b)
{
    return a->fib_DirEntryType == b->fib_DirEntryType &&
        a->fib_Protection == b->fib_Protection &&
        a->fib_EntryType == b->fib_EntryType &&
        a->fib_Size == b->fib_Size &&
        a->fib_NumBlocks == b->fib_NumBlocks &&
        a->fib_Date.ds_Days == b->fib_Date.ds_Days &&
        a->fib_Date.ds_Minute == b->fib_Date.ds_Minute &&
        a->fib_Date.ds_Tick == b->fib_Date.ds_Tick &&
        a->fib_OwnerUID == b->fib_OwnerUID &&
        a->fib_OwnerGID == b->fib_OwnerGID &&
        rd_fields_equal(a->fib_FileName, b->fib_FileName,
            sizeof(a->fib_FileName)) &&
        rd_fields_equal(a->fib_Comment, b->fib_Comment,
            sizeof(a->fib_Comment));
}

static int rd_metadata_from_fib(const struct FileInfoBlock *fib,
    struct rd_metadata *metadata)
{
    size_t name_length, comment_length;
    uint32_t size;

    if (!rd_fib_size_long(fib, &size))
        return RD_LIMIT;
    if (!rd_field_length(fib->fib_FileName, sizeof(fib->fib_FileName),
            &name_length) ||
        !rd_field_length(fib->fib_Comment, sizeof(fib->fib_Comment),
            &comment_length))
        return RD_IO;

    memset(metadata, 0, sizeof(*metadata));
    metadata->protection = (uint32_t)fib->fib_Protection;
    metadata->size = size;
    metadata->type = (int32_t)fib->fib_DirEntryType;
    metadata->days = (int32_t)fib->fib_Date.ds_Days;
    metadata->minutes = (int32_t)fib->fib_Date.ds_Minute;
    metadata->ticks = (int32_t)fib->fib_Date.ds_Tick;
    memcpy(metadata->name, fib->fib_FileName, name_length + 1);
    memcpy(metadata->comment, fib->fib_Comment, comment_length + 1);
    return RD_OK;
}

static int rd_check_object_type(const struct FileInfoBlock *fib)
{
    if (rd_is_link_type(fib->fib_DirEntryType))
        return RD_DENIED;
    if (rd_is_directory_type(fib->fib_DirEntryType) ||
        rd_is_regular_type(fib->fib_DirEntryType))
        return RD_OK;
    return RD_DENIED;
}

static int rd_check_readable(const struct FileInfoBlock *fib)
{
    /* DOS protection flags are active-low for the owner. Do not rely on a
       permissive handler to enforce the owner's explicit no-read bit. */
    return (((uint32_t)fib->fib_Protection & (uint32_t)FIBF_READ) != 0)
        ? RD_DENIED : RD_OK;
}

static int rd_walk_begin(struct rd_files *files, struct rd_walk *walk)
{
    BPTR root_current;

    memset(walk, 0, sizeof(*walk));
    if (!files || !files->root)
        return RD_SESSION;

    root_current = DupLock(files->root);
    if (!root_current)
        return RD_IO;

    walk->previous = CurrentDir(root_current);
    walk->current = root_current;
    walk->active = true;
    return RD_OK;
}

static void rd_walk_end(struct rd_walk *walk)
{
    BPTR active;

    if (!walk->active)
        return;

    active = CurrentDir(walk->previous);
    if (active)
        UnLock(active);
    walk->active = false;
    walk->current = BNULL;
}

static void rd_walk_advance(struct rd_walk *walk, BPTR next)
{
    BPTR previous = CurrentDir(next);

    if (previous)
        UnLock(previous);
    walk->current = next;
}

static bool rd_component_matches(const UBYTE *name, size_t name_capacity,
    const char *component, size_t component_length)
{
    size_t name_length;
    size_t i;

    if (!rd_field_length(name, name_capacity, &name_length) ||
        name_length != component_length)
        return false;

    /* DOS handlers commonly compare ASCII names without case sensitivity.
       Fold only for selecting an enumerated candidate; Lock() below receives
       the caller's original component and the FIB comparison still requires
       that Lock() resolved this exact entry. Other bytes remain byte-exact. */
    for (i = 0; i < component_length; ++i) {
        UBYTE entry_byte = name[i];
        UBYTE requested_byte = (UBYTE)component[i];

        if (entry_byte >= 'A' && entry_byte <= 'Z')
            entry_byte = (UBYTE)(entry_byte + ('a' - 'A'));
        if (requested_byte >= 'A' && requested_byte <= 'Z')
            requested_byte = (UBYTE)(requested_byte + ('a' - 'A'));
        if (entry_byte != requested_byte)
            return false;
    }
    return true;
}

/* ReadLink is the DOS-level no-follow check. A one-byte buffer is enough:
   -2 means that a link exists but its target did not fit. */
static int rd_check_not_softlink(BPTR parent, const char *name)
{
    struct FileLock *parent_file_lock = (struct FileLock *)BADDR(parent);
    char target[1];
    LONG result, error;

    if (!parent_file_lock || !parent_file_lock->fl_Task)
        return RD_DENIED;

    result = ReadLink(parent_file_lock->fl_Task, parent,
        (CONST_STRPTR)name, (STRPTR)target, sizeof(target));
    if (result >= 0 || result == -2)
        return RD_DENIED;

    error = (LONG)IoErr();
    if (error == ERROR_OBJECT_NOT_FOUND ||
        error == ERROR_OBJECT_WRONG_TYPE ||
        error == ERROR_ACTION_NOT_KNOWN)
        return RD_OK;
    return RD_IO;
}

static int rd_verify_parent(BPTR child, BPTR expected_parent)
{
    BPTR parent = ParentDir(child);
    LONG same;

    if (!parent)
        return RD_DENIED;
    same = SameLock(parent, expected_parent);
    UnLock(parent);
    return same == LOCK_SAME ? RD_OK : RD_DENIED;
}

/* Walk the live parent chain after resolution. Immediate-parent checks during
   the walk do not detect an ancestor moved outside the pinned root afterward.
   DOS has no atomic openat/no-follow primitive, so this bounded recheck detects
   observable namespace changes without claiming an atomic namespace snapshot. */
static int rd_verify_root_ancestry(struct rd_files *files, BPTR target,
    struct FileInfoBlock *fib)
{
    BPTR current = DupLock(target);
    unsigned int depth;

    if (!current)
        return RD_IO;

    for (depth = 0; depth <= 64u; ++depth) {
        BPTR parent;
        LONG same = SameLock(current, files->root);

        if (same == LOCK_SAME) {
            UnLock(current);
            return RD_OK;
        }
        if (depth == 64u) {
            UnLock(current);
            return RD_LIMIT;
        }
        if (!Examine(current, fib)) {
            UnLock(current);
            return RD_IO;
        }
        if (rd_check_object_type(fib) != RD_OK ||
            rd_check_readable(fib) != RD_OK) {
            UnLock(current);
            return RD_DENIED;
        }
        parent = ParentDir(current);
        UnLock(current);
        if (!parent)
            return RD_DENIED;
        current = parent;
    }

    UnLock(current);
    return RD_LIMIT;
}

/* Lock one already-enumerated entry while the current directory is parent.
   Comparing the pre-lock entry with Examine() catches replacement races; the
   ParentDir check rejects resolved targets outside this exact parent. */
static int rd_lock_checked_entry(BPTR parent, const char *name,
    const struct FileInfoBlock *entry, struct FileInfoBlock *fib,
    BPTR *child_out)
{
    BPTR child;
    int status;

    status = rd_check_not_softlink(parent, name);
    if (status != RD_OK)
        return status;

    child = Lock((CONST_STRPTR)name, ACCESS_READ);
    if (!child)
        return RD_CHANGED;

    if (!Examine(child, fib)) {
        UnLock(child);
        return RD_IO;
    }
    if (rd_is_link_type(fib->fib_DirEntryType) || !rd_fib_equal(entry, fib)) {
        UnLock(child);
        return RD_CHANGED;
    }

    status = rd_check_object_type(fib);
    if (status == RD_OK)
        status = rd_check_readable(fib);
    if (status == RD_OK)
        status = rd_verify_parent(child, parent);
    if (status != RD_OK) {
        UnLock(child);
        return status;
    }

    *child_out = child;
    return RD_OK;
}

static int rd_find_child(BPTR parent, const char *component,
    size_t component_length, struct FileInfoBlock *fib, BPTR *child_out,
    struct FileInfoBlock *child_fib)
{
    struct FileInfoBlock entry;
    unsigned int scanned;
    int status;

    if (!Examine(parent, fib))
        return RD_IO;
    if (!rd_is_directory_type(fib->fib_DirEntryType))
        return RD_DENIED;
    status = rd_check_readable(fib);
    if (status != RD_OK)
        return status;

    for (scanned = 0; scanned < RD_FILE_ENUM_MAX; ++scanned) {
        if (!ExNext(parent, fib)) {
            if ((LONG)IoErr() == ERROR_NO_MORE_ENTRIES)
                return RD_DENIED;
            return RD_IO;
        }

        if (!rd_component_matches(fib->fib_FileName,
                sizeof(fib->fib_FileName), component, component_length))
            continue;

        /* On case-sensitive handlers, ASCII-fold collisions can produce more
           than one candidate. Resolve the original spelling once, then require
           its canonical FIB to match this candidate; a collision fails closed. */
        entry = *fib;
        if (rd_is_link_type(entry.fib_DirEntryType))
            return RD_DENIED;
        if (rd_check_object_type(&entry) != RD_OK)
            return RD_DENIED;
        status = rd_lock_checked_entry(parent, component, &entry, fib,
            child_out);
        if (status != RD_OK)
            return status;
        *child_fib = *fib;
        return RD_OK;
    }

    return RD_LIMIT;
}

static int rd_resolve(struct rd_files *files, const char *relative,
    struct rd_walk *walk, struct FileInfoBlock *fib, BPTR *target_out,
    struct FileInfoBlock *target_fib)
{
    size_t path_length, position = 0;
    BPTR target = BNULL;
    int status;

    if (!rd_valid_relative(relative))
        return RD_BAD_REQUEST;

    status = rd_walk_begin(files, walk);
    if (status != RD_OK)
        return status;

    path_length = strlen(relative);
    if (!path_length) {
        target = DupLock(files->root);
        if (!target)
            return RD_IO;
        if (!Examine(target, fib)) {
            UnLock(target);
            return RD_IO;
        }
        status = rd_check_object_type(fib);
        if (status == RD_OK)
            status = rd_check_readable(fib);
        if (status != RD_OK) {
            UnLock(target);
            return status;
        }
        *target_fib = *fib;
        *target_out = target;
        return RD_OK;
    }

    while (position < path_length) {
        char component[RD_PATH_MAX];
        size_t start = position, component_length;
        BPTR child = BNULL;
        bool final;

        while (position < path_length && relative[position] != '/')
            ++position;
        component_length = position - start;
        if (!component_length || component_length >= sizeof(component) ||
            component_length >= sizeof(fib->fib_FileName))
            return RD_BAD_REQUEST;
        memcpy(component, relative + start, component_length);
        component[component_length] = '\0';
        final = position == path_length;
        if (!final)
            ++position;

        status = rd_find_child(walk->current, component, component_length,
            fib, &child, target_fib);
        if (status != RD_OK)
            return status;

        if (final) {
            *target_out = child;
            return RD_OK;
        }

        if (!rd_is_directory_type(target_fib->fib_DirEntryType)) {
            UnLock(child);
            return RD_DENIED;
        }
        rd_walk_advance(walk, child);
    }

    return RD_BAD_REQUEST;
}

static int rd_next_entry(BPTR directory, struct FileInfoBlock *fib)
{
    if (ExNext(directory, fib))
        return 1;
    return (LONG)IoErr() == ERROR_NO_MORE_ENTRIES ? 0 : -1;
}

struct rd_files *rd_files_open(const char *trusted_root)
{
    struct rd_files *files = NULL;
    struct FileInfoBlock *fib = NULL;
    BPTR root = BNULL;

    if (!trusted_root || !*trusted_root)
        return NULL;

    root = Lock((CONST_STRPTR)trusted_root, ACCESS_READ);
    if (!root)
        return NULL;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(root, fib) ||
        !rd_is_directory_type(fib->fib_DirEntryType) ||
        rd_check_readable(fib) != RD_OK)
        goto cleanup;

    files = AllocVec(sizeof(*files), MEMF_PUBLIC | MEMF_CLEAR);
    if (!files)
        goto cleanup;
    files->root = root;
    root = BNULL;

cleanup:
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (root)
        UnLock(root);
    return files;
}

void rd_files_close(struct rd_files *files)
{
    if (!files)
        return;
    if (files->root)
        UnLock(files->root);
    FreeVec(files);
}

int rd_files_stat(struct rd_files *files, const char *relative,
    struct rd_metadata *metadata)
{
    struct rd_walk walk;
    struct FileInfoBlock *fib = NULL;
    struct FileInfoBlock target_fib;
    struct rd_metadata candidate;
    BPTR target = BNULL;
    int status;

    if (!metadata)
        return RD_BAD_REQUEST;
    if (!rd_valid_relative(relative))
        return RD_BAD_REQUEST;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib)
        return RD_IO;

    status = rd_resolve(files, relative, &walk, fib, &target, &target_fib);
    if (status == RD_OK)
        status = rd_verify_root_ancestry(files, target, fib);
    if (status == RD_OK && !Examine(target, fib))
        status = RD_IO;
    if (status == RD_OK && !rd_fib_equal(&target_fib, fib))
        status = RD_CHANGED;
    if (status == RD_OK)
        status = rd_metadata_from_fib(fib, &candidate);
    if (status == RD_OK)
        status = rd_verify_root_ancestry(files, target, fib);
    if (target)
        UnLock(target);
    rd_walk_end(&walk);
    FreeDosObject(DOS_FIB, fib);

    if (status == RD_OK)
        *metadata = candidate;
    return status;
}

int rd_files_list(struct rd_files *files, const char *relative, uint32_t index,
    struct rd_metadata *metadata)
{
    struct rd_walk walk;
    struct FileInfoBlock *fib = NULL;
    struct FileInfoBlock target_fib, entry;
    struct rd_metadata candidate;
    BPTR directory = BNULL, directory_current = BNULL;
    BPTR previous_current = BNULL;
    unsigned int scanned;
    int status;

    if (!metadata || !rd_valid_relative(relative))
        return RD_BAD_REQUEST;
    if (index >= RD_FILE_ENUM_MAX)
        return RD_LIMIT;

    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib)
        return RD_IO;
    status = rd_resolve(files, relative, &walk, fib, &directory,
        &target_fib);
    if (status != RD_OK)
        goto cleanup;
    status = rd_verify_root_ancestry(files, directory, fib);
    if (status != RD_OK)
        goto cleanup;
    if (!Examine(directory, fib)) {
        status = RD_IO;
        goto cleanup;
    }
    if (!rd_fib_equal(&target_fib, fib)) {
        status = RD_CHANGED;
        goto cleanup;
    }
    if (!rd_is_directory_type(target_fib.fib_DirEntryType)) {
        status = RD_DENIED;
        goto cleanup;
    }
    status = rd_check_readable(&target_fib);
    if (status != RD_OK)
        goto cleanup;

    directory_current = DupLock(directory);
    if (!directory_current) {
        status = RD_IO;
        goto cleanup;
    }
    previous_current = CurrentDir(directory_current);

    if (!Examine(directory, fib)) {
        status = RD_IO;
        goto cleanup;
    }
    for (scanned = 0; scanned <= index; ++scanned) {
        int next = rd_next_entry(directory, fib);

        if (next == 0) {
            status = RD_END;
            goto cleanup;
        }
        if (next < 0) {
            status = RD_IO;
            goto cleanup;
        }
    }

    entry = *fib;
    if (rd_is_link_type(entry.fib_DirEntryType)) {
        status = RD_DENIED;
        goto cleanup;
    }
    if (rd_check_object_type(&entry) != RD_OK) {
        status = RD_DENIED;
        goto cleanup;
    }
    {
        char name[sizeof(entry.fib_FileName)];
        size_t name_length;
        BPTR child = BNULL;
        struct FileInfoBlock child_fib;

        if (!rd_field_length(entry.fib_FileName, sizeof(entry.fib_FileName),
                &name_length) || name_length >= sizeof(name)) {
            status = RD_IO;
            goto cleanup;
        }
        memcpy(name, entry.fib_FileName, name_length + 1);
        status = rd_lock_checked_entry(directory, name, &entry, fib, &child);
        if (status != RD_OK)
            goto cleanup;
        child_fib = *fib;
        status = rd_verify_root_ancestry(files, child, fib);
        if (status == RD_OK && !Examine(child, fib))
            status = RD_IO;
        if (status == RD_OK && !rd_fib_equal(&child_fib, fib))
            status = RD_CHANGED;
        if (status == RD_OK)
            status = rd_metadata_from_fib(fib, &candidate);
        if (status == RD_OK)
            status = rd_verify_root_ancestry(files, child, fib);
        UnLock(child);
    }

cleanup:
    if (directory_current) {
        BPTR active = CurrentDir(previous_current);
        if (active)
            UnLock(active);
    }
    if (directory)
        UnLock(directory);
    rd_walk_end(&walk);
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (status == RD_OK)
        *metadata = candidate;
    return status;
}

int rd_files_snapshot(struct rd_files *files, const char *relative,
    uint32_t offset, uint32_t length, struct rd_snapshot *snapshot)
{
    struct rd_walk walk;
    struct FileInfoBlock *fib = NULL;
    struct FileInfoBlock target_fib, before_fib, after_fib;
    BPTR target = BNULL, duplicate = BNULL, file = BNULL, opened_lock = BNULL;
    uint8_t *data = NULL;
    uint32_t file_size;
    size_t copied = 0;
    int status;

    if (!snapshot || !rd_valid_relative(relative))
        return RD_BAD_REQUEST;
    if (snapshot->data)
        return RD_BAD_REQUEST;
    snapshot->size = 0;
    snapshot->width = 0;
    snapshot->height = 0;
    if (length > RD_SNAPSHOT_MAX)
        return RD_LIMIT;
    if (offset > (uint32_t)INT32_MAX ||
        (uint64_t)offset + (uint64_t)length > (uint64_t)INT32_MAX)
        return RD_BAD_REQUEST;

    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib)
        return RD_IO;
    status = rd_resolve(files, relative, &walk, fib, &target, &target_fib);
    if (status != RD_OK)
        goto cleanup;
    status = rd_verify_root_ancestry(files, target, fib);
    if (status != RD_OK)
        goto cleanup;
    if (!Examine(target, fib)) {
        status = RD_IO;
        goto cleanup;
    }
    if (!rd_fib_equal(&target_fib, fib)) {
        status = RD_CHANGED;
        goto cleanup;
    }
    target_fib = *fib;
    if (!rd_is_regular_type(target_fib.fib_DirEntryType)) {
        status = RD_DENIED;
        goto cleanup;
    }
    if (!rd_fib_size_long(&target_fib, &file_size)) {
        status = RD_LIMIT;
        goto cleanup;
    }
    if (offset > file_size || length > file_size - offset) {
        status = RD_BAD_REQUEST;
        goto cleanup;
    }

    data = AllocVec(length ? (size_t)length : 1u, MEMF_PUBLIC);
    if (!data) {
        status = RD_IO;
        goto cleanup;
    }

    duplicate = DupLock(target);
    if (!duplicate) {
        status = RD_IO;
        goto cleanup;
    }
    file = OpenFromLock(duplicate);
    if (!file) {
        UnLock(duplicate);
        duplicate = BNULL;
        status = RD_IO;
        goto cleanup;
    }
    duplicate = BNULL; /* OpenFromLock consumed the duplicate. */

    opened_lock = DupLockFromFH(file);
    if (!opened_lock) {
        status = RD_IO;
        goto cleanup;
    }
    if (SameLock(target, opened_lock) != LOCK_SAME) {
        status = RD_CHANGED;
        goto cleanup;
    }
    UnLock(opened_lock);
    opened_lock = BNULL;

    if (!ExamineFH(file, &before_fib)) {
        status = RD_IO;
        goto cleanup;
    }
    if (!rd_is_regular_type(before_fib.fib_DirEntryType) ||
        rd_check_readable(&before_fib) != RD_OK) {
        status = RD_DENIED;
        goto cleanup;
    }
    if (!rd_fib_equal(&target_fib, &before_fib)) {
        status = RD_CHANGED;
        goto cleanup;
    }
    if (!rd_fib_size_long(&before_fib, &file_size)) {
        status = RD_LIMIT;
        goto cleanup;
    }
    if (offset > file_size || length > file_size - offset) {
        status = RD_CHANGED;
        goto cleanup;
    }

    if (Seek(file, (LONG)offset, OFFSET_BEGINNING) == -1) {
        status = RD_IO;
        goto cleanup;
    }
    while (copied < (size_t)length) {
        size_t part = (size_t)length - copied;
        LONG got;

        if (part > RD_FILE_READ_CHUNK)
            part = RD_FILE_READ_CHUNK;
        got = Read(file, data + copied, (LONG)part);
        if (got < 0) {
            status = RD_IO;
            goto cleanup;
        }
        if (got == 0) {
            status = RD_CHANGED;
            goto cleanup;
        }
        if ((size_t)got > part) {
            status = RD_IO;
            goto cleanup;
        }
        copied += (size_t)got;
    }

    if (!ExamineFH(file, &after_fib)) {
        status = RD_IO;
        goto cleanup;
    }
    if (!rd_fib_equal(&before_fib, &after_fib)) {
        status = RD_CHANGED;
        goto cleanup;
    }
    status = rd_verify_root_ancestry(files, target, fib);
    if (status != RD_OK)
        goto cleanup;
    status = RD_OK;

cleanup:
    if (opened_lock)
        UnLock(opened_lock);
    if (duplicate)
        UnLock(duplicate);
    if (file && !Close(file))
        status = RD_IO;
    if (target)
        UnLock(target);
    rd_walk_end(&walk);
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (status == RD_OK) {
        snapshot->data = data;
        snapshot->size = length;
        data = NULL;
    }
    if (data)
        FreeVec(data);
    return status;
}
