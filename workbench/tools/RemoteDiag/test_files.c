/* Copyright (C) 2026, The AROS Development Team. */
#include "backend.h"
#include "files_test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NODE_MAX 512u
#define HANDLE_LOCK 1u
#define HANDLE_FILE 2u

struct rd_test_node {
    unsigned id;
    char name[108];
    char comment[80];
    LONG type;
    uint32_t protection;
    int32_t fib_size;
    unsigned char data[128];
    size_t data_size;
    LONG days, minutes, ticks;
    LONG entry_type;
    LONG blocks;
    uint16_t uid, gid;
    struct rd_test_node *parent;
    struct rd_test_node *volume;
    struct rd_test_node *link_target;
};

struct rd_test_handle {
    struct FileLock lock;
    unsigned kind;
    bool process_owned;
    struct rd_test_node *node;
    size_t cursor;
    size_t position;
};

static struct rd_test_node nodes[TEST_NODE_MAX];
static size_t node_count;
static struct MsgPort test_port;
static struct rd_test_handle original_cwd;
static BPTR current_dir;
static struct rd_test_node *volume_node;
static struct rd_test_node *root_node;
static struct rd_test_node *docs_node;
static struct rd_test_node *report_node;
static struct rd_test_node *empty_node;
static struct rd_test_node *secret_node;
static struct rd_test_node *escape_node;
static struct rd_test_node *inside_link_node;
static struct rd_test_node *outside_node;
static struct rd_test_node *outside_file_node;
static struct rd_test_node *outside_same_node;
static struct rd_test_node *hard_file_node;
static struct rd_test_node *hard_dir_node;
static struct rd_test_node *oversized_node;
static struct rd_test_node *race_entry;
static struct rd_test_node *race_target;
static bool race_lock;
static bool move_docs_after_read;
static bool move_docs_on_lock;
static bool fail_root_lock;
static bool fail_open;
static bool fail_seek;
static bool fail_read;
static bool early_eof;
static bool fail_close;
static bool change_on_open;
static bool change_after_read;
static bool different_open_lock;
static bool readlink_unsupported;
static unsigned read_chunk;
static unsigned read_calls;
static unsigned open_calls;
static unsigned fibs_active;
static unsigned locks_active;
static unsigned files_active;
static unsigned vectors_active;
static unsigned hook_errors;
static LONG last_error;
static unsigned assertions;
static unsigned failures;

#define CHECK(label, expression) do { \
    ++assertions; \
    if (!(expression)) { \
        ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
    } \
} while (0)

static struct rd_test_node *new_node(const char *name, LONG type,
    struct rd_test_node *parent)
{
    struct rd_test_node *node;

    if (node_count >= TEST_NODE_MAX) {
        ++hook_errors;
        return NULL;
    }
    node = &nodes[node_count];
    memset(node, 0, sizeof(*node));
    node->id = (unsigned)node_count + 1u;
    ++node_count;
    if (strlen(name) >= sizeof(node->name)) {
        ++hook_errors;
        return node;
    }
    strcpy(node->name, name);
    node->type = type;
    node->parent = parent;
    node->volume = parent ? parent->volume : node;
    node->entry_type = type;
    node->days = (LONG)(-1000 - (LONG)node->id);
    node->minutes = (LONG)(2000 + (LONG)node->id);
    node->ticks = (LONG)(-3000 - (LONG)node->id);
    node->uid = (uint16_t)(10u + node->id);
    node->gid = (uint16_t)(20u + node->id);
    return node;
}

static void set_data(struct rd_test_node *node, const char *data)
{
    size_t length = strlen(data);

    if (length > sizeof(node->data)) {
        ++hook_errors;
        return;
    }
    memcpy(node->data, data, length);
    node->data_size = length;
    node->fib_size = (int32_t)length;
    node->blocks = (LONG)((length + 511u) / 512u);
}

