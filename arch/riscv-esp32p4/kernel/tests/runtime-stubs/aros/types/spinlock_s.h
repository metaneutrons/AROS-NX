/* Host runtime fixture only; public spinlock ABI is tested separately. */
#ifndef P4_RUNTIME_TEST_SPINLOCK_H
#define P4_RUNTIME_TEST_SPINLOCK_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef struct { uint32_t lock; void *owner; } spinlock_t;
#endif
