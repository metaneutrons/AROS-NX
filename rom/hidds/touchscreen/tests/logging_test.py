#!/usr/bin/env python3
"""Preprocess the real HIDD sources in both diagnostics configurations."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]
TOUCH = ROOT / "rom/hidds/touchscreen"
SOURCES = {
    "worker": TOUCH / "touch_worker.c",
    "files": TOUCH / "touch_files.c",
    "class": TOUCH / "touchscreen_class.c",
}


def external_includes(paths):
    """Return non-local headers used by the selected files and local headers."""
    pending = list(paths)
    seen = set()
    external = set()
    include_re = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)

    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        for delimiter, name in include_re.findall(path.read_text()):
            if delimiter == '"':
                local = path.parent / name
                if local.is_file():
                    pending.append(local)
            elif name not in {"stdint.h", "stddef.h"}:
                external.add(name)
    return external


def preprocess(source, include_dir, diagnostics, edge_trace=1):
    command = [
        "cc", "-E", "-P", "-x", "c",
        f"-DTOUCHSCREEN_DIAGNOSTICS={diagnostics}",
        f"-DTOUCHSCREEN_EDGE_TRACE={edge_trace}",
        "-I", str(include_dir), str(source),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(
            f"preprocessor failed for {source.name}, diagnostics={diagnostics}:\n"
            f"{result.stderr}"
        )
    return result.stdout


def remove_bug_calls(source):
    """Remove bug() statements, also removing a bare if that only guards one."""
    spans = []
    for match in re.finditer(r"\bbug\s*\(", source):
        opening = source.find("(", match.start())
        depth = 0
        quote = None
        escaped = False
        closing = None
        for index in range(opening, len(source)):
            char = source[index]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
                continue
            if char in {'"', "'"}:
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    closing = index
                    break
        if closing is None:
            raise AssertionError("unterminated bug() call in preprocessed source")
        end = closing + 1
        while end < len(source) and source[end].isspace():
            end += 1
        if end < len(source) and source[end] == ";":
            end += 1

        start = match.start()
        before = start
        while before > 0 and source[before - 1].isspace():
            before -= 1
        if before > 0 and source[before - 1] == ")":
            condition_close = before - 1
            condition_depth = 0
            condition_open = None
            for index in range(condition_close, -1, -1):
                if source[index] == ")":
                    condition_depth += 1
                elif source[index] == "(":
                    condition_depth -= 1
                    if condition_depth == 0:
                        condition_open = index
                        break
            if condition_open is not None:
                prefix = source[:condition_open].rstrip()
                if re.search(r"\bif$", prefix):
                    spans.append((len(prefix) - 2, end))
                    continue
        spans.append((start, end))

    for start, end in reversed(spans):
        source = source[:start] + source[end:]
    return re.sub(r"\s+", " ", source).strip()


def section(source, start_marker, end_marker=None):
    start = source.index(start_marker)
    end = source.index(end_marker, start) if end_marker else len(source)
    return source[start:end]


class TouchscreenLoggingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="touchscreen-logging-")
        cls.include_dir = Path(cls.temp.name)
        for header in external_includes(
            [*SOURCES.values(), TOUCH / "touchscreen_intern.h"]
        ):
            stub = cls.include_dir / header
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.touch()

        cls.preprocessed = {}
        for diagnostics in (0, 1):
            cls.preprocessed[diagnostics] = {
                name: preprocess(path, cls.include_dir, diagnostics)
                for name, path in SOURCES.items()
            }

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_routine_logs_are_switchable_and_failures_remain(self):
        off = "\n".join(self.preprocessed[0].values())
        on = "\n".join(self.preprocessed[1].values())
        for text in (
            "settings %lu: mode",
            "event %lu type",
            "multi frame",
            "heartbeat after",
            "worker exiting after",
            "prefs %s, %ld bytes",
            "default calibration",
            "disposed; worker",
        ):
            with self.subTest(diagnostic=text):
                self.assertNotIn(text, off)
                self.assertIn(text, on)

        for text in (
            "read error",
            "recovery limit exhausted",
            "recovery %lu passed",
            "invalid calibration ignored",
            "cannot watch",
            "controller start failed",
            " started, raw",
        ):
            with self.subTest(always_enabled=text):
                self.assertIn(text, off)
                self.assertIn(text, on)

        for diagnostics in (0, 1):
            worker = preprocess(
                SOURCES["worker"], self.include_dir, diagnostics, edge_trace=1
            )
            self.assertIn("edge sample", worker)

    def test_non_logging_touch_behavior_is_identical(self):
        pairs = (
            ("worker", "static VOID touch_event(", "struct TouchEmitter"),
            ("worker", "static void touch_refresh(", "static ULONG touch_distance"),
            ("files", "static void touch_apply_prefs(", "void touch_files_poll"),
            ("class", "OOP_Object *Touch__Root__New(", "VOID Touch__Root__Dispose"),
            ("class", "VOID Touch__Root__Dispose(", "VOID Touch__Root__Get"),
            ("class", "VOID Touch__Root__Set(", None),
        )
        for name, start, end in pairs:
            with self.subTest(function=start):
                off = remove_bug_calls(
                    section(self.preprocessed[0][name], start, end)
                )
                on = remove_bug_calls(
                    section(self.preprocessed[1][name], start, end)
                )
                self.assertEqual(off, on)

        worker = remove_bug_calls(
            section(self.preprocessed[0]["worker"], "static VOID touch_event(",
                    "struct TouchEmitter")
        )
        self.assertIn("++data->published_events;", worker)
        self.assertIn("data->callback(data->callbackdata, &event);", worker)
        refresh = remove_bug_calls(
            section(self.preprocessed[0]["worker"], "static void touch_refresh(",
                    "static ULONG touch_distance")
        )
        self.assertIn("touch_policy_set_params(policy, &params);", refresh)
        self.assertIn("touch_policy_set_mode(policy, mode);", refresh)
        prefs = remove_bug_calls(
            section(self.preprocessed[0]["files"], "static void touch_apply_prefs(",
                    "void touch_files_poll")
        )
        self.assertIn("data->settings.calibration.x_min = prefs.cal.x_min;", prefs)
        self.assertIn("data->settings.params = prefs.params;", prefs)
        self.assertIn("++data->generation;", prefs)

    def test_default_and_numeric_boolean_validation(self):
        result = subprocess.run(
            ["cc", "-E", "-P", "-x", "c", "-DTOUCHSCREEN_EDGE_TRACE=0",
             "-I", str(self.include_dir), str(SOURCES["worker"])],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("heartbeat after", result.stdout)
        self.assertIn(" started, raw", result.stdout)

        invalid = subprocess.run(
            ["cc", "-E", "-P", "-x", "c", "-DTOUCHSCREEN_DIAGNOSTICS=2",
             "-DTOUCHSCREEN_EDGE_TRACE=0", "-I", str(self.include_dir),
             str(SOURCES["worker"])],
            capture_output=True, text=True,
        )
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("must be 0 or 1", invalid.stderr)

        makefile = (TOUCH / "mmakefile.src").read_text()
        self.assertIn("TOUCHSCREEN_DIAGNOSTICS ?= 0", makefile)
        self.assertIn(
            "-DTOUCHSCREEN_DIAGNOSTICS=$(TOUCHSCREEN_DIAGNOSTICS)", makefile
        )


if __name__ == "__main__":
    unittest.main()
