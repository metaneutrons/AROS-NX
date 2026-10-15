#!/usr/bin/env python3
"""Host regressions for AROSTCP's DHCLIENT string and path callbacks.

The harness compiles the production setvalue() and dhclient_path_changed()
bodies, plus the actual ReadItem(), PathPart() and AddPart() implementations.
AROS allocation, error reporting and DOS library entry macros are adapted for
the host. This exercises setvalue() with the generated line; it is not a test
of parsefile(), the running AROSTCP task, or filesystem access on AROS.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True

AROSTCP = Path(__file__).resolve().parents[1]
ROOT = AROSTCP.parents[3]
CONFIG_SOURCE = AROSTCP / "bsdsocket/kern/amiga_config.c"
MAIN_SOURCE = AROSTCP / "bsdsocket/kern/amiga_main.c"
READITEM_SOURCE = ROOT / "rom/dos/readitem.c"
PATHPART_SOURCE = ROOT / "rom/dos/pathpart.c"
ADDPART_SOURCE = ROOT / "rom/dos/addpart.c"
DEFAULTS_SOURCE = ROOT / "arch/riscv-esp32p4/image/make-wifi-defaults.py"


def load_generated_dhclient_line() -> tuple[str, str]:
    spec = importlib.util.spec_from_file_location("wifi_defaults", DEFAULTS_SOURCE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {DEFAULTS_SOURCE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    config = module.defaults()["Prefs/Env-Archive/AROSTCP/db/general.config"]
    line = next((line for line in config.splitlines(keepends=True)
                 if line.startswith("DHCLIENT=")), None)
    if line is None:
        raise RuntimeError("generated general.config has no DHCLIENT assignment")
    return line, line[len("DHCLIENT="):].strip()


GENERATED_LINE, GENERATED_VALUE = load_generated_dhclient_line()


def extract_function(source: str, signature: str) -> str:
    """Return one C function, balancing braces while respecting comments/strings."""
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"missing production function signature: {signature}")
    opening = source.find("{", start + len(signature))
    if opening < 0:
        raise RuntimeError(f"missing function body: {signature}")

    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                    state == "character" and char == "'"):
                state = "code"
        elif char == "/" and following == "*":
            state = "block-comment"
            index += 1
        elif char == "/" and following == "/":
            state = "line-comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
        index += 1
    raise RuntimeError(f"unterminated production function: {signature}")


def extract_readitem_parts() -> tuple[str, str]:
    source = READITEM_SOURCE.read_text(encoding="utf-8")
    macros_start = source.index("/* Macro to get a character from the input source */")
    body_start = source.index("    STRPTR b=buffer;", macros_start)
    body_end = source.index("    AROS_LIBFUNC_EXIT", body_start)
    return source[macros_start:body_start], source[body_start:body_end]


def extract_dos_body(path: Path, library_entry: str) -> str:
    source = path.read_text(encoding="utf-8")
    entry = source.index(library_entry)
    body_start = source.index("AROS_LIBFUNC_INIT", entry) + len("AROS_LIBFUNC_INIT")
    body_end = source.index("AROS_LIBFUNC_EXIT", body_start)
    return source[body_start:body_end]


CONFIG_TEXT = CONFIG_SOURCE.read_text(encoding="utf-8")
MAIN_TEXT = MAIN_SOURCE.read_text(encoding="utf-8")
SETVALUE_BODY = extract_function(CONFIG_TEXT, "LONG\nsetvalue(struct CSource *args")
GETVALUE_BODY = extract_function(CONFIG_TEXT, "LONG\ngetvalue(struct CSource *args")
DHCLIENT_CALLBACK = extract_function(
    MAIN_TEXT, "int dhclient_path_changed(void *pt, IPTR new)")
READITEM_MACROS, READITEM_BODY = extract_readitem_parts()
PATHPART_BODY = extract_dos_body(PATHPART_SOURCE, "AROS_LH1(STRPTR, PathPart,")
ADDPART_BODY = extract_dos_body(ADDPART_SOURCE, "AROS_LH3(BOOL, AddPart,")


HARNESS = r"""
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#undef FILENAME_MAX
#define FILENAME_MAX 256
#define KEYWORDLEN 24

typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t WORD;
typedef char UBYTE;
typedef intptr_t IPTR;
typedef char TEXT;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef int BOOL;

#define ITEM_EQUAL    -2
#define ITEM_ERROR    -1
#define ITEM_NOTHING   0
#define ITEM_UNQUOTED  1
#define ITEM_QUOTED    2
#define RETURN_OK      0
#define RETURN_WARN    5
#define RETURN_ERROR   20
#define VF_TABLE       (1 << 0)
#define VF_READ        (1 << 1)
#define VF_WRITE       (1 << 2)
#define VF_CONF        (1 << 3)
#define VF_FREE        (1 << 8)
#define VF_RCONF       (VF_CONF | VF_READ)
#define M_CFGVAR       1
#define M_WAITOK       1
#define DOSTRUE        1
#define DOSFALSE       0
#define ERROR_LINE_TOO_LONG 120
#define CURRENT(source) ((source)->CS_Buffer + (source)->CS_CurChr)
#define bcopy(source, target, length) memmove((target), (source), (length))

struct CSource {
    STRPTR CS_Buffer;
    LONG CS_Length;
    LONG CS_CurChr;
};

struct in_addr { uint32_t s_addr; };
struct cfg_variable;
typedef LONG (*var_f)(struct CSource *, UBYTE **, struct CSource *);
typedef int (*notify_f)(void *, IPTR);
enum var_type { VAR_FUNC = 1, VAR_LONG, VAR_STRP, VAR_FLAG, VAR_INET, VAR_ENUM };
struct cfg_variable {
    enum var_type type;
    WORD flags;
    const UBYTE *index;
    void *value;
    notify_f notify;
};

static UBYTE ERR_UNKNOWN[] = "Unknown command";
static UBYTE ERR_ILLEGAL_VAR[] = "%s: unknown variable %s";
static UBYTE ERR_ILLEGAL_IND[] = "%s: unknown index %s";
static UBYTE ERR_SYNTAX[] = "Syntax error";
static UBYTE ERR_TOO_LONG[] = "Result too long";
static UBYTE ERR_MEMORY[] = "Memory exhausted";
static UBYTE ERR_VALUE[] = "Illegal value";
static UBYTE ERR_NOWRITE[] = "%s: Variable %s is not writeable";

static BOOL initialized;
static LONG io_error;
static int live_allocations;
static STRPTR dhclient_name;
static TEXT db_path[FILENAME_MAX];
static TEXT dhclient_path[FILENAME_MAX];
static UBYTE *KW_VARS = (UBYTE *)"DHCLIENT";
static int dhclient_path_changed(void *pt, IPTR new);
static LONG setvalue(struct CSource *args, UBYTE **errstrp, struct CSource *res);
static LONG getvalue(struct CSource *args, UBYTE **errstrp, struct CSource *res);
static LONG ReadItem(STRPTR buffer, LONG maxchars, struct CSource *input);
static STRPTR PathPart(CONST_STRPTR path);
static BOOL AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size);

static const UBYTE INITIAL_NAME[] = "C/dhclient";

static struct cfg_variable variables[] = {
    { VAR_STRP, VF_RCONF, NULL, &dhclient_name, dhclient_path_changed }
};

static int FindArg(UBYTE *list, UBYTE *name)
{
    if (strcmp((const char *)list, "DHCLIENT") == 0 &&
        strcasecmp((const char *)name, "DHCLIENT") == 0)
        return 0;
    return -1;
}

static int StrToLong(const char *text, LONG *value)
{
    char *end;
    long parsed = strtol(text, &end, 10);
    if (end == text)
        return 0;
    *value = (LONG)parsed;
    return (int)(end - text);
}

static int ultoa(unsigned long value, char *buffer)
{
    return sprintf(buffer, "%lu", value);
}

static int __inet_aton(const char *text, struct in_addr *address)
{
    (void)text;
    (void)address;
    return 0;
}

static void *bsd_malloc(size_t size, int type, int flags)
{
    void *allocation;
    (void)type;
    (void)flags;
    allocation = malloc(size);
    if (allocation != NULL)
        live_allocations++;
    return allocation;
}

