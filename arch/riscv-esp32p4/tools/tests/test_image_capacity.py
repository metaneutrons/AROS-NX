"""Host tests for exact image sizing and fail-closed artifact bounds."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]
FORMATTER = ROOT / "arch/riscv-esp32p4/image/mkfat32.py"
SIZE_CHECKER = ROOT / "arch/riscv-esp32p4/tools/check-artifact-size.py"
IMAGE_RULES = ROOT / "arch/riscv-esp32p4/image/mmakefile.src"
BOOT_RULES = ROOT / "arch/riscv-esp32p4/boot/mmakefile.src"
MIB = 1024 * 1024
SECTOR = 512


class ImageSizeTests(unittest.TestCase):
    def run_formatter(self, directory, *size_args):
        content = directory / "content"
        content.mkdir()
        (content / "marker.txt").write_bytes(b"capacity test\n")
        output = directory / "volume.img"
        result = subprocess.run(
            [sys.executable, str(FORMATTER), "--content", str(content),
             "--out", str(output), *size_args, "--fat-bits", "16",
             "--serial", "0x12345678", "--timestamp", "2026-01-01"],
            capture_output=True, text=True, check=False)
        return result, output

    def test_ab_develop_image_is_exact_and_has_valid_fat16_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            result, image = self.run_formatter(
                Path(temp), "--size-bytes", "0x002c0000",
                "--part-start", "1")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(image.stat().st_size, 0x002c0000)
            raw = image.read_bytes()
            part_start = struct.unpack_from("<I", raw, 454)[0]
            part_sectors = struct.unpack_from("<I", raw, 458)[0]
            self.assertEqual(part_start, 1)
            self.assertEqual(part_sectors, 0x002c0000 // SECTOR - 1)
            self.assertEqual(raw[part_start * SECTOR + 54:
                                 part_start * SECTOR + 62], b"FAT16   ")

            layout = image.with_suffix(".layout").read_text()
            self.assertIn("total bytes      2883584\n", layout)
            self.assertIn("total sectors    5632\n", layout)
            self.assertIn("partition start  1\n", layout)
            self.assertIn("fat type         FAT16\n", layout)
            clusters = int(next(line.split()[-1] for line in layout.splitlines()
                                if line.startswith("clusters         ")))
            self.assertGreaterEqual(clusters, 4085)

    def test_legacy_mib_size_and_partition_start_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            result, image = self.run_formatter(
                Path(temp), "--size-mb", "4")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(image.stat().st_size, 4 * MIB)
            raw = image.read_bytes()
            self.assertEqual(struct.unpack_from("<I", raw, 454)[0], 2048)

    def test_size_arguments_are_mutually_exclusive(self):
        with tempfile.TemporaryDirectory() as temp:
            result, image = self.run_formatter(
                Path(temp), "--size-mb", "4", "--size-bytes", "4194304")

            self.assertEqual(result.returncode, 2)
            self.assertIn("not allowed with argument", result.stderr)
            self.assertFalse(image.exists())

    def test_byte_size_rejects_nonpositive_unaligned_and_out_of_range_values(self):
        invalid_args = (("--size-bytes", "0"),
                        ("--size-bytes=-512",),
                        ("--size-bytes", "513"),
                        ("--size-bytes", "0x200000000200"))
        for size_args in invalid_args:
            with self.subTest(size_args=size_args), \
                    tempfile.TemporaryDirectory() as temp:
                result, image = self.run_formatter(Path(temp), *size_args)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(image.exists())

    def test_small_fat16_volume_still_obeys_cluster_minimum(self):
        with tempfile.TemporaryDirectory() as temp:
            result, image = self.run_formatter(
                Path(temp), "--size-bytes", "0x002c0000")

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAT16 requires 4085 to 65524", result.stderr)
            self.assertFalse(image.exists())


class ArtifactSizeTests(unittest.TestCase):
    def check_artifact(self, path, limit):
        return subprocess.run(
            [sys.executable, str(SIZE_CHECKER), str(path), str(limit)],
            capture_output=True, text=True, check=False)

    def test_below_and_equal_limit_pass_and_report_actual_size(self):
        with tempfile.TemporaryDirectory() as temp:
            artifact = Path(temp) / "aros-bsp.pkg"
            for size in (15, 16):
                with self.subTest(size=size):
                    artifact.write_bytes(b"x" * size)
                    result = self.check_artifact(artifact, "0x10")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("%d of 16 bytes" % size, result.stdout)

    def test_oversize_fails_and_reports_actual_size(self):
        with tempfile.TemporaryDirectory() as temp:
            artifact = Path(temp) / "aros-bsp.pkg"
            artifact.write_bytes(b"x" * 17)

            result = self.check_artifact(artifact, "0x10")

            self.assertEqual(result.returncode, 1)
            self.assertIn("17 bytes exceeds the 16-byte limit", result.stderr)

    def test_missing_artifact_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            artifact = Path(temp) / "missing.pkg"

            result = self.check_artifact(artifact, "0x10")

            self.assertEqual(result.returncode, 1)
            self.assertIn("cannot stat", result.stderr)
            self.assertNotIn("of 16 bytes", result.stdout)


class MakeRuleTests(unittest.TestCase):
    def test_flash_volumes_use_exact_bytes_but_sd_image_keeps_mib(self):
        rules = IMAGE_RULES.read_text()

        self.assertEqual(
            rules.count("--size-bytes $(FLASHDISK_SIZE_BYTES)"), 2)
        self.assertIn("FLASHDISK_SIZE_BYTES  := "
                      "$(P4_BOARD_FLASHDISK_SIZE_BYTES)", rules)
        self.assertIn("FLASHDISK_PART_START  := 2048", rules)
        self.assertIn("FLASHDISK_PART_START := 1", rules)
        self.assertIn("ifeq ($(P4_FLASH_LAYOUT),ab)", rules)
        self.assertIn("--size-mb $(IMAGE_SIZE_MB)", rules)
        self.assertNotIn("--size-mb $(FLASHDISK_SIZE_MB)", rules)

    def test_package_checksize_calls_fail_closed_byte_checker(self):
        rules = BOOT_RULES.read_text()

        self.assertIn("check-artifact-size.py", rules)
        self.assertIn('"$(AROSARCHDIR)/aros-bsp.pkg" $(PKG_LIMIT)', rules)
        self.assertNotIn("wc -c", rules)


if __name__ == "__main__":
    unittest.main()
