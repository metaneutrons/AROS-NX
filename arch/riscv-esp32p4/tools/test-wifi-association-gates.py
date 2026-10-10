#!/usr/bin/env python3
"""Compile the actual default macros and LibraryInit association gate."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import sys
import tempfile


PORT = Path(__file__).resolve().parents[1]
RUNTIME_SOURCE = PORT / "network" / "runtime_probe.c"
NETWORK_MMAKEFILE = PORT / "network" / "mmakefile.src"
EXTRACTOR = PORT / "tools" / "test-sdmmc-host.py"
GATES = (
    "P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED",
    "P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_function(source: Path, name: str, signature: str) -> str:
    namespace = runpy.run_path(str(EXTRACTOR))
    extract_definition = namespace.get("extract_definition")
    if not callable(extract_definition):
        raise ValueError(f"source extractor is unavailable in {EXTRACTOR}")
    return extract_definition(source, name, signature)


def extract_default(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(?m)^#ifndef[ \t]+{re.escape(name)}\r?\n"
        rf"#define[ \t]+{re.escape(name)}[ \t]+0\r?\n"
        r"#endif(?:\r?\n|$)"
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(
            f"expected exactly one default-zero block for {name}, "
            f"found {len(matches)}"
        )
    return matches[0].group(0)


def audit_build_flag(makefile: str, name: str) -> str:
    expected = re.compile(
        rf"(?m)^ifeq \(\$\({re.escape(name)}\),1\)\r?\n"
        rf"USER_CPPFLAGS \+= -D{re.escape(name)}=1\r?\n"
        r"endif(?:\r?\n|$)"
    )
    matches = list(expected.finditer(makefile))
    emitted_defines = re.findall(
        rf"(?m)^[ \t]*USER_CPPFLAGS[ \t]*\+=.*-D{re.escape(name)}(?:=|\b)",
        makefile,
    )
    if len(matches) != 1 or len(emitted_defines) != 1:
        raise ValueError(
            f"{name} must have one 1-only guarded USER_CPPFLAGS definition; "
            f"found {len(matches)} exact block(s) and "
            f"{len(emitted_defines)} define line(s)"
        )
    return matches[0].group(0)


def extract_assignment() -> tuple[str, str, str]:
    source = RUNTIME_SOURCE.read_text(encoding="utf-8")
    defaults = "\n".join(extract_default(source, name) for name in GATES)
    init_body = extract_function(
        RUNTIME_SOURCE,
        "P4Wifi_Init",
        r"\bstatic\s+int\s+P4Wifi_Init\s*\(",
    )
    assignment_pattern = re.compile(
        r"(?m)^[ \t]*P4WifiBase->association_enabled[ \t]*=[ \t]*\r?\n"
        r"[ \t]*\(P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED[ \t]*&&[ \t]*\r?\n"
        r"[ \t]*P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED\)[ \t]*"
        r"\?[ \t]*TRUE[ \t]*:[ \t]*FALSE[ \t]*;"
    )
    assignments = list(assignment_pattern.finditer(init_body))
    occurrences = re.findall(r"P4WifiBase->association_enabled\s*=", init_body)
    if len(assignments) != 1 or len(occurrences) != 1:
        raise ValueError(
            "P4Wifi_Init must contain exactly one recognized multiline "
            "association_enabled assignment; source signature or expression changed"
        )
    assignment = assignments[0].group(0)

    owner_guard = re.compile(
        r"(?m)^#if P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED && !P4_WIFI_HOSTED_OWNER\r?\n"
        r"#error \"C6 verified association requires the persistent owner\"\r?\n"
        r"#endif(?:\r?\n|$)"
    )
    owner_guards = list(owner_guard.finditer(source))
    if len(owner_guards) != 1:
        raise ValueError(
            "runtime_probe.c must retain exactly one C6-verification/persistent-"
            f"owner #error guard; found {len(owner_guards)}"
        )

    makefile = NETWORK_MMAKEFILE.read_text(encoding="utf-8")
    mmake_blocks = "\n".join(audit_build_flag(makefile, name) for name in GATES)
    digest_material = "\n".join((defaults, assignment, owner_guards[0].group(0), mmake_blocks))
    return defaults, assignment, digest_material


def wrapper(defaults: str, assignment: str, expected: int, label: str) -> str:
    return f'''/* Generated from exact source blocks by test-wifi-association-gates.py. */
#include <stdio.h>
#include <stdlib.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0

{defaults}

struct WifiBase {{
    BOOL association_enabled;
}};

static BOOL extracted_library_init_assignment(void)
{{
    struct WifiBase base = {{ -7 }};
    struct WifiBase *P4WifiBase = &base;
    {assignment}
    return P4WifiBase->association_enabled;
}}

#define CHECK(condition) do {{ \\
    ++checks; \\
    if (!(condition)) {{ \\
        fprintf(stderr, "FAIL {label}: line %u: %s\\n", \\
                (unsigned int)__LINE__, #condition); \\
        return 1; \\
    }} \\
}} while (0)

static unsigned int checks;

int main(void)
{{
    int observed = extracted_library_init_assignment();
    CHECK(observed == {expected});
    CHECK(observed == 0 || observed == 1);
    printf("PASS: {label}, %u checks\\n", checks);
    return 0;
}}
'''


def main() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")

    defaults, assignment, digest_material = extract_assignment()
    print(
        "Extracted gate source SHA-256: "
        f"{sha256(digest_material.encode('utf-8'))}"
    )
    print(
        "Source audits: exact default-zero blocks, exact LibraryInit assignment, "
        "1-only mmake flags and persistent-owner #error guard: PASS"
    )
    print(
        "Scope: compiles only the extracted association_enabled assignment in a "
        "minimal base wrapper; build-flag and owner-guard checks are textual. "
        "The configuration values are synthetic test inputs, not attestations. "
        "No full initializer, resource, runtime, or hardware behavior is tested."
    )

    configurations = (
        ("defaults-unset", (), 0),
        ("C6-0-HOST-0", (0, 0), 0),
        ("C6-0-HOST-1", (0, 1), 0),
        ("C6-1-HOST-0", (1, 0), 0),
        ("C6-1-HOST-1", (1, 1), 1),
    )
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

    total_runs = 0
    total_checks = 0
    with tempfile.TemporaryDirectory(prefix="p4-wifi-association-gates-") as temporary:
        directory = Path(temporary)
        for optimization in (0, 2):
            optimization_checks = 0
            for label, values, expected in configurations:
                source_path = directory / f"association_gates_{label}.c"
                source_path.write_text(
                    wrapper(defaults, assignment, expected, label), encoding="utf-8"
                )
                binary = directory / f"association_gates_{label}_O{optimization}"
                command = compiler + [
                    "-std=c99",
                    f"-O{optimization}",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Wconversion",
                    "-Wshadow",
                    "-Wstrict-prototypes",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                ]
                if values:
                    command += [
                        f"-DP4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED={values[0]}",
                        f"-DP4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED={values[1]}",
                    ]
                command += [str(source_path), "-o", str(binary)]
                print(f"Building {label} at O{optimization} with ASan/UBSan")
                build = subprocess.run(
                    command,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=60,
                )
                if build.returncode != 0:
                    sys.stderr.write(build.stdout)
                    sys.stderr.write(build.stderr)
                    return build.returncode or 1
                if build.stdout:
                    sys.stdout.write(build.stdout)
                if build.stderr:
                    sys.stderr.write(build.stderr)

                try:
                    result = subprocess.run(
                        [str(binary)],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=20,
                        env=environment,
                    )
                except subprocess.TimeoutExpired:
                    print(f"FAIL: {label} O{optimization} timed out", file=sys.stderr)
                    return 1
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                if result.returncode != 0:
                    return result.returncode
                if not re.search(
                    rf"(?m)^PASS: {re.escape(label)}, 2 checks$", result.stdout
                ):
                    print(
                        f"FAIL: {label} O{optimization} omitted its check count",
                        file=sys.stderr,
                    )
                    return 1
                optimization_checks += 2
                total_checks += 2
                total_runs += 1
            print(
                f"PASS: {len(configurations)} configurations, "
                f"{optimization_checks} checks at O{optimization}"
            )

    print(f"PASS: {total_runs} sanitized runs, {total_checks} checks total")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
