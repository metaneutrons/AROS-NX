#ifndef BOARD_SOURCE_STUB_DOS_DOS_H
#define BOARD_SOURCE_STUB_DOS_DOS_H

#include <stdint.h>

typedef uintptr_t BPTR;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef const char *CONST_STRPTR;

#define BNULL ((BPTR)0)
#define MODE_OLDFILE (-2)
#define DOS_FIB 1u

struct FileInfoBlock {
    LONG fib_DirEntryType;
    LONG fib_Size;
};

BPTR Open(CONST_STRPTR name, LONG mode);
struct FileInfoBlock *AllocDosObject(ULONG type, const void *tags);
void FreeDosObject(ULONG type, void *object);
LONG ExamineFH(BPTR file, struct FileInfoBlock *fib);
LONG Read(BPTR file, void *buffer, LONG length);
LONG Close(BPTR file);

#endif