static void fixture_reset(void)
{
    memset(nodes, 0, sizeof(nodes));
    node_count = 0;
    memset(&test_port, 0, sizeof(test_port));
    memset(&original_cwd, 0, sizeof(original_cwd));
    volume_node = new_node("VOL", ST_ROOT, NULL);
    root_node = new_node("sandbox", ST_USERDIR, volume_node);
    docs_node = new_node("docs", ST_USERDIR, root_node);
    report_node = new_node("report.bin", ST_FILE, docs_node);
    set_data(report_node, "0123456789");
    report_node->protection = 0xfedcba04u;
    report_node->days = (LONG)0x87654321u;
    report_node->minutes = (LONG)0x81234567u;
    report_node->ticks = (LONG)0xfedcba98u;
    strcpy(report_node->comment, "keep this comment");
    empty_node = new_node("empty.bin", ST_FILE, docs_node);
    secret_node = new_node("secret.bin", ST_FILE, docs_node);
    secret_node->protection = FIBF_READ | 0x12340000u;
    set_data(secret_node, "private");
    inside_link_node = new_node("inside-link", ST_SOFTLINK, docs_node);
    inside_link_node->link_target = report_node;
    escape_node = new_node("escape", ST_SOFTLINK, root_node);
    outside_node = new_node("outside", ST_USERDIR, volume_node);
    outside_file_node = new_node("other.bin", ST_FILE, outside_node);
    set_data(outside_file_node, "outside");
    escape_node->link_target = outside_node;
    outside_same_node = new_node("report.bin", ST_FILE, outside_node);
    set_data(outside_same_node, "0123456789");
    outside_same_node->protection = report_node->protection;
    outside_same_node->days = report_node->days;
    outside_same_node->minutes = report_node->minutes;
    outside_same_node->ticks = report_node->ticks;
    outside_same_node->uid = report_node->uid;
    outside_same_node->gid = report_node->gid;
    strcpy(outside_same_node->comment, report_node->comment);
    hard_file_node = new_node("hard-file", ST_LINKFILE, docs_node);
    hard_dir_node = new_node("hard-dir", ST_LINKDIR, docs_node);
    oversized_node = new_node("oversized.bin", ST_FILE, docs_node);
    oversized_node->fib_size = -1;
    oversized_node->data_size = 1;
    oversized_node->data[0] = 'x';

    race_entry = NULL;
    race_target = NULL;
    race_lock = false;
    move_docs_after_read = false;
    move_docs_on_lock = false;
    fail_root_lock = false;
    fail_open = false;
    fail_seek = false;
    fail_read = false;
    early_eof = false;
    fail_close = false;
    change_on_open = false;
    change_after_read = false;
    different_open_lock = false;
    readlink_unsupported = false;
    read_chunk = 0;
    read_calls = 0;
    open_calls = 0;
    fibs_active = 0;
    locks_active = 0;
    files_active = 0;
    vectors_active = 0;
    hook_errors = 0;
    last_error = 0;

    original_cwd.kind = HANDLE_LOCK;
    original_cwd.process_owned = true;
    original_cwd.node = volume_node;
    original_cwd.lock.fl_Task = &test_port;
    current_dir = &original_cwd;
}

static struct rd_test_handle *new_handle(unsigned kind,
    struct rd_test_node *node)
{
    struct rd_test_handle *handle = calloc(1, sizeof(*handle));

    if (!handle) {
        ++hook_errors;
        return NULL;
    }
    handle->lock.fl_Task = &test_port;
    handle->kind = kind;
    handle->node = node;
    if (kind == HANDLE_LOCK)
        ++locks_active;
    else if (kind == HANDLE_FILE)
        ++files_active;
    else
        ++hook_errors;
    return handle;
}

static UBYTE test_ascii_fold(UBYTE byte)
{
    return byte >= 'A' && byte <= 'Z'
        ? (UBYTE)(byte + ('a' - 'A')) : byte;
}

