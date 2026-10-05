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


def read_plan(output: Path) -> tuple[str, list[tuple[str, str]]]:
    """The scope and the (host, preset) pairs a plan wrote."""
    values = dict(
        line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines() if "=" in line
    )
    matrix = json.loads(values["matrix"])
    return values["scope"], [(entry["host"], entry["preset"]) for entry in matrix["include"]]


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

    def plan_pull_request(self, files: dict[str, bytes], **pull_request: object) -> tuple[str, list[tuple[str, str]]]:
        base, head = pull_request_merge(self.repo, files, "change")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": base}, **pull_request}})
        output = self.output_file()
        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))
        self.assertEqual(result.returncode, 0, result.stderr)
        return read_plan(output)

    def plan_main_push(self, files: dict[str, bytes]) -> tuple[str, list[tuple[str, str]]]:
        parent = git(self.repo, "rev-parse", "HEAD")
        for relative_path, contents in files.items():
            path = self.repo / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        head = commit(self.repo, "merged change")
        event = self.event_file("event.json", {"before": parent})
        output = self.output_file()
        result = invoke_plan(
            self.repo, "push", head, output, GITHUB_EVENT_PATH=str(event), GITHUB_REF="refs/heads/main"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return read_plan(output)

    def test_only_markdown_changes_on_a_pull_request_skip_the_matrix(self) -> None:
        scope, matrix = self.plan_pull_request({"docs/guide.md": b"guide\n"})

        self.assertEqual(scope, "none")
        self.assertEqual(matrix, [])

    def test_target_source_change_on_a_pull_request_builds_every_preset_on_one_host(self) -> None:
        scope, matrix = self.plan_pull_request({"src/main.c": b"int main(void) { return 0; }\n"})

        self.assertEqual(scope, "pull-request")
        self.assertEqual(
            matrix,
            [("linux-x86_64", "pc-x86_64"), ("linux-x86_64", "arm-raspi"), ("linux-x86_64", "rpi-aarch64")],
        )

    def test_draft_pull_request_builds_nothing(self) -> None:
        scope, matrix = self.plan_pull_request({"rom/exec/x.c": b"source\n"}, draft=True)

        self.assertEqual(scope, "none")
        self.assertEqual(matrix, [])

    def test_ready_pull_request_is_planned_by_its_paths(self) -> None:
        scope, _ = self.plan_pull_request({"rom/exec/x.c": b"source\n"}, draft=False)

        self.assertEqual(scope, "pull-request")

    def test_unbuilt_architectures_skip_the_matrix(self) -> None:
        scope, matrix = self.plan_pull_request(
            {
                "arch/riscv-esp32p4/kernel/x.c": b"source\n",
                "arch/m68k-amiga/y.c": b"source\n",
                "arch/riscv64-opensbi/z.c": b"source\n",
                "arch/ppc-chrp/w.c": b"source\n",
                "arch/riscv-esp32p4/README.md": b"notes\n",
            }
        )

        self.assertEqual(scope, "none")
        self.assertEqual(matrix, [])

    def test_unbuilt_architecture_counterprobes_are_built(self) -> None:
        cases = [
            ({"arch/riscv-esp32p4/x.c": b"source\n", "rom/exec/y.c": b"source\n"}, "with shared code"),
            ({"arch/arm-raspi/x.c": b"source\n"}, "built arm"),
            ({"arch/i386-pc/x.c": b"source\n"}, "i386 parts of pc-x86_64"),
            ({"arch/all-native/x.c": b"source\n"}, "shared all-*"),
            ({"arch/mmakefile.src": b"rules\n"}, "arch top level"),
            ({"rom/riscv-notes/x.c": b"source\n"}, "riscv outside arch"),
        ]
        for files, label in cases:
            with self.subTest(label=label):
                # a fresh repository per case; unittest cleans up the last
                self.tearDown()
                self.setUp()
                scope, _ = self.plan_pull_request(files)
                self.assertEqual(scope, "pull-request")

    def test_build_host_changes_run_the_full_matrix(self) -> None:
        cases = [
            {"tools/genmodule/x.c": b"source\n"},
            {"config/make.tmpl": b"rules\n"},
            {"scripts/ci/x.py": b"script\n"},
            {".github/workflows/x.yml": b"workflow\n"},
            {"configure": b"script\n"},
            {"configure.in": b"script\n"},
            {"acinclude.m4": b"macros\n"},
            {"aros-toolchains.lock.toml": b"lock\n"},
            {"rom/exec/x.c": b"source\n", "tools/y.c": b"source\n"},
        ]
        for files in cases:
            with self.subTest(files=sorted(files)):
                # a fresh repository per case; unittest cleans up the last
                self.tearDown()
                self.setUp()
                scope, matrix = self.plan_pull_request(files)
                self.assertEqual(scope, "full")
                self.assertEqual(len(matrix), 9)

    def test_build_host_counterprobes_stay_on_one_host(self) -> None:
        cases = [
            {"rom/config/x.c": b"source\n"},
            {"workbench/tools/x.c": b"source\n"},
            {"configure.local": b"text\n"},
            {"arch/x86_64-pc/mmakefile": b"rules\n"},
        ]
        for files in cases:
            with self.subTest(files=sorted(files)):
                # a fresh repository per case; unittest cleans up the last
                self.tearDown()
                self.setUp()
                scope, _ = self.plan_pull_request(files)
                self.assertEqual(scope, "pull-request")

    def test_markdown_counterprobes_and_empty_diff_are_built(self) -> None:
        cases = [
            ({"docs/guide.mdx": b"guide\n"}, "mdx", "pull-request"),
            ({"docs/guide.md": b"guide\n", "src/main.c": b"source\n"}, "mixed", "pull-request"),
            ({}, "empty", "full"),
        ]
        for changes, label, expected in cases:
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
                self.assertEqual(read_plan(output)[0], expected)

    def test_uncertain_pr_history_fails_closed(self) -> None:
        _, head = pull_request_merge(self.repo, {"docs/guide.md": b"guide\n"}, "docs")
        event = self.event_file("event.json", {"pull_request": {"base": {"sha": "f" * 40}}})
        output = self.output_file()

        result = invoke_plan(self.repo, "pull_request", head, output, GITHUB_EVENT_PATH=str(event))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(read_plan(output)[0], "full")

    def test_main_push_builds_target_sources_on_the_remaining_hosts(self) -> None:
        scope, matrix = self.plan_main_push({"src/main.c": b"source\n"})

        self.assertEqual(scope, "remaining")
        self.assertEqual(len(matrix), 6)
        self.assertNotIn("linux-x86_64", {host for host, _ in matrix})
        self.assertEqual({preset for _, preset in matrix}, {"pc-x86_64", "arm-raspi", "rpi-aarch64"})

    def test_main_push_skips_what_the_pull_request_qualified_fully_or_nobody_builds(self) -> None:
        for files in ({"tools/genmodule/x.c": b"source\n"}, {"arch/riscv-esp32p4/x.c": b"source\n"}, {"docs/x.md": b"text\n"}):
            with self.subTest(files=sorted(files)):
                # a fresh repository per case; unittest cleans up the last
                self.tearDown()
                self.setUp()
                scope, matrix = self.plan_main_push(files)
                self.assertEqual(scope, "none")
                self.assertEqual(matrix, [])

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
        self.assertEqual(read_plan(output)[0], "full")

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
        scope, matrix = read_plan(output)
        self.assertEqual(scope, "full")
        self.assertEqual(len(matrix), 9)


if __name__ == "__main__":
    unittest.main()
