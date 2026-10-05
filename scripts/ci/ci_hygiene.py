#!/usr/bin/env python3
"""Plan AROS-NX product CI and check tracked paths for Windows compatibility."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Sequence


ZERO_SHA = "0" * 40
OID_RE = re.compile(r"^[0-9a-fA-F]{40,64}$")
WINDOWS_FORBIDDEN = set('<>:"\\|?*')
RESERVED_DEVICE_RE = re.compile(r"^(?:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$", re.IGNORECASE)


def _git(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["git", *args],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def _is_commit(oid: str, cwd: Path | None = None) -> bool:
    if not OID_RE.fullmatch(oid):
        return False
    result = _git("cat-file", "-e", f"{oid}^{{commit}}", cwd=cwd)
    return result.returncode == 0


def _event_payload(env: Mapping[str, str]) -> dict[str, object] | None:
    event_path = env.get("GITHUB_EVENT_PATH")
    if not event_path:
        return None
    try:
        with open(event_path, "r", encoding="utf-8") as event_file:
            payload = json.load(event_file)
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def _pull_request_base(payload: dict[str, object] | None, env: Mapping[str, str]) -> str:
    if payload is not None:
        pull_request = payload.get("pull_request")
        if isinstance(pull_request, dict):
            base = pull_request.get("base")
            if isinstance(base, dict) and isinstance(base.get("sha"), str):
                return base["sha"]
    return env.get("PULL_REQUEST_BASE_SHA", "")


def _push_before(payload: dict[str, object] | None, env: Mapping[str, str]) -> str:
    if payload is not None and isinstance(payload.get("before"), str):
        return payload["before"]
    return env.get("PUSH_BEFORE_SHA", "")


def _commit_parents(oid: str, cwd: Path | None = None) -> list[str] | None:
    result = _git("rev-list", "--parents", "-n", "1", oid, cwd=cwd)
    if result.returncode != 0:
        return None
    fields = result.stdout.decode("ascii", errors="strict").strip().split()
    if not fields or fields[0].lower() != oid.lower():
        return None
    return fields[1:]


def _diff_paths(base: str, head: str, cwd: Path | None = None) -> list[str] | None:
    result = _git(
        "diff",
        "--no-ext-diff",
        "--no-renames",
        "--name-only",
        "-z",
        base,
        head,
        "--",
        cwd=cwd,
    )
    if result.returncode != 0:
        return None
    return [os.fsdecode(path) for path in result.stdout.split(b"\0") if path]


def _resolve_diff(
    event_name: str,
    env: Mapping[str, str],
    cwd: Path | None = None,
) -> tuple[list[str] | None, bool]:
    """Return changed paths and whether the range is reliable for matrix gating."""
    payload = _event_payload(env)
    head = env.get("GITHUB_SHA", "")
    if not _is_commit(head, cwd=cwd):
        return None, False

    if event_name == "pull_request":
        base = _pull_request_base(payload, env)
        if not _is_commit(base, cwd=cwd):
            return None, False
        parents = _commit_parents(head, cwd=cwd)
        paths = _diff_paths(base, head, cwd=cwd)
        if paths is None:
            return None, False
        # pull_request workflows test GitHub's synthetic merge commit. Its first
        # parent must be the PR base; otherwise the comparison is uncertain.
        reliable = bool(parents and parents[0].lower() == base.lower())
        return paths, reliable

    if event_name == "push":
        before = _push_before(payload, env)
        if not before or before == ZERO_SHA or not _is_commit(before, cwd=cwd):
            return None, False
        paths = _diff_paths(before, head, cwd=cwd)
        return paths, paths is not None

    if event_name == "workflow_dispatch":
        parents = _commit_parents(head, cwd=cwd)
        if not parents:
            return None, False
        paths = _diff_paths(parents[0], head, cwd=cwd)
        return paths, paths is not None

    return None, False


def _escape_command_value(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _escape_command_property(value: str) -> str:
    return _escape_command_value(value).replace(":", "%3A").replace(",", "%2C")


def _has_symlink_component(relative_path: str, root: Path | None = None) -> bool:
    current = Path.cwd() if root is None else root
    for component in relative_path.split("/"):
        if component in ("", ".", ".."):
            return True
        current = current / component
        try:
            if current.is_symlink():
                return True
        except OSError:
            return True
    return False


def _report_line_endings(paths: Sequence[str], cwd: Path | None = None) -> None:
    root = Path.cwd() if cwd is None else cwd
    checked = 0
    findings = 0
    for path in paths:
        if Path(path).suffix.casefold() not in {".c", ".h"}:
            continue
        if _has_symlink_component(path, root=root):
            continue
        source = root / path
        try:
            if not source.is_file():
                continue
            data = source.read_bytes()
        except OSError:
            continue

        checked += 1
        crlf_count = data.count(b"\r\n")
        bare_cr_count = data.replace(b"\r\n", b"").count(b"\r")
        if not crlf_count and not bare_cr_count:
            continue

        findings += 1
        details = []
        if crlf_count:
            details.append(f"{crlf_count} CRLF sequence(s)")
        if bare_cr_count:
            details.append(f"{bare_cr_count} bare CR byte(s)")
        message = (
            f"{path} contains {', '.join(details)}; CONTRIBUTING.md asks for "
            "Unix (LF) line endings. This check is advisory and does not modify the file."
        )
        print(
            "::warning "
            f"file={_escape_command_property(path)},title=C/H line endings"
            f"::{_escape_command_value(message)}"
        )

    print(f"Checked {checked} changed C/H file(s); reported {findings} line-ending issue(s).")


def _write_matrix_output(full_matrix: bool, env: Mapping[str, str]) -> bool:
    value = f"full_matrix={'true' if full_matrix else 'false'}\n"
    output_path = env.get("GITHUB_OUTPUT")
    if not output_path:
        sys.stdout.write(value)
        return True
    try:
        with open(output_path, "a", encoding="utf-8", newline="\n") as output_file:
            output_file.write(value)
    except OSError:
        print("Could not write the matrix plan to GITHUB_OUTPUT.", file=sys.stderr)
        return False
    print(value, end="")
    return True


def plan(env: Mapping[str, str] | None = None, cwd: Path | None = None) -> int:
    env = os.environ if env is None else env
    event_name = env.get("GITHUB_EVENT_NAME", "")
    changed_paths, reliable_history = _resolve_diff(event_name, env, cwd=cwd)

    # A protected merge to main has already passed the PR qualification matrix.
    # Require a reliable push range before applying that optimization.
    if (
        event_name == "push"
        and env.get("GITHUB_REF") == "refs/heads/main"
        and reliable_history
    ):
        full_matrix = False
    elif (
        event_name == "pull_request"
        and reliable_history
        and changed_paths
        and all(path.endswith(".md") for path in changed_paths)
    ):
        full_matrix = False
    else:
        full_matrix = True

    if changed_paths is None:
        print("Could not determine changed paths from reliable Git history.", file=sys.stderr)
        _report_line_endings((), cwd=cwd)
    else:
        if not reliable_history and event_name == "pull_request":
            print("Pull request history is uncertain; using the full matrix.", file=sys.stderr)
        _report_line_endings(changed_paths, cwd=cwd)

    if not _write_matrix_output(full_matrix, env):
        return 2
    return 0


def _path_issues(paths: Sequence[str]) -> list[str]:
    issues: list[str] = []
    seen_issues: set[str] = set()
    unique_paths = list(dict.fromkeys(paths))
    prefix_spellings: dict[tuple[str, ...], tuple[tuple[str, ...], str]] = {}

    for path in unique_paths:
        components = path.split("/")
        for component in components:
            invalid_characters = sorted(
                {
                    char
                    for char in component
                    if char in WINDOWS_FORBIDDEN
                    or ord(char) < 32
                    or ord(char) == 127
                    or 0xD800 <= ord(char) <= 0xDFFF
                }
            )
            if invalid_characters:
                issue = f"{path!r}: component {component!r} contains Windows-invalid character(s)"
                if issue not in seen_issues:
                    issues.append(issue)
                    seen_issues.add(issue)
            if component.endswith((" ", ".")):
                issue = f"{path!r}: component {component!r} ends with a space or period"
                if issue not in seen_issues:
                    issues.append(issue)
                    seen_issues.add(issue)
            device_stem = component.split(".", 1)[0].rstrip(" .")
            if RESERVED_DEVICE_RE.fullmatch(device_stem):
                issue = f"{path!r}: component {component!r} is a reserved Windows device name"
                if issue not in seen_issues:
                    issues.append(issue)
                    seen_issues.add(issue)

        spellings = tuple(components)
        folded = tuple(component.casefold() for component in components)
        for length in range(1, len(components) + 1):
            folded_prefix = folded[:length]
            spelling_prefix = spellings[:length]
            existing = prefix_spellings.get(folded_prefix)
            if existing is None:
                prefix_spellings[folded_prefix] = (spelling_prefix, path)
            elif existing[0] != spelling_prefix:
                other_path = existing[1]
                pair = tuple(sorted((other_path, path)))
                issue = f"case-insensitive path collision: {pair[0]!r} and {pair[1]!r}"
                if issue not in seen_issues:
                    issues.append(issue)
                    seen_issues.add(issue)
                break

    return issues


def paths(cwd: Path | None = None) -> int:
    result = _git("ls-files", "-z", cwd=cwd)
    if result.returncode != 0:
        print("Could not list tracked Git paths.", file=sys.stderr)
        return 2
    tracked_paths = [os.fsdecode(path) for path in result.stdout.split(b"\0") if path]
    issues = _path_issues(tracked_paths)
    if issues:
        for issue in issues:
            print(f"ERROR {issue}")
        print(f"Found {len(issues)} Windows-incompatible tracked path issue(s).")
        return 1
    print(f"All {len(set(tracked_paths))} tracked path(s) are Windows-compatible.")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv == ["plan"]:
        return plan()
    if argv == ["paths"]:
        return paths()
    print("Usage: ci_hygiene.py {plan|paths}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