static bool test_ascii_case_equal(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && b[i]; ++i) {
        if (test_ascii_fold((UBYTE)a[i]) != test_ascii_fold((UBYTE)b[i]))
            return false;
    }
    return a[i] == b[i];
}

static struct rd_test_node *find_child(struct rd_test_node *parent,
    const char *name)
{
    size_t i;

    /* Model native DOS behavior: exact spelling wins where supported, then
       use ASCII-insensitive lookup for ordinary case-insensitive handlers. */
    for (i = 0; i < node_count; ++i) {
        if (nodes[i].parent == parent && strcmp(nodes[i].name, name) == 0)
            return &nodes[i];
    }
    for (i = 0; i < node_count; ++i) {
        if (nodes[i].parent == parent &&
            test_ascii_case_equal(nodes[i].name, name))
            return &nodes[i];
    }
    return NULL;
}

static bool copy_field(UBYTE *out, size_t capacity, const char *text)
{
    size_t length = strlen(text);

    if (length >= capacity)
        return false;
    memcpy(out, text, length + 1);
    return true;
}

static LONG fill_fib(struct rd_test_node *node, struct FileInfoBlock *fib)
{
    memset(fib, 0, sizeof(*fib));
    fib->fib_DiskKey = node->id;
    fib->fib_DirEntryType = node->type;
    if (!copy_field(fib->fib_FileName, sizeof(fib->fib_FileName), node->name) ||
        !copy_field(fib->fib_Comment, sizeof(fib->fib_Comment), node->comment)) {
        ++hook_errors;
        return 0;
    }
    fib->fib_Protection = (LONG)node->protection;
    fib->fib_EntryType = node->entry_type;
    fib->fib_Size = node->fib_size;
    fib->fib_NumBlocks = node->blocks;
    fib->fib_Date.ds_Days = node->days;
    fib->fib_Date.ds_Minute = node->minutes;
    fib->fib_Date.ds_Tick = node->ticks;
    fib->fib_OwnerUID = node->uid;
    fib->fib_OwnerGID = node->gid;
    return 1;
}

BPTR Lock(const char *name, LONG access_mode)
{
    struct rd_test_node *node = NULL;
    struct rd_test_handle *handle;

    if (access_mode != ACCESS_READ || !name || !*name) {
        ++hook_errors;
        last_error = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    }
    if (strcmp(name, "operator-root") == 0 && fail_root_lock) {
        last_error = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    } else if (strcmp(name, "operator-root") == 0)
        node = root_node;
    else if (current_dir && current_dir->kind == HANDLE_LOCK)
        node = find_child(current_dir->node, name);
    else
        ++hook_errors;

    if (node && race_lock && node == race_entry)
        node = race_target;
    if (node && move_docs_on_lock && node == report_node) {
        docs_node->parent = outside_node;
        move_docs_on_lock = false;
    }
    if (node && node->type == ST_SOFTLINK)
        node = node->link_target;
    if (!node) {
        last_error = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    }
    handle = new_handle(HANDLE_LOCK, node);
    last_error = handle ? 0 : ERROR_OBJECT_NOT_FOUND;
    return handle;
}

BOOL UnLock(BPTR lock)
{
    if (!lock || lock->kind != HANDLE_LOCK || lock->process_owned) {
        ++hook_errors;
        return 0;
    }
    --locks_active;
    free(lock);
    return 1;
}

BPTR DupLock(BPTR lock)
{
    if (!lock || lock->kind != HANDLE_LOCK) {
        ++hook_errors;
        return BNULL;
    }
    return new_handle(HANDLE_LOCK, lock->node);
}

LONG Examine(BPTR lock, struct FileInfoBlock *fib)
{
    if (!lock || lock->kind != HANDLE_LOCK || !fib) {
        ++hook_errors;
        return 0;
    }
    if (lock->node->type == ST_ROOT || lock->node->type == ST_USERDIR)
        lock->cursor = 0;
    last_error = 0;
    return fill_fib(lock->node, fib);
}