static void bsd_free(void *allocation, int type)
{
    (void)type;
    if (allocation != NULL) {
        live_allocations--;
        free(allocation);
    }
}

static int csprintf(struct CSource *source, const UBYTE *format, ...)
{
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(CURRENT(source),
                        (size_t)(source->CS_Length - source->CS_CurChr),
                        (const char *)format, args);
    va_end(args);
    return written;
}

static int Input(void) { return 0; }
static int FGetC(int input) { (void)input; return EOF; }
static void UnGetC(int input, int count) { (void)input; (void)count; }
static LONG SetIoErr(LONG error) { io_error = error; return error; }

static LONG ReadItem(STRPTR buffer, LONG maxchars, struct CSource *input)
{
@READITEM_MACROS@
@READITEM_BODY@
}

static STRPTR PathPart(CONST_STRPTR path)
{
@PATHPART_BODY@
}

static BOOL AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size)
{
@ADDPART_BODY@
}

@SETVALUE_BODY@

@GETVALUE_BODY@

@DHCLIENT_CALLBACK@

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(2); \
    } \
} while (0)

static const char generated_line[] = @GENERATED_LINE@;
static const char generated_value[] = @GENERATED_VALUE@;

static void reset_case(void)
{
    if (variables[0].flags & VF_FREE)
        bsd_free(dhclient_name, M_CFGVAR);
    variables[0].flags = VF_RCONF;
    initialized = 0;
    dhclient_name = (STRPTR)INITIAL_NAME;
    strcpy(db_path, "SYS:System/Network/AROSTCP/db");
    strcpy(dhclient_path, "old/path");
    io_error = 0;
}

static LONG apply_line(const char *line, UBYTE **error)
{
    UBYTE reply[128];
    struct CSource args = { (STRPTR)line, (LONG)strlen(line), 0 };
    struct CSource result = { (STRPTR)reply, sizeof(reply), 0 };
    *error = NULL;
    reply[0] = '\0';
    return setvalue(&args, error, &result);
}

static LONG query_line(const char *line, UBYTE *reply, LONG reply_length,
                       UBYTE **error)
{
    struct CSource args = { (STRPTR)line, (LONG)strlen(line), 0 };
    struct CSource result = { reply, reply_length, 0 };
    *error = NULL;
    return getvalue(&args, error, &result);
}

static void test_generated_line(void)
{
    UBYTE *error;
    LONG status;
    reset_case();
    CHECK(strlen(generated_value) > KEYWORDLEN - 1);
    status = apply_line(generated_line, &error);
    CHECK(status == RETURN_OK);
    CHECK(error == NULL);
    CHECK(strcmp(dhclient_path, generated_value) == 0);
    CHECK(strcmp(dhclient_name, generated_value) == 0);
    CHECK(variables[0].flags & VF_FREE);
    {
        UBYTE reply[FILENAME_MAX + 1];
        CHECK(query_line("DHCLIENT\n", reply, sizeof(reply), &error) == RETURN_OK);
        CHECK(error == NULL);
        CHECK(strcmp(reply, generated_value) == 0);
    }
}

static void test_relative_line(void)
{
    UBYTE *error;
    reset_case();
    CHECK(apply_line("DHCLIENT=C/dhclient\n", &error) == RETURN_OK);
    CHECK(error == NULL);
    CHECK(strcmp(dhclient_path,
                 "SYS:System/Network/AROSTCP/C/dhclient") == 0);
    CHECK(strcmp(dhclient_name, "C/dhclient") == 0);
}

static void test_maximum_valid_value(void)
{
    char path[FILENAME_MAX];
    char line[FILENAME_MAX + 32];
    UBYTE *error;
    reset_case();
    path[0] = 'V';
    path[1] = ':';
    memset(path + 2, 'x', FILENAME_MAX - 3);
    path[FILENAME_MAX - 1] = '\0';
    CHECK(strlen(path) == FILENAME_MAX - 1);
    snprintf(line, sizeof(line), "DHCLIENT=\"%s\"\n", path);
    CHECK(apply_line(line, &error) == RETURN_OK);
    CHECK(error == NULL);
    CHECK(strcmp(dhclient_path, path) == 0);
    CHECK(strcmp(dhclient_name, path) == 0);
}

