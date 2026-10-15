#!/usr/bin/env python3
"""Host-only layout and bounded-status checks for ESP32Board's MUI front end."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "main.c").read_text()
LAYOUT = (ROOT / "layout_gui.c").read_text()
HEADER = (ROOT / "boardtool.h").read_text()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    in_string = False
    in_char = False
    escaped = False
    for index in range(opening, len(source)):
        char = source[index]
        if escaped:
            escaped = False
            continue
        if (in_string or in_char) and char == "\\":
            escaped = True
            continue
        if not in_char and char == '"':
            in_string = not in_string
            continue
        if not in_string and char == "'":
            in_char = not in_char
            continue
        if in_string or in_char:
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise SystemExit("FAIL: could not extract set_status body")


def test_layout_contracts() -> None:
    require('"Overview", "Update", "Flash & Backup", "Diagnostics", NULL' in MAIN,
            "four requested Register pages are present")
    require("MUIA_Window_Width, 740" in MAIN,
            "window inner width stays compact on wider panels")
    require("MUIA_Window_Height, 500" in MAIN,
            "window inner height leaves desktop/window chrome in600 pixels")
    for page in (1, 2, 3):
        require(f"MUIA_Group_ActivePage, {page}" in MAIN,
                f"Overview navigation reaches Register page {page}")
    require("MUIA_String_MaxLen, BOARDTOOL_PATH_MAX - 1" in MAIN,
            "native file paths keep the 511-character UI limit")
    require("#define BOARDTOOL_PATH_MAX   512" in HEADER,
            "shared CLI path capacity remains 512 bytes")
    require("MUIA_String_MaxLen, BOARDTOOL_SHA256_LEN" in MAIN,
            "trusted SHA inputs retain the 64-character limit")
    require("sha_string, MUIA_String_Contents" in MAIN,
            "C6 Check/Compare use their own SHA field")
    require("table_sha_string,\n                                             MUIA_String_Contents" in MAIN,
            "imported-table preview uses its visible table SHA field")
    for field in ("sha_string", "table_sha_string"):
        require(f"SET({field}, MUIA_Disabled, TRUE)" in MAIN and
                f"SET({field}, MUIA_Disabled, FALSE)" in MAIN,
                f"{field} is disabled/restored around worker use")
    require("SET(update_button, MUIA_Disabled, !update->available)" in MAIN,
            "UPDATE-C6 button remains governed by the unavailable registry entry")
    require('"UPDATE-C6", "Update C6 (unavailable)",       FALSE' in HEADER,
            "UPDATE-C6 remains unavailable in the shared operation registry")
    require("Refresh releases (unavailable)" in MAIN and
            "Check package (unavailable)" in MAIN and
            "Install (unavailable)" in MAIN and
            "no listing or download available" in MAIN,
            "release, package, and install functions are explicit non-features")
    for control in ("release_refresh_button", "package_check_button",
                    "backup_button", "write_button", "raw_compare_button",
                    "expert_check"):
        require(f"SET({control}, MUIA_Disabled, TRUE)" in MAIN,
                f"{control} is disabled")
    require("DoMethod(update_button," not in MAIN,
            "the unavailable update button has no operation notification")
    require("Preview imported table" in MAIN and "Check C6 file" in MAIN and
            "Compare C6 (WLAN offline)" in MAIN,
            "existing safe C6 and imported-layout workflows remain exposed")
    require("MUIA_List_DisplayHook" in MAIN and
            '"Idx", "Partition", "Offset", "Size", "Type/Sub", "Role R/B/U"' in LAYOUT,
            "partition rows present readable indexed columns")
    require('"BAR,BAR,BAR,BAR,BAR,COL=5"' in MAIN,
            "six List format columns match the six display-hook strings")
    require("if (!DoMethod(group, OM_ADDMEMBER, (IPTR)block))" in LAYOUT,
            "failed bar attachment cannot publish a successful block")
    require("NOT VERIFIED FREE" in LAYOUT and "?/?/?" in LAYOUT and
            "active/boot/update roles are unknown" in LAYOUT,
            "gaps are not labelled free and imported roles remain unknown")
    require("ASL_FileRequest" in MAIN and "MUII_PopFile" in MAIN,
            "file fields use native ASL requesters")
    require("WLAN stops and leaves it offline" in MAIN or
            "WLAN stops and remains offline" in MAIN,
            "diagnostics disclose the offline WLAN effect")
    require("MUIA_Scrollgroup_FreeVert, TRUE" in MAIN,
            "diagnostic details can scroll vertically")
    require("STATUS_WRAP_COLUMN 68" in MAIN,
            "status width uses a bounded line length")


def test_status_with_sanitizers() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    require(compiler is not None,
            "a host C compiler is available for wrapper tests")
    function = extract_function(MAIN, "static void set_status(CONST_STRPTR text)")
    harness = r'''
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BOARDTOOL_STATUS_MAX 384
#define STATUS_WRAP_COLUMN 68
#define MUIA_Text_Contents 1
typedef const char *CONST_STRPTR;
typedef unsigned long IPTR;
struct fake_object { const char *text; };
static struct fake_object *status_text;
static char status_buffer[BOARDTOOL_STATUS_MAX * 2];
#define SET(object, attribute, value) ((object)->text = (const char *)(value))
'''
    harness += function
    harness += r'''
static int check_lines(const char *text)
{
    size_t line = 0;
    while (*text != '\0')
    {
        if (*text == '\n')
        {
            if (line > STATUS_WRAP_COLUMN) return 0;
            line = 0;
        }
        else
        {
            ++line;
            if (line > STATUS_WRAP_COLUMN) return 0;
        }
        ++text;
    }
    return line <= STATUS_WRAP_COLUMN;
}

static int check_count(const char *text, char expected, size_t want)
{
    size_t count = 0;
    while (*text != '\0')
        if (*text++ == expected) ++count;
    return count == want;
}

int main(void)
{
    struct fake_object object = { NULL };
    char max_status[BOARDTOOL_STATUS_MAX];
    char very_long[4096];
    size_t i;

    status_text = &object;
    set_status(NULL);
    if (object.text == NULL || object.text[0] != '\0') return 1;
    set_status("first line\nsecond line");
    if (strstr(object.text, "first line\nsecond line") == NULL ||
        !check_lines(object.text)) return 2;

    memset(max_status, 'x', sizeof(max_status) - 1);
    max_status[sizeof(max_status) - 1] = '\0';
    set_status(max_status);
    if (!check_lines(object.text) ||
        !check_count(object.text, 'x', sizeof(max_status) - 1)) return 3;

    memset(very_long, 'y', sizeof(very_long) - 1);
    very_long[sizeof(very_long) - 1] = '\0';
    set_status(very_long);
    if (strlen(object.text) >= sizeof(status_buffer) ||
        !check_lines(object.text)) return 4;
    for (i = 0; i < strlen(object.text); ++i)
        if (object.text[i] != 'y' && object.text[i] != '\n') return 5;

    status_text = NULL;
    set_status(very_long);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="esp32board-status-") as temp:
        source = Path(temp) / "status_test.c"
        source.write_text(harness)
        for optimization in ("-O0", "-O2"):
            executable = Path(temp) / f"status-{optimization[2:]}"
            command = shlex.split(compiler) + [
                optimization,
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source),
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(command, capture_output=True, text=True)
            require(compiled.returncode == 0,
                    f"status harness compiles with {optimization}: {compiled.stderr}")
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            ran = subprocess.run([str(executable)], capture_output=True, text=True,
                                 env=env)
            require(ran.returncode == 0,
                    f"status harness passes with {optimization}: {ran.stderr}")


if __name__ == "__main__":
    test_layout_contracts()
    test_status_with_sanitizers()
    print("PASS: ESP32Board GUI source contracts and ASan/UBSan status wrapping")