LONG ExNext(BPTR lock, struct FileInfoBlock *fib)
{
    size_t i;

    if (!lock || lock->kind != HANDLE_LOCK || !fib ||
        (lock->node->type != ST_ROOT && lock->node->type != ST_USERDIR)) {
        ++hook_errors;
        last_error = ERROR_OBJECT_WRONG_TYPE;
        return 0;
    }
    for (i = lock->cursor; i < node_count; ++i) {
        if (nodes[i].parent == lock->node) {
            lock->cursor = i + 1;
            last_error = 0;
            return fill_fib(&nodes[i], fib);
        }
    }
    last_error = ERROR_NO_MORE_ENTRIES;
    return 0;
}

BPTR ParentDir(BPTR lock)
{
    if (!lock || lock->kind != HANDLE_LOCK) {
        ++hook_errors;
        return BNULL;
    }
    if (!lock->node->parent) {
        last_error = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    }
    return new_handle(HANDLE_LOCK, lock->node->parent);
}

LONG SameLock(BPTR first, BPTR second)
{
    if (!first || !second || first->kind != HANDLE_LOCK ||
        second->kind != HANDLE_LOCK) {
        ++hook_errors;
        return -1;
    }
    if (first->node == second->node)
        return LOCK_SAME;
    if (first->node->volume == second->node->volume)
        return LOCK_SAME_VOLUME;
    return -1;
}

LONG ReadLink(struct MsgPort *port, BPTR parent, const char *path,
    char *buffer, ULONG size)
{
    struct rd_test_node *node;

    if (port != &test_port || !parent || parent->kind != HANDLE_LOCK ||
        !path || !buffer || !size) {
        ++hook_errors;
        last_error = ERROR_OBJECT_NOT_FOUND;
        return -1;
    }
    if (readlink_unsupported) {
        last_error = ERROR_ACTION_NOT_KNOWN;
        return -1;
    }
    node = find_child(parent->node, path);
    if (!node) {
        last_error = ERROR_OBJECT_NOT_FOUND;
        return -1;
    }
    if (node->type == ST_SOFTLINK) {
        last_error = 0;
        if (size)
            buffer[0] = '\0';
        return 0;
    }
    last_error = ERROR_OBJECT_WRONG_TYPE;
    return -1;
}

APTR AllocDosObject(ULONG type, const void *tags)
{
    struct FileInfoBlock *fib;

    if (type != DOS_FIB || tags != NULL) {
        ++hook_errors;
        return NULL;
    }
    fib = calloc(1, sizeof(*fib));
    if (fib)
        ++fibs_active;
    return fib;
}

void FreeDosObject(ULONG type, APTR object)
{
    if (type != DOS_FIB || !object || !fibs_active) {
        ++hook_errors;
        return;
    }
    --fibs_active;
    free(object);
}

BPTR CurrentDir(BPTR lock)
{
    BPTR previous = current_dir;

    current_dir = lock;
    return previous;
}

LONG IoErr(void)
{
    return last_error;
}

BPTR OpenFromLock(BPTR lock)
{
    struct rd_test_handle *file;

    ++open_calls;
    if (!lock || lock->kind != HANDLE_LOCK || lock->node->type != ST_FILE) {
        ++hook_errors;
        return BNULL;
    }
    if (fail_open)
        return BNULL;
    file = new_handle(HANDLE_FILE, lock->node);
    if (!file)
        return BNULL;
    UnLock(lock); /* OpenFromLock consumes its lock on success. */
    if (change_on_open)
        ++file->node->days;
    return file;
}

BPTR DupLockFromFH(BPTR file)
{
    struct rd_test_node *node;

    if (!file || file->kind != HANDLE_FILE) {
        ++hook_errors;
        return BNULL;
    }
    node = different_open_lock ? outside_file_node : file->node;
    return new_handle(HANDLE_LOCK, node);
}

BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    if (!file || file->kind != HANDLE_FILE || !fib) {
        ++hook_errors;
        return 0;
    }
    return fill_fib(file->node, fib);
}

LONG Seek(BPTR file, LONG position, LONG mode)
{
    size_t previous;

    if (!file || file->kind != HANDLE_FILE || mode != OFFSET_BEGINNING ||
        position < 0) {
        ++hook_errors;
        return -1;
    }
    if (fail_seek)
        return -1;
    previous = file->position;
    file->position = (size_t)position;
    return (LONG)previous;
}

LONG Read(BPTR file, void *buffer, LONG length)
{
    size_t count, available;

    ++read_calls;
    if (!file || file->kind != HANDLE_FILE || length < 0 || !buffer) {
        ++hook_errors;
        return -1;
    }
    if (fail_read)
        return -1;
    if (early_eof)
        return 0;
    if (file->position >= file->node->data_size)
        return 0;
    available = file->node->data_size - file->position;
    count = (size_t)length;
    if (count > available)
        count = available;
    if (read_chunk && count > read_chunk)
        count = read_chunk;
    memcpy(buffer, file->node->data + file->position, count);
    file->position += count;
    if (change_after_read) {
        ++file->node->days;
        change_after_read = false;
    }
    if (move_docs_after_read) {
        docs_node->parent = outside_node;
        move_docs_after_read = false;
    }
    return (LONG)count;
}

BOOL Close(BPTR file)
{
    bool result = !fail_close;

    if (!file || file->kind != HANDLE_FILE) {
        ++hook_errors;
        return 0;
    }
    --files_active;
    free(file);
    return result;
}

void *AllocVec(size_t size, ULONG requirements)
{
    void *memory;

    if ((requirements & MEMF_PUBLIC) == 0 ||
        (requirements & ~(MEMF_PUBLIC | MEMF_CLEAR)) != 0) {
        ++hook_errors;
        return NULL;
    }
    memory = (requirements & MEMF_CLEAR) ? calloc(1, size) : malloc(size);
    if (memory)
        ++vectors_active;
    return memory;
}

void FreeVec(void *memory)
{
    if (!memory || !vectors_active) {
        ++hook_errors;
        return;
    }
    --vectors_active;
    free(memory);
}

#include "files_aros.c"

static struct rd_files *open_backend(void)
{
    struct rd_files *files = rd_files_open("operator-root");

    CHECK("backend opens the trusted root", files != NULL);
    CHECK("open restores the worker CurrentDir", current_dir == &original_cwd);
    return files;
}

static void check_clean(const char *label)
{
    CHECK(label, current_dir == &original_cwd);
    CHECK("no lock leaks", locks_active == 0);
    CHECK("no file-handle leaks", files_active == 0);
    CHECK("no FIB leaks", fibs_active == 0);
    CHECK("no AllocVec leaks", vectors_active == 0);
    CHECK("fake platform hooks were used correctly", hook_errors == 0);
}