static void test_overlong_value_is_rejected(void)
{
    char path[FILENAME_MAX + 1];
    char line[FILENAME_MAX + 32];
    UBYTE *error;
    reset_case();
    path[0] = 'V';
    path[1] = ':';
    memset(path + 2, 'x', FILENAME_MAX - 1);
    path[FILENAME_MAX] = '\0';
    CHECK(strlen(path) == FILENAME_MAX);
    snprintf(line, sizeof(line), "DHCLIENT=\"%s\"\n", path);
    CHECK(apply_line(line, &error) == RETURN_WARN);
    CHECK(error == ERR_SYNTAX);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(strcmp(dhclient_name, (const char *)INITIAL_NAME) == 0);
    CHECK(!(variables[0].flags & VF_FREE));
}

static void test_malformed_values_are_rejected(void)
{
    UBYTE *error;
    reset_case();
    CHECK(apply_line("DHCLIENT\n", &error) == RETURN_WARN);
    CHECK(error == ERR_SYNTAX);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(strcmp(dhclient_name, (const char *)INITIAL_NAME) == 0);
    CHECK(apply_line("DHCLIENT=\"unterminated\n", &error) == RETURN_WARN);
    CHECK(error == ERR_SYNTAX);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(strcmp(dhclient_name, (const char *)INITIAL_NAME) == 0);
    CHECK(!(variables[0].flags & VF_FREE));
}

static void test_empty_and_null_notifications_reject_without_commit(void)
{
    UBYTE *error;
    reset_case();
    CHECK(dhclient_path_changed(NULL, (IPTR)NULL) == 0);
    CHECK(dhclient_path_changed(NULL, (IPTR)"") == 0);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(apply_line("DHCLIENT=\"\"\n", &error) == RETURN_WARN);
    CHECK(error == ERR_VALUE);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(strcmp(dhclient_name, (const char *)INITIAL_NAME) == 0);
    CHECK(!(variables[0].flags & VF_FREE));
    CHECK(live_allocations == 0);
}

static void test_callback_rejects_too_long_absolute_path(void)
{
    char path[FILENAME_MAX + 1];
    reset_case();
    path[0] = 'V';
    path[1] = ':';
    memset(path + 2, 'x', FILENAME_MAX - 1);
    path[FILENAME_MAX] = '\0';
    CHECK(dhclient_path_changed(NULL, (IPTR)path) == 0);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
}

static void test_callback_preserves_path_when_relative_join_overflows(void)
{
    reset_case();
    db_path[0] = 'V';
    db_path[1] = ':';
    memset(db_path + 2, 'x', FILENAME_MAX - 6);
    memcpy(db_path + FILENAME_MAX - 4, "/db", 4);
    CHECK(strlen(db_path) == FILENAME_MAX - 1);
    CHECK(dhclient_path_changed(NULL, (IPTR)"C/dhclient") == 0);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
    CHECK(io_error == ERROR_LINE_TOO_LONG);
}

static void test_callback_rejects_unterminated_database_path(void)
{
    reset_case();
    memset(db_path, 'x', sizeof(db_path));
    CHECK(strnlen(db_path, sizeof(db_path)) == sizeof(db_path));
    CHECK(dhclient_path_changed(NULL, (IPTR)"C/dhclient") == 0);
    CHECK(strcmp(dhclient_path, "old/path") == 0);
}

