/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <string.h>
#include "source_file.h"

int boardtool_check_c6(const char *path, const char *sha256,
                      char *status, size_t capacity)
{
    struct boardtool_c6_source source;
    int passed;
    memset(&source, 0, sizeof(source));
    passed = boardtool_c6_source_load(path, sha256, &source, status, capacity);
    boardtool_c6_source_dispose(&source);
    return passed;
}
