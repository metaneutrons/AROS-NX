#!/usr/bin/env python3
"""Compile and exercise the lease-selection helpers extracted from worker.c."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


WORKBENCH = Path(__file__).resolve().parents[3]
WIFI = WORKBENCH / "prefs" / "wifi"

HARNESS_PREFIX = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL;
typedef uint32_t ULONG;
typedef unsigned char UBYTE;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
#define TRUE 1
#define FALSE 0

static unsigned checks;
static unsigned char ToUpper(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? (unsigned char)(c - 'a' + 'A') : c;
}

static void check_at(int condition, const char *text, int line)
{
    ++checks;
    if (!condition)
    {
        fprintf(stderr, "FAIL line %d: %s\n", line, text);
        exit(1);
    }
}
#define CHECK(x) check_at((x), #x, __LINE__)
'''

HARNESS_TESTS = r'''
static void expect_lease(const char *data, const char *interface,
    const char *address, const char *want_gateway, const char *want_dns)
{
    char gateway[20], dns[64];

    SelectLeaseForAddress(data, strlen(data), interface, address, gateway,
        sizeof(gateway), dns, sizeof(dns));
    CHECK(strcmp(gateway, want_gateway) == 0);
    CHECK(strcmp(dns, want_dns) == 0);
}

int main(void)
{
    const char *multiple =
        "lease { interface \"net0\"; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; option domain-name-servers 1.1.1.1; }\n"
        "lease { interface \"net1\"; fixed-address 192.0.2.20; "
        "option routers 192.0.2.21; option domain-name-servers 8.8.8.8; }\n"
        "lease { interface \"net0\"; fixed-address 192.0.2.10; "
        "option routers 192.0.2.254; option domain-name-servers 9.9.9.9; }\n";
    const char *stale_and_prefix =
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; }\n"
        "lease { interface net0; fixed-address 192.0.2.100; "
        "option routers 192.0.2.100; }\n"
        "lease { interface net00; fixed-address 192.0.2.10; "
        "option routers 192.0.2.99; }\n";
    const char *truncated =
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; }\n"
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.2;";
    const char *quoted_comments =
        "# lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.99; }\n"
        "\"lease { interface \\\"net0\\\"; fixed-address 192.0.2.10; }\"\n"
        "lease { interface \"net0\"; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; }\n";
    const char *no_cross_block =
        "lease { interface net0; fixed-address 192.0.2.10; }\n"
        "lease { interface net1; fixed-address 192.0.2.20; "
        "option routers 192.0.2.1; option domain-name-servers 8.8.8.8; }\n";
    const char *missing_options =
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; option domain-name-servers 1.1.1.1; }\n"
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.254; }\n";
    const char *malformed_then_valid =
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.2 }\n"
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.3; }\n";
    const char *duplicate_identity =
        "lease { interface net0; interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.7; }\n"
        "lease { interface net0; fixed-address 192.0.2.10; "
        "fixed-address 192.0.2.10; option routers 192.0.2.8; }\n";
    const char *nested_and_writer_statements =
        "lease6 { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.90; }\n"
        "lease { interface net0; fixed-address 192.0.2.10; "
        "on expiry { interface netX; fixed-address 192.0.2.10; "
        "option routers 192.0.2.91; }; "
        "option host-name \"quote \\\" brace } { text\"; "
        "renew 6 2026/10/10 12:00:00; rebind 6 2026/10/10 12:00:00; "
        "expire 6 2026/10/10 12:00:00; option routers 192.0.2.5; }";
    static const char exact_record[] =
        "lease { interface net0; fixed-address 192.0.2.10; "
        "option routers 192.0.2.1; option domain-name-servers 1.1.1.1; }";
    char gateway[20];
    size_t cut;

    expect_lease(multiple, "net0", "192.0.2.10", "192.0.2.254", "9.9.9.9");
    expect_lease(stale_and_prefix, "net0", "192.0.2.10", "192.0.2.1", "");
    expect_lease(truncated, "net0", "192.0.2.10", "192.0.2.1", "");
    expect_lease(quoted_comments, "net0", "192.0.2.10", "192.0.2.1", "");
    expect_lease(no_cross_block, "net0", "192.0.2.10", "", "");
    expect_lease(missing_options, "net0", "192.0.2.10", "192.0.2.254", "");
    expect_lease(malformed_then_valid, "net0", "192.0.2.10", "192.0.2.3", "");
    expect_lease(duplicate_identity, "net0", "192.0.2.10", "", "");
    expect_lease(stale_and_prefix, "net0", "192.0.2.1", "", "");
    expect_lease(nested_and_writer_statements, "net0", "192.0.2.10",
        "192.0.2.5", "");

    for (cut = 0; cut < sizeof(exact_record) - 1; cut++)
    {
        char partial_gateway[20] = "x", partial_dns[64] = "x";
        SelectLeaseForAddress(exact_record, cut, "net0", "192.0.2.10",
            partial_gateway, sizeof(partial_gateway), partial_dns,
            sizeof(partial_dns));
        CHECK(partial_gateway[0] == '\0' && partial_dns[0] == '\0');
    }
    {
        char exact_data[sizeof(exact_record) - 1];
        char one_byte[1];
        memcpy(exact_data, exact_record, sizeof(exact_data));
        SelectLeaseForAddress(exact_data, sizeof(exact_data), "net0",
            "192.0.2.10", gateway, sizeof(gateway), one_byte,
            sizeof(one_byte));
        CHECK(strcmp(gateway, "192.0.2.1") == 0 && one_byte[0] == '\0');
        SelectLeaseForAddress(exact_data, sizeof(exact_data), "net0",
            "192.0.2.10", NULL, 0, one_byte, sizeof(one_byte));
        CHECK(one_byte[0] == '\0');
    }

    {
        const char *interfaces =
            "# net0 DEV=fake GW=192.0.2.90\n"
            "net10 DEV=other GW=192.0.2.10\n"
            "net1 DEV=wireless GW6=2001:db8::1 gW=192.0.2.1 ; ignored\n"
            "net1-extra DEV=other GW=192.0.2.99\n";
        CHECK(InterfaceGatewayFromPrefs(interfaces, strlen(interfaces),
            "net1", gateway, sizeof(gateway)));
        CHECK(strcmp(gateway, "192.0.2.1") == 0);
    }
    {
        const char *interfaces = "net1 DEV=wireless GW6=2001:db8::1\n";
        CHECK(!InterfaceGatewayFromPrefs(interfaces, strlen(interfaces),
            "net1", gateway, sizeof(gateway)));
        CHECK(gateway[0] == '\0');
    }
    {
        static const char no_output[] = "net1 DEV=wireless GW=192.0.2.1\n";
        CHECK(!InterfaceGatewayFromPrefs(no_output, sizeof(no_output) - 1,
            "net1", NULL, 0));
    }
    {
        const char *continued = "net1 DEV=wireless+\n    GW=192.0.2.45\n";
        char one_byte[1];
        CHECK(InterfaceGatewayFromPrefs(continued, strlen(continued), "net1",
            gateway, sizeof(gateway)));
        CHECK(strcmp(gateway, "192.0.2.45") == 0);
        CHECK(InterfaceGatewayFromPrefs(continued, strlen(continued), "net1",
            one_byte, sizeof(one_byte)));
        CHECK(one_byte[0] == '\0');
    }

    printf("Extracted lease selection checks: %u passed\n", checks);
    return 0;
}
'''


def matching_brace(source: str, open_at: int) -> int:
    depth = 0
    state = "code"
    pos = open_at
    while pos < len(source):
        char = source[pos]
        following = source[pos + 1] if pos + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and following == "*":
                state = "block-comment"
                pos += 1
            elif char == "/" and following == "/":
                state = "line-comment"
                pos += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return pos
        elif state in ("string", "char"):
            if char == "\\":
                pos += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif state == "block-comment" and char == "*" and following == "/":
            state = "code"
            pos += 1
        elif state == "line-comment" and char == "\n":
            state = "code"
        pos += 1
    raise AssertionError("unterminated C brace block")


def extract_block(source: str, marker: str) -> str:
    start = source.index(marker)
    opening = source.index("{", start)
    return source[start : matching_brace(source, opening) + 1]


def function_definition(source: str, name: str) -> str:
    match = re.search(r"\b" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError(f"could not find {name}()")
    opening = source.index("{", match.end())
    start = source.rfind("\n", 0, match.start()) + 1
    return source[start : matching_brace(source, opening) + 1].strip()


def source_checks(worker: str) -> int:
    route = function_definition(worker, "FillRouteAndDNS")
    checks = [
        ("SelectLeaseForAddress(buf, (size_t)len" in route,
         "route display must use the bounded lease selector"),
        ("m->wm_Interface" in route and "m->wm_Address" in route,
         "lease selection must use the current interface and address"),
        ('"%s/interfaces"' in route and '"%s/netdb-myhost"' in route,
         "both fallback files must come from the configured database path"),
        ("StackDBDir(db, sizeof(db))" in route,
         "fallback configuration must follow StackDBDir"),
        ('"ENV:AROSTCP/db/interfaces"' not in route and
         '"ENV:AROSTCP/db/netdb-myhost"' not in route,
         "fallbacks must not bypass the configured database"),
    ]
    for passed, description in checks:
        if not passed:
            raise AssertionError(description)
    print(f"Source contract checks: {len(checks)} passed")
    return len(checks)


def compile_and_run(compiler: str, optimization: str, worker: str) -> str:
    declarations = extract_block(worker, "enum\n{") + ";\n" + \
        extract_block(worker, "struct LeaseToken") + ";\n"
    names = (
        "LeaseNextToken", "LeaseTokenIs", "LeaseCopyToken",
        "SelectLeaseForAddress", "InterfaceGatewayFromPrefs",
    )
    functions = "\n".join(function_definition(worker, name) for name in names)
    source = HARNESS_PREFIX + declarations + functions + HARNESS_TESTS
    with tempfile.TemporaryDirectory(prefix="wifi-lease-selection-") as temp:
        c_file = Path(temp) / "lease-selection.c"
        executable = Path(temp) / "lease-selection"
        c_file.write_text(source, encoding="utf-8")
        subprocess.run(
            [compiler, "-std=c99", optimization, "-Wall", "-Wextra", "-Werror",
             "-g", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", str(c_file), "-o", str(executable)],
            check=True,
        )
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        result = subprocess.run([str(executable)], cwd=WORKBENCH, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False)
        if result.returncode != 0:
            raise AssertionError(f"extracted helper exited {result.returncode}:\n{result.stdout}")
        return result.stdout.strip()


def main() -> int:
    compiler = os.environ.get("CC", "clang")
    worker_path = WIFI / "worker.c"
    if not worker_path.is_file():
        raise SystemExit(f"production source not found: {worker_path}")
    worker = worker_path.read_text(encoding="utf-8")
    count = source_checks(worker)
    for optimization in ("-O0", "-O2"):
        print(f"extracted helpers {optimization}: {compile_and_run(compiler, optimization, worker)}")
    print(f"Total source contract checks: {count}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