static void test_query_path_boundary_and_small_response(void)
{
    char path[FILENAME_MAX];
    char expected_pair[FILENAME_MAX * 2 + 2];
    UBYTE exact[FILENAME_MAX];
    UBYTE too_small[FILENAME_MAX - 1];
    UBYTE pair_too_small[FILENAME_MAX * 2 - 1];
    UBYTE *error;
    reset_case();
    path[0] = 'V';
    path[1] = ':';
    memset(path + 2, 'x', FILENAME_MAX - 3);
    path[FILENAME_MAX - 1] = '\0';
    dhclient_name = path;
    CHECK(strlen(path) == FILENAME_MAX - 1);

    memset(exact, '#', sizeof(exact));
    CHECK(query_line("DHCLIENT\n", exact, sizeof(exact), &error) == RETURN_OK);
    CHECK(error == NULL);
    CHECK(strcmp(exact, path) == 0);

    memset(too_small, '#', sizeof(too_small));
    CHECK(query_line("DHCLIENT\n", too_small, sizeof(too_small), &error) ==
          RETURN_ERROR);
    CHECK(error == ERR_TOO_LONG);
    CHECK(too_small[0] == '#');

    memset(pair_too_small, '#', sizeof(pair_too_small));
    CHECK(query_line("DHCLIENT DHCLIENT\n", pair_too_small,
                     sizeof(pair_too_small), &error) == RETURN_ERROR);
    CHECK(error == ERR_TOO_LONG);
    CHECK(strncmp((const char *)pair_too_small, path, FILENAME_MAX - 1) == 0);
    CHECK(pair_too_small[FILENAME_MAX - 1] == '#');

    snprintf(expected_pair, sizeof(expected_pair), "%s %s", path, path);
    {
        UBYTE exact_pair[FILENAME_MAX * 2];
        CHECK(query_line("DHCLIENT DHCLIENT\n", exact_pair,
                         sizeof(exact_pair), &error) == RETURN_OK);
        CHECK(error == NULL);
        CHECK(strcmp(exact_pair, expected_pair) == 0);
    }
}

