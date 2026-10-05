#!/usr/bin/env python3
"""Merge upstream AROS into the checked-out AROS-NX branch.

AROS-NX owns its repository automation. Whatever upstream changes under
``.github/`` is discarded during the merge, with or without a conflict, and
listed in a report for review. A new upstream workflow would otherwise merge
cleanly and start running with this repository's runners and credentials.

Every conflict outside ``.github/`` stays fail-closed: the merge is aborted and
the working tree is left as it was before.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


OWNED_PREFIX = ".github"
CONFLICT_EXIT = 68


def _git(*args: str, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=check,
    )


def _owned(path: str) -> bool:
    return path == OWNED_PREFIX or path.startswith(f"{OWNED_PREFIX}/")


def _unmerged_paths(cwd: Path | None) -> list[str]:
    output = _git("diff", "--name-only", "--diff-filter=U", "-z", cwd=cwd).stdout
    return sorted({path for path in output.split("\0") if path})


def _dropped_report(upstream: str, cwd: Path | None) -> str:
    base = _git("merge-base", "HEAD", upstream, cwd=cwd).stdout.strip()
    changes = _git(
        "diff", "--name-status", "--no-renames", base, upstream, "--", OWNED_PREFIX, cwd=cwd
    ).stdout.strip()
    if not changes:
        return ""
    commits = _git(
        "log", "--format=%h %s", f"{base}..{upstream}", "--", OWNED_PREFIX, cwd=cwd
    ).stdout.strip()
    lines = [
        f"Upstream changes under `{OWNED_PREFIX}/` were discarded; AROS-NX owns its automation.",
        "Port anything useful into AROS-NX CI deliberately.",
        "",
        "Paths:",
        "",
    ]
    for change in changes.splitlines():
        status, _, path = change.partition("\t")
        lines.append(f"- `{status}` `{path}`")
    lines += ["", "Upstream commits:", ""]
    lines += [f"- {commit}" for commit in commits.splitlines()]
    return "\n".join(lines) + "\n"


def merge(upstream: str, cwd: Path | None = None) -> tuple[int, str]:
    """Merge ``upstream`` into HEAD, keeping HEAD's ``.github/`` tree.

    Returns the exit status and a message: the Markdown report of discarded
    upstream automation on success, the blocking conflicts otherwise.
    """
    report = _dropped_report(upstream, cwd)
    _git("merge", "--no-ff", "--no-commit", upstream, cwd=cwd, check=False)
    if not _git("rev-parse", "-q", "--verify", "MERGE_HEAD", cwd=cwd, check=False).stdout.strip():
        # Already contained or refused before starting: nothing to integrate.
        return 1, "git did not start a merge; upstream may already be contained in HEAD"

    blocking = [path for path in _unmerged_paths(cwd) if not _owned(path)]
    if blocking:
        _git("merge", "--abort", cwd=cwd, check=False)
        return CONFLICT_EXIT, "\n".join(blocking)

    # Reset the owned tree to HEAD exactly: drop upstream additions, keep
    # files AROS-NX deleted deleted, and resolve owned conflicts to HEAD.
    _git("rm", "-r", "-q", "-f", "--ignore-unmatch", "--", OWNED_PREFIX, cwd=cwd)
    if _git("ls-tree", "-d", "HEAD", OWNED_PREFIX, cwd=cwd).stdout.strip():
        _git("checkout", "HEAD", "--", OWNED_PREFIX, cwd=cwd)

    remaining = _unmerged_paths(cwd)
    owned_diff = _git("diff", "--cached", "--name-only", "HEAD", "--", OWNED_PREFIX, cwd=cwd).stdout.strip()
    if remaining or owned_diff:
        _git("merge", "--abort", cwd=cwd, check=False)
        return CONFLICT_EXIT, "\n".join(remaining or owned_diff.splitlines())

    _git("commit", "--no-edit", "--quiet", cwd=cwd)
    return 0, report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("upstream", help="upstream commit to merge into HEAD")
    parser.add_argument("--report", type=Path, required=True, help="Markdown file for discarded automation")
    args = parser.parse_args(argv)

    status, message = merge(args.upstream)
    if status == CONFLICT_EXIT:
        print("::error::Upstream does not merge cleanly into AROS-NX main.", file=sys.stderr)
        for path in message.splitlines():
            print(f"Conflict: {path}", file=sys.stderr)
        return status
    if status:
        print(f"::error::{message}", file=sys.stderr)
        return status
    args.report.write_text(message, encoding="utf-8")
    if message:
        print(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
