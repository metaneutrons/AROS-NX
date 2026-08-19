/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ESP32-P4 Pixel Processing Accelerator (PPA) 2D GPU Blitter Header
          for AROS Graphics OOP HIDD Subsystem.
*/

#ifndef RISCV32_PPA_P4_H
#define RISCV32_PPA_P4_H

#include "types.h"

/* ESP32-P4 PPA Register Base */
#define ESP32P4_PPA_BASE                0x500C5000UL

/* Scaling, Rotating & Mirroring Engine (SRM) Registers */
#define PPA_SRM_CTRL                    0x0000
#define PPA_SRM_SRC_ADDR                0x0004
#define PPA_SRM_DST_ADDR                0x0008
#define PPA_SRM_SRC_SIZE                0x000C
#define PPA_SRM_DST_SIZE                0x0010
#define PPA_SRM_TRANS_CONF              0x0014

/* Alpha Blending Engine Registers */
#define PPA_BLEND_CTRL                  0x0040
#define PPA_BLEND_SRC_A_ADDR            0x0044
#define PPA_BLEND_SRC_B_ADDR            0x0048
#define PPA_BLEND_DST_ADDR              0x004C
#define PPA_BLEND_SIZE                  0x0050

/* Color Formats */
#define PPA_FMT_ARGB8888                0
#define PPA_FMT_RGB888                  1
#define PPA_FMT_RGB565                  2
#define PPA_FMT_A8                      3

/* Rotations */
#define PPA_ROT_0                       0
#define PPA_ROT_90                      1
#define PPA_ROT_180                     2
#define PPA_ROT_270                     3

/* Driver Interface */
int  ppa_p4_init(void);
int  ppa_p4_blit(const void *src_buf, ULONG src_pitch,
                 void *dst_buf, ULONG dst_pitch,
                 UWORD width, UWORD height, UBYTE color_fmt);
int  ppa_p4_fill_rect(void *dst_buf, ULONG dst_pitch,
                      UWORD x, UWORD y, UWORD w, UWORD h,
                      ULONG color_val, UBYTE color_fmt);
int  ppa_p4_rotate(const void *src_buf, UWORD src_w, UWORD src_h,
                   void *dst_buf, UBYTE rotation_mode, UBYTE color_fmt);

#endif /* RISCV32_PPA_P4_H */
