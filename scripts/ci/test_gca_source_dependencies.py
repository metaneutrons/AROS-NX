#!/usr/bin/env python3
"""Check GCA archive input declarations, not a Mesa build or SDK qualification."""

from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "arch/arm-native/soc/broadcom/2708/hidd/vc4gallium/mmakefile.src"
ARCHIVE = "$(top_libdir)/libgallium_vc4_gca.a"
PRIVATE_LIST = "$(GCA_GENDIR)/gca_private.list"


def archive_header(source: str) -> str:
    lines = source.splitlines()
    starts = [i for i, line in enumerate(lines)
              if line.startswith(ARCHIVE + " :")]
    if len(starts) != 1:
        raise AssertionError("expected exactly one original GCA archive rule")
    i = starts[0]
    header = [lines[i]]
    while header[-1].endswith("\\"):
        i += 1
        header.append(lines[i])
    return "\n".join(header)


class GcaSourceDependencyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.make = shutil.which("gmake") or shutil.which("make")
        self.assertIsNotNone(self.make, "GNU Make is required")
        version = subprocess.run([self.make, "--version"], check=True,
                                 capture_output=True, text=True, timeout=10)
        self.assertRegex(version.stdout, r"GNU Make (?:[4-9]|[1-9][0-9]+)\.")

    def expanded_prerequisites(self, header: str) -> list[str]:
        # Interpret the ORIGINAL header with GNU Make, not a duplicate Python
        # variable evaluator. Only recipes are synthetic no-op/data recorders;
        # no archive, compiler, generator or original shell recipe is executed.
        with tempfile.TemporaryDirectory(prefix="aros-gca-inputs-") as temporary:
            root = Path(temporary)
            program = root / "rules.mk"
            program.write_text(
                f"top_libdir := {root}/lib\nGCA_GENDIR := {root}/generated\n"
                + header + "\n\t@printf '%s\\n' '$^'\n"
                + "%:\n\t@:\n", encoding="utf-8")
            result = subprocess.run(
                [self.make, "--no-builtin-rules", "--no-builtin-variables",
                 "--no-print-directory", "-f", str(program),
                 str(root / "lib/libgallium_vc4_gca.a")],
                capture_output=True, text=True, timeout=10, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return [str(Path(word).relative_to(root))
                    for word in shlex.split(result.stdout)]

    def assert_list_dependency(self, prerequisites: list[str]) -> None:
        self.assertEqual(prerequisites.count("generated/gca_private.list"), 1,
                         "private list must be an explicit archive prerequisite")

    def test_original_archive_declares_private_list(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        self.assert_list_dependency(self.expanded_prerequisites(archive_header(source)))

    def test_previous_missing_edge_is_rejected(self) -> None:
        header = archive_header(SOURCE.read_text(encoding="utf-8"))
        self.assertEqual(header.count(PRIVATE_LIST), 1)
        old_header = header.replace(PRIVATE_LIST + " ", "", 1)
        # GNU Make still accepts the old rule; the semantic gate must detect
        # its missing dependency rather than relying on a parse failure.
        prerequisites = self.expanded_prerequisites(old_header)
        with self.assertRaisesRegex(AssertionError, "explicit archive prerequisite"):
            self.assert_list_dependency(prerequisites)


if __name__ == "__main__":
    unittest.main()
