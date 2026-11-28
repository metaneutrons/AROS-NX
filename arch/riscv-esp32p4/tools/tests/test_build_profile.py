import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("profile", Path(__file__).resolve().parents[1] / "check-build-profile.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProfileTests(unittest.TestCase):
    def test_fixed_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            module.check(build, "d1001", "ab")
            module.check(build, "d1001", "ab")
            for board, layout in [("d1001", "legacy"), ("jc1060p470c-v2", "ab")]:
                with self.assertRaises(ValueError):
                    module.check(build, board, layout)

    def test_unmarked_kernel_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            kernel = build / "bin/esp32p4-riscv-smp/gen/rom/boot/core.elf"
            kernel.parent.mkdir(parents=True)
            kernel.touch()
            with self.assertRaises(ValueError):
                module.check(build, "d1001", "ab")
            module.check(build, "d1001", "legacy")

    def test_malformed_marker_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "config").mkdir()
            (build / "config/esp32p4-build-profile.json").write_text("bad")
            with self.assertRaises(ValueError):
                module.check(build, "d1001", "ab")


if __name__ == "__main__":
    unittest.main()