static void test_metadata_and_list(void)
{
    struct rd_files *files = open_backend();
    struct rd_metadata metadata;
    int status;

    if (!files)
        return;
    status = rd_files_stat(files, "docs/report.bin", &metadata);
    CHECK("nested stat succeeds", status == RD_OK);
    CHECK("stat preserves the complete protection mask",
        metadata.protection == report_node->protection);
    CHECK("stat preserves the full DateStamp bit patterns",
        metadata.days == report_node->days &&
        metadata.minutes == report_node->minutes &&
        metadata.ticks == report_node->ticks);
    CHECK("stat preserves comment and name",
        strcmp(metadata.comment, "keep this comment") == 0 &&
        strcmp(metadata.name, "report.bin") == 0);
    status = rd_files_stat(files, "DOCS/REPORT.BIN", &metadata);
    CHECK("ASCII case variants resolve through native DOS lookup",
        status == RD_OK && strcmp(metadata.name, "report.bin") == 0);
    CHECK("stat leaves CurrentDir unchanged", current_dir == &original_cwd);

    status = rd_files_stat(files, "", &metadata);
    CHECK("empty relative path stats the pinned root", status == RD_OK &&
        metadata.type == ST_USERDIR && strcmp(metadata.name, "sandbox") == 0);

    status = rd_files_list(files, "docs", 0, &metadata);
    CHECK("list returns entry zero", status == RD_OK &&
        strcmp(metadata.name, "report.bin") == 0);
    status = rd_files_list(files, "docs", 1, &metadata);
    CHECK("list returns entry one", status == RD_OK &&
        strcmp(metadata.name, "empty.bin") == 0);
    status = rd_files_list(files, "docs", 7, &metadata);
    CHECK("list reports end of directory", status == RD_END);
    status = rd_files_list(files, "docs", 128, &metadata);
    CHECK("list refuses indexes past the 128-entry bound", status == RD_LIMIT);
    CHECK("list restores CurrentDir", current_dir == &original_cwd);
    status = rd_files_stat(files, "missing", &metadata);
    CHECK("missing path components fail closed", status == RD_DENIED);
    status = rd_files_stat(files, "docs/report.bin/child", &metadata);
    CHECK("regular files cannot be traversed as directories",
        status == RD_DENIED);

    rd_files_close(files);
    check_clean("close restores CurrentDir");
}

static void test_readonly_snapshots(void)
{
    struct rd_files *files = open_backend();
    struct rd_snapshot snapshot = {0};
    int status;

    if (!files)
        return;
    read_chunk = 2;
    status = rd_files_snapshot(files, "docs/report.bin", 2, 5, &snapshot);
    CHECK("ranged snapshot succeeds with short reads", status == RD_OK);
    CHECK("snapshot owns exact requested bytes", snapshot.data &&
        snapshot.size == 5 && memcmp(snapshot.data, "23456", 5) == 0);
    memcpy(report_node->data, "XXXXXXXXXX", 10);
    CHECK("completed snapshot no longer depends on the source",
        snapshot.data && memcmp(snapshot.data, "23456", 5) == 0);
    FreeVec(snapshot.data);
    memset(&snapshot, 0, sizeof(snapshot));

    memcpy(report_node->data, "0123456789", 10);
    status = rd_files_snapshot(files, "docs/empty.bin", 0, 0, &snapshot);
    CHECK("zero-length empty-file snapshot succeeds", status == RD_OK &&
        snapshot.data != NULL && snapshot.size == 0);
    FreeVec(snapshot.data);
    memset(&snapshot, 0, sizeof(snapshot));

    status = rd_files_snapshot(files, "docs/report.bin", 10, 0, &snapshot);
    CHECK("zero-length EOF snapshot succeeds", status == RD_OK &&
        snapshot.data != NULL && snapshot.size == 0);
    FreeVec(snapshot.data);
    memset(&snapshot, 0, sizeof(snapshot));

    status = rd_files_snapshot(files, "docs/report.bin", 9, 2, &snapshot);
    CHECK("snapshot rejects ranges past EOF without clamping",
        status == RD_BAD_REQUEST && snapshot.data == NULL);
    status = rd_files_snapshot(files, "docs/report.bin", 0,
        RD_SNAPSHOT_MAX + 1u, &snapshot);
    CHECK("snapshot rejects lengths above 4 MiB", status == RD_LIMIT);
    status = rd_files_snapshot(files, "docs/report.bin", UINT32_MAX, 0,
        &snapshot);
    CHECK("snapshot rejects offsets outside signed LONG", status == RD_BAD_REQUEST);
    status = rd_files_stat(files, "docs/oversized.bin", &(struct rd_metadata){0});
    CHECK("stat refuses sizes it cannot represent without truncation",
        status == RD_LIMIT);
    status = rd_files_snapshot(files, "docs/oversized.bin", 0, 0, &snapshot);
    CHECK("snapshot refuses sizes above signed LONG", status == RD_LIMIT);

    CHECK("snapshot calls restore CurrentDir", current_dir == &original_cwd);
    rd_files_close(files);
    check_clean("snapshot cleanup releases all handles and buffers");
}

