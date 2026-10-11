/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#define MUIMASTER_YES_INLINE_STDARG

#include <libraries/mui.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <clib/alib_protos.h>
#include <utility/hooks.h>
#include <stdio.h>
#include <string.h>

#include "layout_gui.h"

#define PARTITION_COLUMNS 6
#define PARTITION_ROW_MAX 112
#define DETAIL_TEXT_MAX 768

/* The display hook returns pointers into this short-lived display cache. MUI
 * consumes the columns synchronously while drawing one row. */
struct Hook boardtool_partition_display_hook;
static char display_columns[PARTITION_COLUMNS][32];

/* One application/window owns this renderer. Labels outlive their MUI
 * objects. The bar includes every unlisted span as a neutral segment, never
 * as verified free space. */
static Object *blocks[BOARD_LAYOUT_MAX_PARTITIONS * 2 + 1];
static char help_text[BOARD_LAYOUT_MAX_PARTITIONS * 2 + 1][320];
static size_t block_count;
static char details[DETAIL_TEXT_MAX];

AROS_UFH3S(LONG, partition_display,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(char **, array, A2),
    AROS_UFHA(STRPTR, entry, A1))
{
    static const char *titles[PARTITION_COLUMNS] =
        { "Idx", "Partition", "Offset", "Size", "Type/Sub", "Role R/B/U" };
    const char *cursor = entry;
    ULONG column;

    AROS_USERFUNC_INIT

    (void)hook;
    if (entry == NULL)
    {
        for (column = 0; column < PARTITION_COLUMNS; ++column)
            array[column] = (STRPTR)titles[column];
        return 0;
    }

    for (column = 0; column < PARTITION_COLUMNS; ++column)
    {
        size_t copied = 0;
        while (*cursor != '\0' && *cursor != '\t')
        {
            if (copied + 1 < sizeof(display_columns[column]))
                display_columns[column][copied++] = *cursor;
            ++cursor;
        }
        display_columns[column][copied] = '\0';
        array[column] = display_columns[column];
        if (*cursor == '\t')
            ++cursor;
    }
    return 0;

    AROS_USERFUNC_EXIT
}

void boardtool_layout_gui_init(void)
{
    boardtool_partition_display_hook.h_Entry = (HOOKFUNC)partition_display;
    boardtool_partition_display_hook.h_SubEntry = NULL;
    boardtool_partition_display_hook.h_Data = NULL;
}

static const char *role_marker(uint8_t reported, size_t index, char role)
{
    if (reported == BOARD_DISCOVERY_UNKNOWN_ROLE)
        return "?";
    return reported == index ? (role == '\0' ? "?" :
        (role == 'R' ? "R" : role == 'B' ? "B" : "U")) : "-";
}

static void clear_bar(Object *group)
{
    while (block_count != 0)
    {
        Object *block = blocks[--block_count];
        DoMethod(group, OM_REMMEMBER, (IPTR)block);
        MUI_DisposeObject(block);
    }
}

static int add_block(Object *group, uint32_t offset, uint32_t bytes,
    uint32_t total, const struct board_partition *partition,
    const struct board_discovery_snapshot *snapshot, size_t role_index)
{
    Object *block;
    ULONG weight;
    size_t index = block_count;

    if (index >= sizeof(blocks) / sizeof(blocks[0]) || total == 0 || bytes == 0)
        return 0;
    weight = (ULONG)(((uint64_t)bytes * 65535u) / total);
    if (weight == 0)
        weight = 1;

    if (partition && snapshot)
        snprintf(help_text[index], sizeof(help_text[index]),
            "%s\nOffset 0x%08lx, size 0x%08lx bytes\n"
            "Registry index %lu; role markers R/B/U are reported indices.\n"
            "Historical registry data, not write authority.",
            partition->label, (unsigned long)offset, (unsigned long)bytes,
            (unsigned long)role_index);
    else if (partition)
        snprintf(help_text[index], sizeof(help_text[index]),
            "%s\nOffset 0x%08lx, size 0x%08lx bytes\n"
            "Imported partition; active/boot/update roles are unknown.",
            partition->label, (unsigned long)offset, (unsigned long)bytes);
    else
        snprintf(help_text[index], sizeof(help_text[index]),
            "Unlisted/reserved span\nOffset 0x%08lx, size 0x%08lx bytes\n"
            "NOT VERIFIED FREE.", (unsigned long)offset,
            (unsigned long)bytes);

    block = RectangleObject,
        MUIA_Weight, weight,
        MUIA_Background, partition ? MUII_FILL : MUII_BACKGROUND,
        MUIA_ShortHelp, (IPTR)help_text[index],
    End;
    if (block == NULL)
        return 0;
    if (!DoMethod(group, OM_ADDMEMBER, (IPTR)block))
    {
        MUI_DisposeObject(block);
        return 0;
    }
    blocks[index] = block;
    ++block_count;
    return 1;
}

static void role_text(char *buffer, size_t capacity,
    const struct board_discovery_snapshot *snapshot, size_t index)
{
    if (snapshot == NULL)
    {
        snprintf(buffer, capacity, "?/?/?");
        return;
    }
    snprintf(buffer, capacity, "%s/%s/%s",
        role_marker(snapshot->running, index, 'R'),
        role_marker(snapshot->boot, index, 'B'),
        role_marker(snapshot->update, index, 'U'));
}

