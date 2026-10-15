#!/usr/bin/env python3
"""Host regression tests for worker.c's extracted DHCP lease reader.

Compile the production ReadLeasePrefs() body against mock stdio, environment,
and allocation functions at -O0 and -O2 with AddressSanitizer and UBSan.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


WORKBENCH = Path(__file__).resolve().parents[3]
WIFI = WORKBENCH / "prefs" / "wifi"


HARNESS_PREFIX = r'''#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef int BOOL;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef unsigned char UBYTE;
typedef void *APTR;
typedef char *STRPTR;

#define TRUE 1
#define FALSE 0
#define MEMF_ANY 1UL
#define MEMF_CLEAR 2UL

static unsigned checks;
static unsigned fopen_calls, successful_open_calls, fclose_calls;
static unsigned fseek_end_calls, fseek_set_calls, ftell_calls;
static unsigned fread_calls, ferror_calls, alloc_attempts, alloc_calls;
static unsigned free_calls, clear_calls, bad_free_calls, write_calls;
static const char *environment_path;
static char opened_path[256];
static char opened_mode[16];
static BOOL fopen_fails, fseek_end_fails, fseek_set_fails;
static BOOL ferror_result, fclose_fails, alloc_fails;
static long ftell_result, fread_result_override;
static size_t fread_requested;
static UBYTE fixture[64];
static size_t fixture_length;

static void check_at(int condition, const char *description, int line)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "FAIL at line %d: %s\n", line, description);
        exit(1);
    }
}

#define CHECK(x) check_at((x), #x, __LINE__)

typedef struct LeaseFile
{
    BOOL open;
} LeaseFile;

static LeaseFile mock_file;

static const char *MockGetenv(const char *name);
static LeaseFile *MockFopen(const char *path, const char *mode);
static int MockFseek(LeaseFile *file, long offset, int whence);
static long MockFtell(LeaseFile *file);
static size_t MockFread(void *buffer, size_t size, size_t count,
                        LeaseFile *file);
static int MockFerror(LeaseFile *file);
static int MockFclose(LeaseFile *file);
static size_t MockFwrite(const void *buffer, size_t size, size_t count,
                         LeaseFile *file);
static int MockFputc(int character, LeaseFile *file);
static int MockFputs(const char *text, LeaseFile *file);
static int MockFprintf(LeaseFile *file, const char *format, ...);

#define FILE LeaseFile
#define getenv MockGetenv
#define fopen MockFopen
#define fseek MockFseek
#define ftell MockFtell
#define fread MockFread
#define ferror MockFerror
#define fclose MockFclose
#define fwrite MockFwrite
#define fputc MockFputc
#define fputs MockFputs
#define fprintf MockFprintf

static const char *MockGetenv(const char *name)
{
    CHECK(strcmp(name, "PATH_DHCLIENT_DB") == 0);
    return environment_path;
}

static LeaseFile *MockFopen(const char *path, const char *mode)
{
    ++fopen_calls;
    snprintf(opened_path, sizeof(opened_path), "%s", path);
    snprintf(opened_mode, sizeof(opened_mode), "%s", mode);
    CHECK(strcmp(mode, "rb") == 0);
    if (fopen_fails)
        return NULL;
    mock_file.open = TRUE;
    ++successful_open_calls;
    return &mock_file;
}

static int MockFseek(LeaseFile *file, long offset, int whence)
{
    CHECK(file == &mock_file && file->open);
    CHECK(offset == 0);
    if (whence == SEEK_END)
    {
        ++fseek_end_calls;
        return fseek_end_fails ? -1 : 0;
    }
    if (whence == SEEK_SET)
    {
        ++fseek_set_calls;
        return fseek_set_fails ? -1 : 0;
    }
    CHECK(FALSE);
    return -1;
}

static long MockFtell(LeaseFile *file)
{
    CHECK(file == &mock_file && file->open);
    ++ftell_calls;
    return ftell_result;
}

static size_t MockFread(void *buffer, size_t size, size_t count,
                        LeaseFile *file)
{
    size_t amount;

    CHECK(file == &mock_file && file->open);
    CHECK(size == 1);
    ++fread_calls;
    fread_requested = count;
    amount = count < fixture_length ? count : fixture_length;
    if (fread_result_override >= 0 &&
        (size_t)fread_result_override < amount)
        amount = (size_t)fread_result_override;
    if (amount != 0)
        memcpy(buffer, fixture, amount);
    return amount;
}

static int MockFerror(LeaseFile *file)
{
    CHECK(file == &mock_file && file->open);
    ++ferror_calls;
    return ferror_result;
}

static int MockFclose(LeaseFile *file)
{
    CHECK(file == &mock_file && file->open);
    file->open = FALSE;
    ++fclose_calls;
    return fclose_fails ? EOF : 0;
}

static size_t MockFwrite(const void *buffer, size_t size, size_t count,
                         LeaseFile *file)
{
    (void)buffer;
    (void)size;
    (void)count;
    (void)file;
    ++write_calls;
    return 0;
}

static int MockFputc(int character, LeaseFile *file)
{
    (void)character;
    (void)file;
    ++write_calls;
    return EOF;
}

static int MockFputs(const char *text, LeaseFile *file)
{
    (void)text;
    (void)file;
    ++write_calls;
    return EOF;
}

static int MockFprintf(LeaseFile *file, const char *format, ...)
{
    (void)file;
    (void)format;
    ++write_calls;
    return -1;
}

struct Allocation
{
    APTR pointer;
    size_t length;
};

static struct Allocation allocation;

static void reset_mocks(void)
{
    CHECK(!mock_file.open);
    CHECK(allocation.pointer == NULL);
    memset(&mock_file, 0, sizeof(mock_file));
    memset(&allocation, 0, sizeof(allocation));
    memset(opened_path, 0, sizeof(opened_path));
    memset(opened_mode, 0, sizeof(opened_mode));
    memset(fixture, 0, sizeof(fixture));
    fopen_calls = successful_open_calls = fclose_calls = 0;
    fseek_end_calls = fseek_set_calls = ftell_calls = 0;
    fread_calls = ferror_calls = alloc_attempts = alloc_calls = 0;
    free_calls = clear_calls = bad_free_calls = write_calls = 0;
    environment_path = NULL;
    fopen_fails = fseek_end_fails = fseek_set_fails = FALSE;
    ferror_result = fclose_fails = alloc_fails = FALSE;
    ftell_result = 0;
    fread_result_override = -1;
    fread_requested = 0;
    fixture_length = 0;
}

static void set_fixture(const UBYTE *data, size_t length)
{
    CHECK(length <= sizeof(fixture));
    fixture_length = length;
    if (length != 0)
        memcpy(fixture, data, length);
    ftell_result = (long)length;
}

static APTR AllocVec(ULONG size, ULONG flags)
{
    size_t length = size == 0 ? 1 : (size_t)size;
    APTR pointer;

    ++alloc_attempts;
    if (alloc_fails)
        return NULL;
    CHECK(allocation.pointer == NULL);
    pointer = malloc(length);
    if (pointer == NULL)
        return NULL;
    if ((flags & MEMF_CLEAR) != 0)
        memset(pointer, 0, length);
    allocation.pointer = pointer;
    allocation.length = length;
    ++alloc_calls;
    return pointer;
}

static void WifiClearSecret(APTR pointer, size_t length)
{
    volatile UBYTE *bytes = pointer;
    size_t i;

    ++clear_calls;
    if (pointer == NULL)
        return;
    for (i = 0; i < length; ++i)
        bytes[i] = 0;
}

static void FreeVec(APTR pointer)
{
    const UBYTE *bytes = pointer;
    size_t i;

    CHECK(pointer != NULL && pointer == allocation.pointer);
    for (i = 0; i < allocation.length; ++i)
        if (bytes[i] != 0)
        {
            ++bad_free_calls;
            break;
        }
    free(pointer);
    allocation.pointer = NULL;
    allocation.length = 0;
    ++free_calls;
}

static void check_balanced(void)
{
    CHECK(!mock_file.open);
    CHECK(successful_open_calls == fclose_calls);
    CHECK(alloc_calls == free_calls);
    CHECK(allocation.pointer == NULL);
    CHECK(bad_free_calls == 0);
    CHECK(write_calls == 0);
}
'''


HARNESS_TESTS = r'''
static void finish_success(STRPTR data, LONG length)
{
    CHECK(data != NULL);
    WifiClearSecret(data, (size_t)length + 1);
    FreeVec(data);
}

static void test_override_and_binary_bytes(void)
{
    static const UBYTE bytes[] = { 'a', 0, 'b', 0xff };
    LONG length = -1;
    STRPTR data;

    reset_mocks();
    environment_path = "RAM:dhclient.custom";
    set_fixture(bytes, sizeof(bytes));
    data = ReadLeasePrefs(&length);
    CHECK(data != NULL && length == (LONG)sizeof(bytes));
    CHECK(strcmp(opened_path, "RAM:dhclient.custom") == 0);
    CHECK(strcmp(opened_mode, "rb") == 0);
    CHECK(fread_calls == 1 && fread_requested == sizeof(bytes));
    CHECK(memcmp(data, bytes, sizeof(bytes)) == 0);
    CHECK((UBYTE)data[sizeof(bytes)] == 0);
    CHECK(fclose_calls == 1 && write_calls == 0);
    finish_success(data, length);
    check_balanced();
}

static void test_default_path_and_empty_file(void)
{
    LONG length = -1;
    STRPTR data;

    reset_mocks();
    environment_path = NULL;
    data = ReadLeasePrefs(&length);
    CHECK(data != NULL && length == 0);
    CHECK(strcmp(opened_path, "/T/dhclient.leases") == 0);
    CHECK(strcmp(opened_mode, "rb") == 0);
    CHECK(fread_calls == 1 && fread_requested == 0);
    CHECK(data[0] == '\0');
    CHECK(fclose_calls == 1);
    finish_success(data, length);
    check_balanced();
}

static void test_missing_override_and_empty_override(void)
{
    LONG length;
    STRPTR data;

    reset_mocks();
    fopen_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(strcmp(opened_path, "/T/dhclient.leases") == 0);
    CHECK(fopen_calls == 1 && fclose_calls == 0);
    check_balanced();

    reset_mocks();
    environment_path = "RAM:missing.leases";
    fopen_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(strcmp(opened_path, "RAM:missing.leases") == 0);
    CHECK(fopen_calls == 1 && fclose_calls == 0);
    check_balanced();

    reset_mocks();
    environment_path = "";
    fopen_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(opened_path[0] == '\0');
    CHECK(fopen_calls == 1 && fclose_calls == 0);
    check_balanced();
}

static void test_seek_and_size_failures(void)
{
    static const UBYTE bytes[] = { 'x', 'y' };
    LONG length;
    STRPTR data;

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    fseek_end_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fseek_end_calls == 1 && ftell_calls == 0 && fseek_set_calls == 0);
    CHECK(fclose_calls == 1 && alloc_attempts == 0);
    check_balanced();

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    fseek_set_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fseek_end_calls == 1 && ftell_calls == 1 && fseek_set_calls == 1);
    CHECK(fclose_calls == 1 && alloc_attempts == 0);
    check_balanced();

    reset_mocks();
    ftell_result = -1;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fseek_end_calls == 1 && ftell_calls == 1 && fseek_set_calls == 0);
    CHECK(fclose_calls == 1 && alloc_attempts == 0);
    check_balanced();

    reset_mocks();
    ftell_result = 0x7fffffffL;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fseek_end_calls == 1 && ftell_calls == 1 && fseek_set_calls == 0);
    CHECK(fclose_calls == 1 && alloc_attempts == 0);
    check_balanced();
}

static void test_allocation_and_read_failures(void)
{
    static const UBYTE bytes[] = { 's', 'e', 'c', 'r', 'e', 't' };
    LONG length;
    STRPTR data;

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    alloc_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(alloc_attempts == 1 && alloc_calls == 0 && fread_calls == 0);
    CHECK(fclose_calls == 1);
    check_balanced();

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    fread_result_override = 2;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fread_calls == 1 && ferror_calls == 0);
    CHECK(clear_calls == 1 && free_calls == 1 && bad_free_calls == 0);
    CHECK(fclose_calls == 1);
    check_balanced();

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    ferror_result = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fread_calls == 1 && ferror_calls == 1);
    CHECK(clear_calls == 1 && free_calls == 1 && bad_free_calls == 0);
    CHECK(fclose_calls == 1);
    check_balanced();
}

static void test_close_failure_clears_buffer(void)
{
    static const UBYTE bytes[] = { 'l', 'e', 'a', 's', 'e' };
    LONG length;
    STRPTR data;

    reset_mocks();
    set_fixture(bytes, sizeof(bytes));
    fclose_fails = TRUE;
    length = 99;
    data = ReadLeasePrefs(&length);
    CHECK(data == NULL && length == 0);
    CHECK(fread_calls == 1 && ferror_calls == 1);
    CHECK(clear_calls == 1 && free_calls == 1 && bad_free_calls == 0);
    CHECK(fclose_calls == 1);
    check_balanced();
}

int main(void)
{
    test_override_and_binary_bytes();
    test_default_path_and_empty_file();
    test_missing_override_and_empty_override();
    test_seek_and_size_failures();
    test_allocation_and_read_failures();
    test_close_failure_clears_buffer();
    printf("Extracted ReadLeasePrefs checks: %u passed; alloc/free balanced; no writes\n",
           checks);
    return 0;
}
'''


def _skip_space_and_comments(source: str, position: int) -> int:
    while position < len(source):
        if source[position].isspace():
            position += 1
        elif source.startswith("/*", position):
            end = source.find("*/", position + 2)
            if end < 0:
                raise AssertionError("unterminated C block comment")
            position = end + 2
        elif source.startswith("//", position):
            end = source.find("\n", position + 2)
            position = len(source) if end < 0 else end + 1
        else:
            break
    return position


def _matching_brace(source: str, opening: int) -> int:
    depth = 0
    position = opening
    state = "code"
    while position < len(source):
        char = source[position]
        following = source[position + 1] if position + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and following == "*":
                state = "block-comment"
                position += 1
            elif char == "/" and following == "/":
                state = "line-comment"
                position += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return position
        elif state in ("string", "character"):
            if char == "\\":
                position += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif state == "block-comment" and char == "*" and following == "/":
            state = "code"
            position += 1
        elif state == "line-comment" and char == "\n":
            state = "code"
        position += 1
    raise AssertionError("unterminated C function body")


def function_definition(source: str, name: str) -> str:
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    for match in pattern.finditer(source):
        position = match.end()
        depth = 1
        while position < len(source) and depth:
            if source[position] == "(":
                depth += 1
            elif source[position] == ")":
                depth -= 1
            position += 1
        if depth:
            continue
        position = _skip_space_and_comments(source, position)
        if position < len(source) and source[position] == "{":
            end = _matching_brace(source, position)
            start = source.rfind("\n", 0, match.start()) + 1
            return source[start : end + 1].strip()
    raise AssertionError(f"could not extract definition of {name}()")


def source_contract_checks() -> int:
    worker = (WIFI / "worker.c").read_text(encoding="utf-8")
    makefile = (WIFI / "mmakefile.src").read_text(encoding="utf-8")
    reader = function_definition(worker, "ReadLeasePrefs")
    route_dns = function_definition(worker, "FillRouteAndDNS")
    checks = 0

    def require(condition: bool, description: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(description)

    require(
        'getenv("PATH_DHCLIENT_DB")' in reader
        and 'path != NULL ? path : "/T/dhclient.leases"' in reader,
        "lease path must honor PATH_DHCLIENT_DB and default only when unset",
    )
    require(
        'fopen(path != NULL ? path : "/T/dhclient.leases", "rb")' in reader,
        "lease file must be opened in binary read-only mode",
    )
    require(
        re.search(r"ReadLeasePrefs\s*\(\s*&len\s*\)", route_dns) is not None,
        "FillRouteAndDNS must obtain lease data through ReadLeasePrefs",
    )
    require(
        re.search(r"%build_prog\s+mmake=workbench-prefs-wifi\s+nix=yes", makefile)
        is not None,
        "WiFi program build must enable nix=yes for the POSIX lease reader",
    )
    require(
        re.search(r"\b(fwrite|fputc|fputs|fprintf)\b", reader) is None,
        "lease reader must not invoke stdio output functions",
    )
    require(
        "WifiClearSecret(buf, (size_t)size + 1)" in reader
        and reader.find("WifiClearSecret(buf, (size_t)size + 1)")
        < reader.find("FreeVec(buf)"),
        "failed reads must clear the whole allocation before freeing it",
    )
    require(
        "fclose(file) != 0 && buf != NULL" in reader
        and "WifiClearSecret(buf, (size_t)*lenp + 1)" in reader,
        "close failure must clear and release a successfully read buffer",
    )
    print(f"Source contract checks: {checks} passed")
    return checks


def compile_and_run(compiler: str, optimization: str) -> str:
    source = (
        HARNESS_PREFIX
        + "\n"
        + function_definition((WIFI / "worker.c").read_text(encoding="utf-8"),
                              "ReadLeasePrefs")
        + "\n"
        + HARNESS_TESTS
    )
    with tempfile.TemporaryDirectory(prefix="wifi-lease-reader-") as temp:
        temp_path = Path(temp)
        harness = temp_path / "lease-reader.c"
        harness.write_text(source, encoding="utf-8")
        executable = temp_path / "lease-reader"
        subprocess.run(
            [
                compiler,
                "-std=c99",
                optimization,
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(harness),
                "-o",
                str(executable),
            ],
            check=True,
            cwd=WORKBENCH,
        )
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        result = subprocess.run(
            [str(executable)],
            check=False,
            cwd=WORKBENCH,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"extracted lease reader exited {result.returncode}:\n{result.stdout}"
            )
        return result.stdout.strip()


def main() -> int:
    compiler = os.environ.get("CC", "clang")
    if not (WIFI / "worker.c").is_file():
        raise SystemExit(f"production source not found: {WIFI / 'worker.c'}")

    source_checks = source_contract_checks()
    for optimization in ("-O0", "-O2"):
        output = compile_and_run(compiler, optimization)
        print(f"extracted ReadLeasePrefs {optimization}: {output}")
    print(f"Total source contract checks: {source_checks}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