static void test_path_and_links(void)
{
    static const char *const invalid[] = {
        "/docs/report.bin", "docs/", "docs//report.bin", "../outside",
        "docs/../outside", "docs/./report.bin", "VOL:outside",
        "docs\\report.bin", "bad\001name"
    };
    struct rd_files *files = open_backend();
    struct rd_metadata metadata;
    size_t i;
    int status;

    if (!files)
        return;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        status = rd_files_stat(files, invalid[i], &metadata);
        CHECK("invalid relative path is rejected", status == RD_BAD_REQUEST);
    }
    status = rd_files_stat(files, "escape/outside", &metadata);
    CHECK("root escape through a soft link is denied", status == RD_DENIED);
    status = rd_files_stat(files, "docs/inside-link", &metadata);
    CHECK("same-directory soft link is denied", status == RD_DENIED);
    status = rd_files_stat(files, "docs/inside-link/child", &metadata);
    CHECK("link cannot be traversed as an intermediate component",
        status == RD_DENIED);
    status = rd_files_stat(files, "docs/hard-file", &metadata);
    CHECK("hard file links are denied", status == RD_DENIED);
    status = rd_files_stat(files, "docs/hard-dir", &metadata);
    CHECK("hard directory links are denied", status == RD_DENIED);

    {
        struct rd_test_node *upper = new_node("Case.txt", ST_FILE, docs_node);
        struct rd_test_node *lower = new_node("case.txt", ST_FILE, docs_node);

        set_data(upper, "upper");
        set_data(lower, "lower");
        status = rd_files_stat(files, "docs/case.txt", &metadata);
        CHECK("case-fold collision cannot select a different canonical entry",
            status == RD_CHANGED);
    }

    race_entry = report_node;
    race_target = outside_same_node;
    race_lock = true;
    status = rd_files_stat(files, "docs/report.bin", &metadata);
    CHECK("same-named external replacement fails exact-parent verification",
        status == RD_DENIED || status == RD_CHANGED);
    race_lock = false;
    CHECK("path failures restore CurrentDir", current_dir == &original_cwd);

    rd_files_close(files);
    check_clean("path and link checks release all locks");
}

static void test_read_protection_and_limits(void)
{
    struct rd_files *files = open_backend();
    struct rd_metadata metadata;
    size_t i;
    int status;

    if (!files)
        return;
    status = rd_files_stat(files, "docs/secret.bin", &metadata);
    CHECK("stat honors the owner read-denied bit", status == RD_DENIED);
    status = rd_files_list(files, "docs", 2, &metadata);
    CHECK("list honors the owner read-denied bit on the entry",
        status == RD_DENIED);
    {
        struct rd_snapshot snapshot = {0};
        status = rd_files_snapshot(files, "docs/secret.bin", 0, 3, &snapshot);
        CHECK("snapshot honors owner read-denied bit", status == RD_DENIED &&
            snapshot.data == NULL);
    }
    docs_node->protection |= FIBF_READ;
    status = rd_files_stat(files, "docs/report.bin", &metadata);
    CHECK("directory read denial blocks child traversal", status == RD_DENIED);
    docs_node->protection &= ~FIBF_READ;

    for (i = 0; i < 130; ++i) {
        char name[32];
        struct rd_test_node *node;
        snprintf(name, sizeof(name), "item%03lu", (unsigned long)i);
        node = new_node(name, ST_FILE, docs_node);
        set_data(node, "x");
    }
    status = rd_files_stat(files, "docs/item129", &metadata);
    CHECK("component enumeration stops at the 128-entry bound",
        status == RD_LIMIT);
    status = rd_files_list(files, "docs", 127, &metadata);
    CHECK("the last in-bound list index is serviced", status == RD_OK);

    rd_files_close(files);
    check_clean("read protection and enumeration limits clean up");
}

