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
 * not writable.  And 0x29 is display-on, near the end, safe here only because
 * this port keeps the backlight dark, so the panel is driven but nothing is
 * lit.
 *
 * This is now the whole table - 174 commands.  It used to be eight, which was
 * a mistake with a specific consequence: those eight are the page-unlock magic
 * and display-on, and everything between them is the actual configuration -
 * gamma curves, power settings, panel timing, register pages 1 through 4.  A
 * JD9365 given only the unlock and the display-on has been told to show a
 * picture without being told how, and it stops answering reads after it, which
 * is exactly what this port measured for four sessions.  The eight commands
 * were taken from a driver's abbreviated example rather than from its actual
 * initialisation path.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "kernel_intern.h"

const struct P4JD9365Cmd krnP4JD9365Init[] =
{
    /* The wrapper has already selected page zero once.  The vendor table
       selects it once more here; that makes two effective transactions, just
       like the running Vellum driver, not two entries in this array. */
    { 0xE0, 0x00, 1, 0 },
    { 0xE1, 0x93, 1, 0 },
    { 0xE2, 0x65, 1, 0 },
    { 0xE3, 0xF8, 1, 0 },
    { 0x80, 0x01, 1, 0 },    /* 0X03：4-LANE;0X02：3-LANE;0X01:2-LANE */
    { 0xE0, 0x01, 1, 0 },
    { 0x00, 0x00, 1, 0 },
    { 0x01, 0x4E, 1, 0 },
    { 0x03, 0x00, 1, 0 },
    { 0x04, 0x65, 1, 0 },
    { 0x0C, 0x74, 1, 0 },
    { 0x17, 0x00, 1, 0 },
    { 0x18, 0xB7, 1, 0 },
    { 0x19, 0x00, 1, 0 },
    { 0x1A, 0x00, 1, 0 },
    { 0x1B, 0xB7, 1, 0 },
    { 0x1C, 0x00, 1, 0 },
    { 0x24, 0xFE, 1, 0 },
    { 0x37, 0x19, 1, 0 },
    { 0x38, 0x05, 1, 0 },
    { 0x39, 0x00, 1, 0 },
    { 0x3A, 0x01, 1, 0 },
    { 0x3B, 0x01, 1, 0 },
    { 0x3C, 0x70, 1, 0 },
    { 0x3D, 0xFF, 1, 0 },
    { 0x3E, 0xFF, 1, 0 },
    { 0x3F, 0xFF, 1, 0 },
    { 0x40, 0x06, 1, 0 },
    { 0x41, 0xA0, 1, 0 },
    { 0x43, 0x1E, 1, 0 },
    { 0x44, 0x0F, 1, 0 },
    { 0x45, 0x28, 1, 0 },
    { 0x4B, 0x04, 1, 0 },
    /* The reference source contains a commented-out 0x4A/0x35 BIST write in
       this position.  It is deliberately not part of the active sequence. */
    { 0x55, 0x02, 1, 0 },
    { 0x56, 0x01, 1, 0 },
    { 0x57, 0xA9, 1, 0 },
    { 0x58, 0x0A, 1, 0 },
    { 0x59, 0x0A, 1, 0 },
    { 0x5A, 0x37, 1, 0 },
    { 0x5B, 0x19, 1, 0 },
    { 0x5D, 0x78, 1, 0 },
    { 0x5E, 0x63, 1, 0 },
    { 0x5F, 0x54, 1, 0 },
    { 0x60, 0x49, 1, 0 },
    { 0x61, 0x45, 1, 0 },
    { 0x62, 0x38, 1, 0 },
    { 0x63, 0x3D, 1, 0 },
    { 0x64, 0x28, 1, 0 },
    { 0x65, 0x43, 1, 0 },
    { 0x66, 0x41, 1, 0 },
    { 0x67, 0x43, 1, 0 },
    { 0x68, 0x62, 1, 0 },
    { 0x69, 0x50, 1, 0 },
    { 0x6A, 0x57, 1, 0 },
    { 0x6B, 0x49, 1, 0 },
    { 0x6C, 0x44, 1, 0 },
    { 0x6D, 0x37, 1, 0 },
    { 0x6E, 0x23, 1, 0 },
    { 0x6F, 0x10, 1, 0 },
    { 0x70, 0x78, 1, 0 },
    { 0x71, 0x63, 1, 0 },
    { 0x72, 0x54, 1, 0 },
    { 0x73, 0x49, 1, 0 },
    { 0x74, 0x45, 1, 0 },
    { 0x75, 0x38, 1, 0 },
    { 0x76, 0x3D, 1, 0 },
    { 0x77, 0x28, 1, 0 },
    { 0x78, 0x43, 1, 0 },
    { 0x79, 0x41, 1, 0 },
    { 0x7A, 0x43, 1, 0 },
    { 0x7B, 0x62, 1, 0 },
    { 0x7C, 0x50, 1, 0 },
    { 0x7D, 0x57, 1, 0 },
    { 0x7E, 0x49, 1, 0 },
    { 0x7F, 0x44, 1, 0 },
    { 0x80, 0x37, 1, 0 },
    { 0x81, 0x23, 1, 0 },
    { 0x82, 0x10, 1, 0 },
    { 0xE0, 0x02, 1, 0 },
    { 0x00, 0x47, 1, 0 },
    { 0x01, 0x47, 1, 0 },
    { 0x02, 0x45, 1, 0 },
    { 0x03, 0x45, 1, 0 },
    { 0x04, 0x4B, 1, 0 },
    { 0x05, 0x4B, 1, 0 },
    { 0x06, 0x49, 1, 0 },
    { 0x07, 0x49, 1, 0 },
    { 0x08, 0x41, 1, 0 },
    { 0x09, 0x1F, 1, 0 },
    { 0x0A, 0x1F, 1, 0 },
    { 0x0B, 0x1F, 1, 0 },
    { 0x0C, 0x1F, 1, 0 },
    { 0x0D, 0x1F, 1, 0 },
    { 0x0E, 0x1F, 1, 0 },
    { 0x0F, 0x5F, 1, 0 },
    { 0x10, 0x5F, 1, 0 },
    { 0x11, 0x57, 1, 0 },
    { 0x12, 0x77, 1, 0 },
    { 0x13, 0x35, 1, 0 },
    { 0x14, 0x1F, 1, 0 },
    { 0x15, 0x1F, 1, 0 },
    { 0x16, 0x46, 1, 0 },
    { 0x17, 0x46, 1, 0 },
    { 0x18, 0x44, 1, 0 },
    { 0x19, 0x44, 1, 0 },
    { 0x1A, 0x4A, 1, 0 },
    { 0x1B, 0x4A, 1, 0 },
    { 0x1C, 0x48, 1, 0 },
    { 0x1D, 0x48, 1, 0 },
    { 0x1E, 0x40, 1, 0 },
    { 0x1F, 0x1F, 1, 0 },
    { 0x20, 0x1F, 1, 0 },
    { 0x21, 0x1F, 1, 0 },
    { 0x22, 0x1F, 1, 0 },
    { 0x23, 0x1F, 1, 0 },
    { 0x24, 0x1F, 1, 0 },
    { 0x25, 0x5F, 1, 0 },
    { 0x26, 0x5F, 1, 0 },
    { 0x27, 0x57, 1, 0 },
    { 0x28, 0x77, 1, 0 },
    { 0x29, 0x35, 1, 0 },
    { 0x2A, 0x1F, 1, 0 },
    { 0x2B, 0x1F, 1, 0 },
    { 0x58, 0x40, 1, 0 },
    { 0x59, 0x00, 1, 0 },
    { 0x5A, 0x00, 1, 0 },
    { 0x5B, 0x10, 1, 0 },
    { 0x5C, 0x06, 1, 0 },
    { 0x5D, 0x40, 1, 0 },
    { 0x5E, 0x01, 1, 0 },
    { 0x5F, 0x02, 1, 0 },
    { 0x60, 0x30, 1, 0 },
    { 0x61, 0x01, 1, 0 },
    { 0x62, 0x02, 1, 0 },
    { 0x63, 0x03, 1, 0 },
    { 0x64, 0x6B, 1, 0 },
    { 0x65, 0x05, 1, 0 },
    { 0x66, 0x0C, 1, 0 },
    { 0x67, 0x73, 1, 0 },
    { 0x68, 0x09, 1, 0 },
    { 0x69, 0x03, 1, 0 },
    { 0x6A, 0x56, 1, 0 },
    { 0x6B, 0x08, 1, 0 },
    { 0x6C, 0x00, 1, 0 },
    { 0x6D, 0x04, 1, 0 },
    { 0x6E, 0x04, 1, 0 },
    { 0x6F, 0x88, 1, 0 },
    { 0x70, 0x00, 1, 0 },
    { 0x71, 0x00, 1, 0 },
    { 0x72, 0x06, 1, 0 },
    { 0x73, 0x7B, 1, 0 },
    { 0x74, 0x00, 1, 0 },
    { 0x75, 0xF8, 1, 0 },
    { 0x76, 0x00, 1, 0 },
    { 0x77, 0xD5, 1, 0 },
    { 0x78, 0x2E, 1, 0 },
    { 0x79, 0x12, 1, 0 },
    { 0x7A, 0x03, 1, 0 },
    { 0x7B, 0x00, 1, 0 },
    { 0x7C, 0x00, 1, 0 },
    { 0x7D, 0x03, 1, 0 },
    { 0x7E, 0x7B, 1, 0 },
    { 0xE0, 0x04, 1, 0 },
    { 0x00, 0x0E, 1, 0 },
    { 0x02, 0xB3, 1, 0 },
    { 0x09, 0x60, 1, 0 },
    { 0x0E, 0x2A, 1, 0 },
    { 0x36, 0x59, 1, 0 },
    { 0x37, 0x58, 1, 0 },    /* A133 */
    { 0x2B, 0x0F, 1, 0 },    /* A133 */
    { 0xE0, 0x00, 1, 0 },
    /*
     * Sleep-out and display-on, with a parameter byte they do not take.
     *
     * That looks wrong and is what the vendor driver sends: its table has
     * {0x11, {0x00}, 1, 120} and {0x29, {0x00}, 1, 20}, and it reaches the
     * panel through the same DCS short-write-1 path.  Sending them without the
     * parameter was tried here and changed nothing observable, so the panel
     * accepts either; the vendor's form is kept because it is the one known to
     * work on this hardware.  DCS 0x0A afterwards still reports the display
     * off, which is a separate matter - the panel enables its output when a
     * valid video stream arrives.
     */
    { 0x11, 0x00, 1, 120 },
    { 0x29, 0x00, 1, 20 },
    { 0x35, 0x00, 1, 0 },
};

const unsigned int krnP4JD9365InitCount =
    sizeof(krnP4JD9365Init) / sizeof(krnP4JD9365Init[0]);
