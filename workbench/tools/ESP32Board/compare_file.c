/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdio.h>
#include "compare_file.h"
#include "source_file.h"

int boardtool_discover_c6(struct board_client_observation *, char *, size_t);

int boardtool_compare_c6(const char *path, const char *sha256,
    struct board_client_observation *observation,
    struct board_c6_target_report *report, char *status, size_t capacity)
{
    struct boardtool_c6_source source = {0};
    struct board_c6_target_report result;
    int produced = 0;
    if (!observation || !report || !status || !capacity) return 0;
    /* Invalid files never cause a maintenance query. Source remains owned
     * through the query and exact recheck, then is disposed on every exit. */
    if (!boardtool_c6_source_load(path, sha256, &source, status, capacity))
        return 0;
    if (!boardtool_discover_c6(observation, status, capacity)) goto done;
    if (!boardtool_c6_source_recheck(&source) ||
        !board_c6_target_compare(&source.info, &observation->snapshot, &result)) {
        snprintf(status, capacity, "C6 comparison refused: source/metadata changed or invalid; no write");
        goto done;
    }
    *report = result;
    snprintf(status, capacity,
        "C6 comparison INCOMPLETE: rejected=%lx unknown=%lx risk=%lx. "
        "Bits: chip1 revision2 counter4 security8 slot10 block20 policy40 "
        "origin80 MMU100 (hex). Policy/origin/recovery NOT proved; "
        "counter risk is not an enforced floor. WLAN offline; no write.",
        (unsigned long)result.rejected, (unsigned long)result.unknown,
        (unsigned long)result.risk);
    produced = 1;
done:
    boardtool_c6_source_dispose(&source);
    return produced;
}