static void test_query_rejects_unterminated_stored_value(void)
{
    char path[FILENAME_MAX + 1];
    UBYTE reply[FILENAME_MAX + 1];
    UBYTE *error;
    reset_case();
    memset(path, 'x', FILENAME_MAX);
    path[FILENAME_MAX] = '\0';
    dhclient_name = path;
    memset(reply, '#', sizeof(reply));
    CHECK(query_line("DHCLIENT\n", reply, sizeof(reply), &error) == RETURN_ERROR);
    CHECK(error == ERR_TOO_LONG);
    CHECK(reply[0] == '#');
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s CASE\n", argv[0]);
        return 64;
    }
    if (strcmp(argv[1], "generated") == 0)
        test_generated_line();
    else if (strcmp(argv[1], "relative") == 0)
        test_relative_line();
    else if (strcmp(argv[1], "maximum") == 0)
        test_maximum_valid_value();
    else if (strcmp(argv[1], "overlong") == 0)
        test_overlong_value_is_rejected();
    else if (strcmp(argv[1], "malformed") == 0)
        test_malformed_values_are_rejected();
    else if (strcmp(argv[1], "empty-null") == 0)
        test_empty_and_null_notifications_reject_without_commit();
    else if (strcmp(argv[1], "callback-overlong") == 0)
        test_callback_rejects_too_long_absolute_path();
    else if (strcmp(argv[1], "join-overflow") == 0)
        test_callback_preserves_path_when_relative_join_overflows();
    else if (strcmp(argv[1], "unterminated-db") == 0)
        test_callback_rejects_unterminated_database_path();
    else if (strcmp(argv[1], "query-boundary") == 0)
        test_query_path_boundary_and_small_response();
    else if (strcmp(argv[1], "query-overlong") == 0)
        test_query_rejects_unterminated_stored_value();
    else {
        fprintf(stderr, "unknown case: %s\n", argv[1]);
        return 64;
    }
    puts("PASS");
    return 0;
}
"""


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def build_harness(directory: Path, *, setvalue_body: str = SETVALUE_BODY,
                  getvalue_body: str = GETVALUE_BODY,
                  callback_body: str = DHCLIENT_CALLBACK,
                  name: str = "current") -> Path:
    readitem_macros, readitem_body = READITEM_MACROS, READITEM_BODY
    pathpart_body, addpart_body = PATHPART_BODY, ADDPART_BODY
    replacements = {
        "@READITEM_MACROS@": readitem_macros,
        "@READITEM_BODY@": readitem_body,
        "@PATHPART_BODY@": pathpart_body,
        "@ADDPART_BODY@": addpart_body,
        "@SETVALUE_BODY@": setvalue_body,
        "@GETVALUE_BODY@": getvalue_body,
        "@DHCLIENT_CALLBACK@": callback_body,
        "@GENERATED_LINE@": c_string(GENERATED_LINE),
        "@GENERATED_VALUE@": c_string(GENERATED_VALUE),
    }
    source = HARNESS
    for placeholder, value in replacements.items():
        source = source.replace(placeholder, value)

    source_path = directory / f"{name}.c"
    binary_path = directory / name
    source_path.write_text(source, encoding="utf-8")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    command = compiler + [
        "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-parentheses", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-O1", str(source_path), "-o", str(binary_path),
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"host harness compilation failed:\n{result.stdout}{result.stderr}")
    return binary_path


class DhclientPathRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix="arostcp-dhclient-test-")
        cls.build_dir = Path(cls.temp.name)
        cls.binary = build_harness(cls.build_dir)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def assert_case_passes(self, case: str) -> None:
        result = subprocess.run([str(self.binary), case], text=True,
                                capture_output=True)
        self.assertEqual(result.returncode, 0,
                         f"{case} failed:\n{result.stdout}{result.stderr}")
        self.assertIn("PASS", result.stdout)

    def test_generated_absolute_dhclient_line_is_accepted(self) -> None:
        self.assertGreater(len(GENERATED_VALUE), 23)
        self.assert_case_passes("generated")

    def test_relative_path_resolves_from_stack_root(self) -> None:
        self.assert_case_passes("relative")

    def test_filename_maximum_value_is_accepted(self) -> None:
        self.assert_case_passes("maximum")

    def test_overlong_setvalue_path_is_rejected_without_state_change(self) -> None:
        self.assert_case_passes("overlong")

    def test_malformed_assignments_are_rejected_without_state_change(self) -> None:
        self.assert_case_passes("malformed")

    def test_empty_and_null_notifications_are_rejected(self) -> None:
        self.assert_case_passes("empty-null")

    def test_callback_rejects_overlong_absolute_path(self) -> None:
        self.assert_case_passes("callback-overlong")

    def test_callback_preserves_old_path_when_relative_join_overflows(self) -> None:
        self.assert_case_passes("join-overflow")

    def test_callback_rejects_unterminated_database_path(self) -> None:
        self.assert_case_passes("unterminated-db")

    def test_getvalue_full_path_and_small_response_boundaries(self) -> None:
        self.assert_case_passes("query-boundary")

    def test_getvalue_rejects_unterminated_stored_path(self) -> None:
        self.assert_case_passes("query-overlong")

    def test_old_keyword_sized_setvalue_buffer_fails_generated_line(self) -> None:
        old_setvalue = SETVALUE_BODY.replace(
            "UBYTE StringValue[FILENAME_MAX];",
            "UBYTE StringValue[KEYWORDLEN];", 1)
        self.assertNotEqual(old_setvalue, SETVALUE_BODY,
                            "setvalue source changed; refresh this negative control")
        old_binary = build_harness(self.build_dir, setvalue_body=old_setvalue,
                                   name="old-keyword-buffer")
        result = subprocess.run([str(old_binary), "generated"], text=True,
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status == RETURN_OK", result.stderr)

    def test_old_zero_success_callback_rejects_generated_line(self) -> None:
        old_callback = DHCLIENT_CALLBACK.rsplit("return 1;", 1)
        self.assertEqual(len(old_callback), 2,
                         "callback success return changed; refresh this negative control")
        old_binary = build_harness(
            self.build_dir, callback_body=old_callback[0] + "return 0;" + old_callback[1],
            name="old-zero-success-callback")
        result = subprocess.run([str(old_binary), "generated"], text=True,
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("status == RETURN_OK", result.stderr)

    def test_old_keyword_sized_getvalue_read_truncates_generated_path(self) -> None:
        old_getvalue = GETVALUE_BODY.replace(
            "strnlen(value, FILENAME_MAX)", "strnlen(value, KEYWORDLEN)", 1)
        self.assertNotEqual(old_getvalue, GETVALUE_BODY,
                            "getvalue source changed; refresh this negative control")
        old_binary = build_harness(self.build_dir, getvalue_body=old_getvalue,
                                   name="old-keyword-getvalue")
        result = subprocess.run([str(old_binary), "generated"], text=True,
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("strcmp(reply, generated_value) == 0", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
