/* Copyright (C) 2026, The AROS Development Team. */
#ifndef RD_FILES_TEST_PLATFORM_H
#define RD_FILES_TEST_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef int BOOL;
typedef const char *CONST_STRPTR;
typedef char *STRPTR;
typedef struct rd_test_handle *BPTR;
typedef void *APTR;

struct MsgPort { unsigned unused; };
struct FileLock { struct MsgPort *fl_Task; };
struct DateStamp {
    LONG ds_Days;
    LONG ds_Minute;
    LONG ds_Tick;
};
struct FileInfoBlock {
    uintptr_t fib_DiskKey;
    LONG fib_DirEntryType;
    UBYTE fib_FileName[108];
    LONG fib_Protection;
    LONG fib_EntryType;
    LONG fib_Size;
    LONG fib_NumBlocks;
    struct DateStamp fib_Date;
    UBYTE fib_Comment[80];
    uint16_t fib_OwnerUID;
    uint16_t fib_OwnerGID;
    UBYTE fib_Reserved[32];
};

#define BNULL ((BPTR)0)
#define BADDR(lock) ((void *)(lock))
#define MEMF_PUBLIC 0x0001u
#define MEMF_CLEAR  0x0002u
#define DOS_FIB 2u
#define ACCESS_READ (-2)
#define MODE_OLDFILE 1005
#define OFFSET_BEGINNING (-1)
#define FIBF_READ (1u << 3)
#define ST_PIPEFILE (-5)
#define ST_LINKFILE (-4)
#define ST_FILE (-3)
#define ST_ROOT 1
#define ST_USERDIR 2
#define ST_SOFTLINK 3
#define ST_LINKDIR 4
#define LOCK_SAME 0
#define LOCK_SAME_VOLUME 1
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_OBJECT_WRONG_TYPE 212
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_ACTION_NOT_KNOWN 209

struct rd_test_handle;

BPTR Lock(const char *name, LONG access_mode);
BOOL UnLock(BPTR lock);
BPTR DupLock(BPTR lock);
LONG Examine(BPTR lock, struct FileInfoBlock *fib);
LONG ExNext(BPTR lock, struct FileInfoBlock *fib);
BPTR ParentDir(BPTR lock);
LONG SameLock(BPTR first, BPTR second);
LONG ReadLink(struct MsgPort *port, BPTR parent, const char *path,
    char *buffer, ULONG size);
APTR AllocDosObject(ULONG type, const void *tags);
void FreeDosObject(ULONG type, APTR object);
BPTR CurrentDir(BPTR lock);
LONG IoErr(void);
BPTR OpenFromLock(BPTR lock);
BPTR DupLockFromFH(BPTR file);
BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib);
LONG Seek(BPTR file, LONG position, LONG mode);
LONG Read(BPTR file, void *buffer, LONG length);
BOOL Close(BPTR file);
void *AllocVec(size_t size, ULONG requirements);
void FreeVec(void *memory);

#endif