static void test_snapshot_failures_and_changes(void)
{
    struct rd_files *files = open_backend();
    struct rd_snapshot snapshot;
    int status;

    if (!files)
        return;
    memset(&snapshot, 0, sizeof(snapshot));
    fail_open = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("failed OpenFromLock is reported", status == RD_IO &&
        snapshot.data == NULL);
    fail_open = false;

    fail_seek = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("failed bounded seek is reported", status == RD_IO &&
        snapshot.data == NULL);
    fail_seek = false;

    fail_read = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("read error is reported", status == RD_IO && snapshot.data == NULL);
    fail_read = false;

    early_eof = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("premature EOF is reported as a changed source",
        status == RD_CHANGED && snapshot.data == NULL);
    early_eof = false;

    change_on_open = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("metadata change before first read is detected",
        status == RD_CHANGED && snapshot.data == NULL);
    change_on_open = false;

    change_after_read = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("metadata change during copy is detected",
        status == RD_CHANGED && snapshot.data == NULL);

    move_docs_after_read = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("snapshot detects an ancestor moved outside the pinned root",
        status == RD_DENIED && snapshot.data == NULL);
    docs_node->parent = root_node;

    move_docs_on_lock = true;
    {
        struct rd_metadata metadata;
        status = rd_files_list(files, "docs", 0, &metadata);
        CHECK("list child ancestry is rechecked after locking the entry",
            status == RD_DENIED || status == RD_CHANGED);
    }
    docs_node->parent = root_node;

    different_open_lock = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("opened file handle must identify the pinned lock",
        status == RD_CHANGED && snapshot.data == NULL);
    different_open_lock = false;

    fail_close = true;
    status = rd_files_snapshot(files, "docs/report.bin", 0, 3, &snapshot);
    CHECK("close failure prevents publishing a snapshot", status == RD_IO &&
        snapshot.data == NULL);
    fail_close = false;

    rd_files_close(files);
    check_clean("all snapshot errors close handles and free buffers");
}

static void test_open_errors_and_softlink_fallback(void)
{
    struct rd_files *files;
    struct rd_metadata metadata;
    int status;

    fixture_reset();
    readlink_unsupported = true;
    files = open_backend();
    if (files) {
        status = rd_files_stat(files, "docs/report.bin", &metadata);
        CHECK("unsupported ReadLink falls back to enumerated DOS type",
            status == RD_OK);
        status = rd_files_stat(files, "docs/inside-link", &metadata);
        CHECK("enumerated soft-link type still fails closed",
            status == RD_DENIED);
        rd_files_close(files);
    }
    check_clean("ReadLink fallback cleanup");

    fixture_reset();
    root_node->protection |= FIBF_READ;
    files = rd_files_open("operator-root");
    CHECK("root owner read-denial refuses backend open", files == NULL);
    check_clean("failed root open releases lock and FIB");

    fixture_reset();
    fail_root_lock = true;
    files = rd_files_open("operator-root");
    CHECK("failed trusted-root Lock refuses backend open", files == NULL);
    check_clean("failed root Lock has no resources to release");
}

int main(void)
{
    fixture_reset();
    test_metadata_and_list();
    fixture_reset();
    test_readonly_snapshots();
    fixture_reset();
    test_path_and_links();
    fixture_reset();
    test_read_protection_and_limits();
    fixture_reset();
    test_snapshot_failures_and_changes();
    fixture_reset();
    test_open_errors_and_softlink_fallback();

    if (failures) {
        fprintf(stderr, "%u/%u tests failed\n", failures, assertions);
        return 1;
    }
    printf("%u file backend assertions passed\n", assertions);
    return 0;
}
