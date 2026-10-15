/* Copyright (C) 2026, The AROS Development Team. */
#ifndef RD_BACKEND_H
#define RD_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RD_SNAPSHOT_MAX (4u * 1024u * 1024u)
#define RD_PATH_MAX 512u
#define RD_DATA_MAX 4096u
enum rd_status { RD_OK=0, RD_BAD_REQUEST=1, RD_DENIED=2, RD_UNAVAILABLE=3,
    RD_IO=4, RD_CHANGED=5, RD_LIMIT=6, RD_SESSION=7, RD_BUSY=8, RD_END=9 };
struct rd_snapshot { uint8_t *data; uint32_t size, width, height; };
struct rd_metadata {
    uint32_t protection, size;
    int32_t type, days, minutes, ticks;
    char name[108], comment[80];
};
/* Native functions run only in an ordinary DOS worker, never an IRQ/USB callback.
   All pointers and events remain owned until synchronous native calls finish. */
struct rd_screen;
struct rd_files;
struct rd_screen *rd_screen_open(const char *public_name, bool allow_mouse);
bool rd_screen_has_mouse(const struct rd_screen *);
void rd_screen_close(struct rd_screen *);
int rd_screen_capture(struct rd_screen *, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, struct rd_snapshot *);
int rd_screen_mouse(struct rd_screen *, uint32_t x, uint32_t y, unsigned button);
/* button0 is move-only;1/2/3 are complete left/right/middle clicks. No held drag. */
struct rd_files *rd_files_open(const char *trusted_root);
void rd_files_close(struct rd_files *);
int rd_files_stat(struct rd_files *, const char *relative, struct rd_metadata *);
int rd_files_list(struct rd_files *, const char *relative, uint32_t index,
    struct rd_metadata *);
int rd_files_snapshot(struct rd_files *, const char *relative, uint32_t offset,
    uint32_t length, struct rd_snapshot *);
/* Snapshot output must be fresh/zero-initialized, never an existing allocation.
   Snapshot storage comes from AllocVec; caller releases after transfer/cancel.
   Capture is redraw-nonatomic; file before/after metadata detects some edits,
   not every same-metadata rewrite. Completed snapshots never reread the source. */
bool rd_valid_relative(const char *path);
#endif
