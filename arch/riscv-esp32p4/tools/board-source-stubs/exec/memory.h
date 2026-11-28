#ifndef BOARD_SOURCE_STUB_EXEC_MEMORY_H
#define BOARD_SOURCE_STUB_EXEC_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define MEMF_PUBLIC 1u

void *AllocVec(size_t size, uint32_t requirements);
void FreeVec(void *memory);

#endif
