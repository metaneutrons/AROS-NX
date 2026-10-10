/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#define MUIMASTER_YES_INLINE_STDARG
#include <libraries/mui.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <stdio.h>
#include "layout.h"
#include "../../../arch/riscv-esp32p4/boardtool/discovery.h"

/* One application/window owns this renderer. Labels outlive their MUI objects.
 * The horizontal graph includes unlisted/reserved spans, NOT assumed free
 * space. Tiny extents receive a minimum visual weight; exact values are in
 * tooltips. Imported data never assigns active or writable roles. */
static Object *blocks[BOARD_LAYOUT_MAX_PARTITIONS * 2 + 1];
static char labels[BOARD_LAYOUT_MAX_PARTITIONS * 2 + 1][320];
static char names[BOARD_LAYOUT_MAX_PARTITIONS * 2 + 1][32];
static size_t block_count;
static int add_block(Object *group, uint32_t offset, uint32_t bytes,
                     uint32_t total, const struct board_partition *partition,
                     const struct board_discovery_snapshot *snapshot, size_t role_index)
{
    Object *block;
    ULONG weight = (ULONG)(((uint64_t)bytes * 65535u) / total);
    size_t index = block_count;
    if (index >= sizeof(blocks) / sizeof(blocks[0])) return 0;
    if (!weight) weight = 1;
    snprintf(names[index], sizeof(names[index]), "%s", partition ? partition->label : "-");
    if (partition && snapshot)
        snprintf(names[index], sizeof(names[index]), "%s%s%s%s", partition->label,
            snapshot->running == role_index ? " [R]" : "",
            snapshot->boot == role_index ? " [B]" : "",
            snapshot->update == role_index ? " [U]" : "");
    if (partition && snapshot)
        snprintf(labels[index], sizeof(labels[index]),
            "%s\n0x%08lx +0x%08lx\nType %02x/%02x normalized flags %08lx\n"
            "Historical index %lu: running=%s boot=%s update=%s state=%u\n"
            "Security known=%02x enabled=%02x; unknown != disabled\nNo write authority",
            partition->label, (unsigned long)offset, (unsigned long)bytes,
            partition->type, partition->subtype, (unsigned long)partition->flags,
            (unsigned long)role_index,
            snapshot->running == role_index ? "yes" : "no/unknown",
            snapshot->boot == role_index ? "yes" : "no/unknown",
            snapshot->update == role_index ? "yes" : "no/unknown",
            snapshot->running_state,
            snapshot->security_known, snapshot->security_enabled);
    else if (partition)
        snprintf(labels[index], sizeof(labels[index]),
            "%s\n0x%08lx +0x%08lx\nType %02x/%02x flags %08lx\nImported: active/update role unknown",
            partition->label, (unsigned long)offset, (unsigned long)bytes,
            partition->type, partition->subtype, (unsigned long)partition->flags);
    else
        snprintf(labels[index], sizeof(labels[index]),
            "Unlisted/reserved\n0x%08lx +0x%08lx\nNot verified free space",
            (unsigned long)offset, (unsigned long)bytes);
    block = TextObject,
        MUIA_Weight, weight,
        MUIA_Frame, MUIV_Frame_Text,
        MUIA_Background, partition ? MUII_FILL : MUII_BACKGROUND,
        MUIA_Text_Contents, (IPTR)names[index],
        MUIA_ShortHelp, (IPTR)labels[index],
        MUIA_Text_SetMin, FALSE,
    End;
    if (!block) return 0;
    blocks[index] = block;
    ++block_count;
    DoMethod(group, OM_ADDMEMBER, (IPTR)block);
    return 1;
}
int boardtool_layout_render(Object *app, Object *group, const struct board_layout *layout,
    const struct board_discovery_snapshot *snapshot)
{
    size_t i;
    uint32_t offset = 0;
    int ok = 1;
    (void)app;
    if (!group || !layout || !layout->flash_bytes ||
        !layout->count || layout->count > BOARD_LAYOUT_MAX_PARTITIONS ||
        (snapshot && (layout != &snapshot->layout ||
                      !board_discovery_validate(snapshot)))) return 0;
    DoMethod(group, MUIM_Group_InitChange);
    while (block_count) {
        Object *block = blocks[--block_count];
        DoMethod(group, OM_REMMEMBER, (IPTR)block);
        MUI_DisposeObject(block);
    }
    for (i = 0; i < layout->count && ok; ++i) {
        const struct board_partition *part = &layout->partitions[i];
        if (part->offset > offset)
            ok = add_block(group, offset, part->offset - offset, layout->flash_bytes, NULL, NULL, 0);
        if (ok) ok = add_block(group, part->offset, part->size, layout->flash_bytes, part, snapshot, i);
        offset = part->offset + part->size;
    }
    if (ok && offset < layout->flash_bytes)
        ok = add_block(group, offset, layout->flash_bytes - offset, layout->flash_bytes, NULL, NULL, 0);
    DoMethod(group, MUIM_Group_ExitChange);
    return ok;
}
