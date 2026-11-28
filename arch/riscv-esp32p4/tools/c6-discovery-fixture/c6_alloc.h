/* Injected only while compiling the real collector, validator and parser. */
#ifndef C6_DISCOVERY_FIXTURE_C6_ALLOC_H
#define C6_DISCOVERY_FIXTURE_C6_ALLOC_H

#include <stddef.h>

void *c6_test_malloc(size_t size);
void *c6_test_calloc(size_t count, size_t size);
void *c6_test_realloc(void *pointer, size_t size);
void c6_test_free(void *pointer);

#endif
