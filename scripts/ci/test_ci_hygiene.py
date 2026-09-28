from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_hygiene


SCRIPT = Path(__file__).resolve().with_name("ci_hygiene.py")


def git(repo: Path, *args: str, input_bytes: bytes | None = None) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return result.stdout.decode("utf-8").strip()


def initialize_repo(repo: Path) -> None:
    git(repo, "init", "--initial-branch=main")
    git(repo, "config", "user.name", "CI Hygiene Test")
    git(repo, "config", "user.email", "ci-hygiene@example.invalid")


def commit(repo: Path, message: str) -> str:
    git(repo, "add", "--all")
    git(repo, "commit", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def pull_request_merge(repo: Path, files: dict[str, bytes], message: str) -> tuple[str, str]:
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "checkout", "-b", "change")
    for relative_path, contents in files.items():
        path = repo / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    commit(repo, f"change: {message}")
    git(repo, "checkout", "main")
    git(repo, "merge", "--no-ff", "change", "-m", f"Merge: {message}")
    return base, git(repo, "rev-parse", "HEAD")


def invoke_plan(repo: Path, event_name: str, head: str, output: Path, **env_values: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for name in ("GITHUB_EVENT_PATH", "GITHUB_REF", "PUSH_BEFORE_SHA", "PULL_REQUEST_BASE_SHA"):
        env.pop(name, None)
    env.update(
        {
            "GITHUB_EVENT_NAME": event_name,
            "GITHUB_SHA": head,
            "GITHUB_OUTPUT": str(output),
            **env_values,
        }
    )
    return subprocess.run(
        [sys.executable, str(SCRIPT), "plan"],
        cwd=repo,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


class WindowsPathValidationTests(unittest.TestCase):
    def test_rejects_invalid_characters_trailing_space_and_device_names(self) -> None:
        issues = ci_hygiene._path_issues(
            ["bad:name.c", "control\x7f.c", "folder/ends-with. ", "CON.txt", "subdir/com1.log", "LPT³.data"]
        )
        joined = "\n".join(issues)
        self.assertIn("Windows-invalid character", joined)
        self.assertIn("ends with a space or period", joined)
        self.assertIn("reserved Windows device name", joined)

    def test_valid_counterprobes_do_not_trigger_false_positives(self) -> None:
        self.assertEqual(
            ci_hygiene._path_issues(
                ["CONSOLE.txt", "COM10.txt", "LPT0", "notes.md", "dir.with.dot/file"]
            ),
            [],
        )

    def test_detects_case_insensitive_file_and_directory_collisions(self) -> None:
        issues = ci_hygiene._path_issues(
            ["Readme.md", "README.md", "Assets/icon.png", "assets/logo.png"]
        )
        collisions = [issue for issue in issues if issue.startswith("case-insensitive")]
        self.assertEqual(len(collisions), 2)

    def test_paths_command_reads_nul_delimited_tracked_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo = Path(temporary_directory)
            initialize_repo(repo)
            blob = git(repo, "hash-object", "-w", "--stdin", input_bytes=b"content")
            git(repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},NUL.txt")
            git(repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},COM10.txt")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "paths"],
                cwd=repo,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("reserved Windows device name", result.stdout)
            self.assertNotIn("COM10.txt", result.stdout)


class MatrixPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary_directory.name)
        initialize_repo(self.repo)
        (self.repo / "README.md").write_bytes(b"base\n")
        self.base = commit(self.repo, "base")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def event_file(self, name: str, payload: dict[str, object]) -> Path:
        path = self.repo / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def output_file(self, name: str = "github-output") -> Path:
        path = self.repo / name
        path.write_text("", encoding="utf-8")
        return path

    def test_only_markdown_changes_on_a_pull_request_skip_the_matrix(self) -> None:
        base, head = pull_request_merge(self.repo, {"docs/guide.md": b"guide\n"}, "docs")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": base}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=false", output.read_text(encoding="utf-8"))

    def test_source_change_on_a_pull_request_runs_the_full_matrix(self) -> None:
        base, head = pull_request_merge(self.repo, {"src/main.c": b"int main(void) { return 0; }\n"}, "source")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": base}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=true", output.read_text(encoding="utf-8"))

    def test_markdown_counterprobes_and_empty_diff_run_the_full_matrix(self) -> None:
        cases = [
            ({"docs/guide.mdx": b"guide\n"}, "mdx"),
            ({"docs/guide.md": b"guide\n", "src/main.c": b"source\n"}, "mixed"),
            ({}, "empty"),
        ]
        for changes, label in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as case_directory:
                case_repo = Path(case_directory)
                initialize_repo(case_repo)
                (case_repo / "README.md").write_bytes(b"base\n")
                commit(case_repo, "base")
                if changes:
                    base, head = pull_request_merge(case_repo, changes, label)
                else:
                    base = git(case_repo, "rev-parse", "HEAD")
                    head = base
                event = case_repo / "event.json"
                event.write_text(json.dumps({"pull_request": {"base": {"sha": base}}}), encoding="utf-8")
                output = case_repo / "output"
                output.write_text("", encoding="utf-8")
                result = invoke_plan(case_repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("full_matrix=true", output.read_text(encoding="utf-8"))

    def test_uncertain_pr_history_fails_closed(self) -> None:
        _, head = pull_request_merge(self.repo, {"docs/guide.md": b"guide\n"}, "docs")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": "f" * 40}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=true", output.read_text(encoding="utf-8"))

    def test_main_push_skips_after_a_protected_pr_merge(self) -> None:
        parent = git(self.repo, "rev-parse", "HEAD")
        (self.repo / "src").mkdir()
        (self.repo / "src/main.c").write_bytes(b"source\n")
        head = commit(self.repo, "merged source")
        event = self.event_file("event.json", {"before": parent})
        output = self.output_file()

        result = invoke_plan(
            self.repo,
            "push",
            head,
            output,
            GITHUB_EVENT_PATH=str(event),
            GITHUB_REF="refs/heads/main",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=false", output.read_text(encoding="utf-8"))

    def test_main_push_with_missing_history_fails_closed(self) -> None:
        output = self.output_file()
        result = invoke_plan(
            self.repo,
            "push",
            self.base,
            output,
            GITHUB_REF="refs/heads/main",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=true", output.read_text(encoding="utf-8"))

    def test_line_ending_warning_escapes_github_command_values(self) -> None:
        path_name = "bad%,:\nname.c"
        base, head = pull_request_merge(self.repo, {path_name: b"line\r\nwith bare\rend\n"}, "line endings")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": base}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning file=bad%25%2C%3A%0Aname.c,title=C/H line endings::", result.stdout)
        self.assertIn("1 CRLF sequence(s)", result.stdout)
        self.assertIn("1 bare CR byte(s)", result.stdout)

    def test_line_ending_check_does_not_follow_a_symlink(self) -> None:
        base = git(self.repo, "rev-parse", "HEAD")
        git(self.repo, "checkout", "-b", "change")
        (self.repo / "target.txt").write_bytes(b"line\r\n")
        (self.repo / "linked.c").symlink_to("target.txt")
        commit(self.repo, "add symlink")
        git(self.repo, "checkout", "main")
        git(self.repo, "merge", "--no-ff", "change", "-m", "Merge symlink")
        head = git(self.repo, "rev-parse", "HEAD")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": base}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("::warning", result.stdout)
        self.assertIn("Checked 0 changed C/H file(s)", result.stdout)

    def test_workflow_dispatch_always_runs_the_full_matrix(self) -> None:
        head = git(self.repo, "rev-parse", "HEAD")
        output = self.output_file()

        result = invoke_plan(self.repo, "workflow_dispatch", head, output)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("full_matrix=true", output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
