from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upstream_merge


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return result.stdout.decode("utf-8").strip()


def write(repo: Path, files: dict[str, str | None]) -> None:
    for relative_path, contents in files.items():
        path = repo / relative_path
        if contents is None:
            path.unlink()
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")


def commit(repo: Path, files: dict[str, str | None], message: str) -> str:
    write(repo, files)
    git(repo, "add", "--all")
    git(repo, "commit", "-m", message)
    return git(repo, "rev-parse", "HEAD")


class UpstreamMergeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.repo = Path(self._temporary.name)
        git(self.repo, "init", "--initial-branch=main")
        git(self.repo, "config", "user.name", "Upstream Merge Test")
        git(self.repo, "config", "user.email", "upstream-merge@example.invalid")
        commit(
            self.repo,
            {
                ".github/workflows/upstream.yml": "name: upstream\n",
                ".github/workflows/ci.yml": "name: ci\n",
                "rom/source.c": "int a;\n",
                "rom/other.c": "int b;\n",
            },
            "base",
        )
        git(self.repo, "branch", "upstream")

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def upstream(self, files: dict[str, str | None]) -> str:
        git(self.repo, "switch", "-q", "upstream")
        head = commit(self.repo, files, "upstream change")
        git(self.repo, "switch", "-q", "main")
        return head

    def test_owned_tree_stays_exactly_as_in_main(self) -> None:
        commit(
            self.repo,
            {".github/workflows/upstream.yml": None, ".github/workflows/ci.yml": "name: nx\n"},
            "nx owns automation",
        )
        main_tree = git(self.repo, "rev-parse", "HEAD:.github")
        upstream = self.upstream(
            {
                ".github/workflows/upstream.yml": "name: upstream v2\n",
                ".github/workflows/ci.yml": "name: upstream ci\n",
                ".github/workflows/new.yml": "name: new\n",
                "rom/source.c": "int a = 1;\n",
            }
        )

        status, report = upstream_merge.merge(upstream, cwd=self.repo)

        self.assertEqual(status, 0, report)
        self.assertEqual(git(self.repo, "rev-parse", "HEAD:.github"), main_tree)
        self.assertEqual((self.repo / "rom/source.c").read_text(), "int a = 1;\n")
        self.assertEqual(git(self.repo, "rev-list", "--parents", "-n1", "HEAD").count(" "), 2)
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        for path in ("upstream.yml", "ci.yml", "new.yml"):
            self.assertIn(f".github/workflows/{path}", report)

    def test_upstream_without_automation_changes_has_empty_report(self) -> None:
        upstream = self.upstream({"rom/source.c": "int a = 2;\n"})

        status, report = upstream_merge.merge(upstream, cwd=self.repo)

        self.assertEqual(status, 0, report)
        self.assertEqual(report, "")

    def test_conflict_outside_owned_tree_aborts_and_restores(self) -> None:
        main_head = commit(self.repo, {"rom/source.c": "int a = 3;\n"}, "nx change")
        upstream = self.upstream(
            {"rom/source.c": "int a = 4;\n", ".github/workflows/upstream.yml": "name: changed\n"}
        )

        status, conflicts = upstream_merge.merge(upstream, cwd=self.repo)

        self.assertEqual(status, upstream_merge.CONFLICT_EXIT)
        self.assertEqual(conflicts, "rom/source.c")
        self.assertEqual(git(self.repo, "rev-parse", "HEAD"), main_head)
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")
        self.assertFalse((self.repo / ".git/MERGE_HEAD").exists())

    def test_contained_upstream_is_refused(self) -> None:
        status, _ = upstream_merge.merge(git(self.repo, "rev-parse", "upstream"), cwd=self.repo)

        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()