static void role_index_text(char *buffer, size_t capacity, uint8_t index)
{
    if (index == BOARD_DISCOVERY_UNKNOWN_ROLE)
        snprintf(buffer, capacity, "unknown");
    else
        snprintf(buffer, capacity, "%u", (unsigned)index);
}

static const char *target_fact(uint8_t known, uint8_t bit, unsigned value,
    char *buffer, size_t capacity)
{
    if (!(known & bit))
        snprintf(buffer, capacity, "unknown");
    else
        snprintf(buffer, capacity, "%u", value);
    return buffer;
}

static void make_details(const struct board_layout *layout,
    const struct board_discovery_snapshot *snapshot)
{
    if (snapshot == NULL)
    {
        snprintf(details, sizeof(details),
            "Imported table preview; not live or active.\n"
            "Partition roles: unknown (imported).\n"
            "Unlisted/reserved gaps are NOT VERIFIED FREE.\n"
            "Explicit capacity: %lu MiB. No write authority.",
            (unsigned long)(layout->flash_bytes / (1024UL * 1024UL)));
    }
    else
    {
        char running[16], boot[16], update[16];
        char chip[16], revision[16], secure_count[16];

        role_index_text(running, sizeof(running), snapshot->running);
        role_index_text(boot, sizeof(boot), snapshot->boot);
        role_index_text(update, sizeof(update), snapshot->update);
        target_fact(snapshot->target_known, BOARD_DISCOVERY_TARGET_CHIP,
            snapshot->chip_id, chip, sizeof(chip));
        target_fact(snapshot->target_known, BOARD_DISCOVERY_TARGET_REVISION,
            snapshot->chip_revision, revision, sizeof(revision));
        target_fact(snapshot->target_known,
            BOARD_DISCOVERY_TARGET_SECURE_VERSION, snapshot->secure_version,
            secure_count, sizeof(secure_count));
        snprintf(details, sizeof(details),
            "Historical registry inventory, not raw-table attestation.\n"
            "Sorted roles: running=%s boot=%s update=%s; state=%u.\n"
            "Role R/B/U columns index the sorted rows below.\n"
            "Security known=0x%02x enabled=0x%02x; unknown is not disabled.\n"
            "Target known=0x%02x; chip=%s revision=%s.\n"
            "Physical=%lu MiB; usable layout=%lu MiB; secure count=%s.\n"
            "No write authority. WLAN remains offline after query.",
            running, boot, update, (unsigned)snapshot->running_state,
            (unsigned)snapshot->security_known,
            (unsigned)snapshot->security_enabled,
            (unsigned)snapshot->target_known, chip, revision,
            (unsigned long)(snapshot->physical_bytes / (1024UL * 1024UL)),
            (unsigned long)(layout->flash_bytes / (1024UL * 1024UL)),
            secure_count);
    }
}

int boardtool_layout_render(Object *app, Object *bar_group,
    Object *partition_list, Object *details_text,
    const struct board_layout *layout,
    const struct board_discovery_snapshot *snapshot)
{
    size_t i;
    uint32_t offset = 0;
    int ok = 1;
    (void)app;

    if (bar_group == NULL || partition_list == NULL || details_text == NULL ||
        layout == NULL || layout->flash_bytes == 0 || layout->count == 0 ||
        layout->count > BOARD_LAYOUT_MAX_PARTITIONS ||
        (snapshot && (layout != &snapshot->layout ||
                      !board_discovery_validate(snapshot))))
        return 0;

    DoMethod(bar_group, MUIM_Group_InitChange);
    clear_bar(bar_group);
    DoMethod(partition_list, MUIM_List_Clear);

    for (i = 0; i < layout->count && ok; ++i)
    {
        const struct board_partition *part = &layout->partitions[i];
        char row[PARTITION_ROW_MAX];
        char roles[8];

        if (part->offset > offset)
            ok = add_block(bar_group, offset, part->offset - offset,
                layout->flash_bytes, NULL, NULL, 0);
        if (!ok)
            break;
        ok = add_block(bar_group, part->offset, part->size,
            layout->flash_bytes, part, snapshot, i);
        if (!ok)
            break;

        role_text(roles, sizeof(roles), snapshot, i);
        snprintf(row, sizeof(row), "%02lu\t%s\t0x%08lx\t0x%08lx\t%02x/%02x\t%s",
            (unsigned long)i, part->label,
            (unsigned long)part->offset, (unsigned long)part->size,
            (unsigned)part->type, (unsigned)part->subtype, roles);
        if ((LONG)DoMethod(partition_list, MUIM_List_InsertSingle,
                (IPTR)row, MUIV_List_Insert_Bottom) < 0)
            ok = 0;
        offset = part->offset + part->size;
    }

    if (ok && offset < layout->flash_bytes)
        ok = add_block(bar_group, offset, layout->flash_bytes - offset,
            layout->flash_bytes, NULL, NULL, 0);
    DoMethod(bar_group, MUIM_Group_ExitChange);

    if (ok)
    {
        make_details(layout, snapshot);
        SET(details_text, MUIA_Text_Contents, (IPTR)details);
    }
    return ok;
}
