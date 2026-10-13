#!/usr/bin/env python3
"""Host regression for the production bootstrap command-line routing.

Compile the unchanged ParseCmdLine function body with a deterministic VESA
stub. This checks success/failure routing, not BIOS or target ABI correctness.
Run with an optional bootstrap.c path to demonstrate the pre-fix failure.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4] / "arch/all-pc/bootstrap/bootstrap.c"
text = source.read_text()
start = text.index("int ParseCmdLine(const char *cmdline)")
end = text.index("\nstruct mb_mmap *mmap_make", start)
function = text[start:end]
fixture = r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define KRN_CmdLine 1
#define KERNEL_OFFSET 0
struct TagItem { uintptr_t ti_Tag; uintptr_t ti_Data; };
static struct TagItem tags[2], *tag;
static int mode_success, calls;
static int setupVESA(char *mode) { assert(strncmp(mode, "800x600x32", 10) == 0); calls++; return mode_success; }
#define kprintf(...) ((void)0)
''' + function + r'''
static void reset(int success) { memset(tags, 0, sizeof(tags)); tag = tags; calls = 0; mode_success = success; }
int main(void) {
    reset(0); assert(ParseCmdLine(NULL) == 1 && tag == tags && calls == 0);
    reset(0); assert(ParseCmdLine(" debug=serial") == 1 && tag == tags + 1 && calls == 0);
    reset(0); assert(ParseCmdLine(" novesa debug=serial") == 0 && calls == 0);
    reset(1); assert(ParseCmdLine(" vesa=800x600x32 debug=serial") == 0 && calls == 1 && tag == tags + 1);
    reset(0); assert(ParseCmdLine(" vesa=800x600x32 debug=serial") == 1 && calls == 1 && tag == tags + 1);
    puts("bootstrap command-line routing: 5 cases passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aros-cmdline-regression-") as directory:
    root = Path(directory)
    c = root / "test.c"
    exe = root / "test"
    c.write_text(fixture)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)], check=False)
    sys.exit(result.returncode)
