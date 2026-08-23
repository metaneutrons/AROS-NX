/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The JD9365 initialisation sequence, kept under its own licence.
 *
 * This file is deliberately not AROS-licensed and deliberately contains
 * nothing else.  The command values below are Espressif's, taken from
 * `esp_lcd_jd9365_8` in the Seeed reTerminal D1001 support package, and they
 * are panel-vendor data rather than an implementation: changing them would
 * mean asking the panel supplier, not reasoning about them.  Keeping them in
 * one file with the original notice means the port carries the attribution
 * the licence asks for and nothing has to be argued about when the rest of
 * the display driver is written.
 *
 * Provenance and the licence decision are recorded in
 * display/DISPLAY-CONTRACT.md.
 *
 * Two things about the sequence are worth knowing before reading it.  The
 * four commands 0xE1 0x93, 0xE2 0x65, 0xE3 0xF8 and 0x80 0x01 are the
 * controller's page-unlock magic, without which the registers around them are
 * not writable.  And 0x29 is display-on: it is the last command and it is safe
 * here only because this port keeps the backlight dark, so the panel is driven
 * but nothing is lit.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "kernel_intern.h"

const struct P4JD9365Cmd krnP4JD9365Init[] =
{
    { 0x11, 0x00, 1, 500 },     /* sleep out, and it needs all 500 ms */
    { 0xE0, 0x00, 1, 0 },
    { 0xE1, 0x93, 1, 0 },
    { 0xE2, 0x65, 1, 0 },
    { 0xE3, 0xF8, 1, 0 },
    { 0x80, 0x01, 1, 0 },
    { 0xE0, 0x00, 1, 0 },
    { 0x29, 0x00, 1, 50 },      /* display on, with the backlight still off */
};

const unsigned int krnP4JD9365InitCount =
    sizeof(krnP4JD9365Init) / sizeof(krnP4JD9365Init[0]);
