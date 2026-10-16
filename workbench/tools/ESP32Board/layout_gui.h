/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_LAYOUT_GUI_H
#define ESP32BOARD_LAYOUT_GUI_H

#include <libraries/mui.h>
#include <utility/hooks.h>

#include "layout.h"
#include "../../../arch/riscv-esp32p4/boardtool/discovery.h"

extern struct Hook boardtool_partition_display_hook;

void boardtool_layout_gui_init(void);
int boardtool_layout_render(Object *app, Object *bar_group,
    Object *partition_list, Object *details_text,
    const struct board_layout *layout,
    const struct board_discovery_snapshot *snapshot);

#endif
