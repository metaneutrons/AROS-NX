"""Compile the live diagnostic wrapper with host ROM stubs."""
import pathlib
import os
import shutil
import subprocess
import tempfile
import unittest


class BootloaderConsoleTest(unittest.TestCase):
    def test_missing_idf_environment_fails_before_creating_evidence(self):
        helper = pathlib.Path(__file__).parents[1] / "build-bootloader-console-comparison.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            idf = root / "idf"
            source = idf / "components/bootloader_support/src"
            source.mkdir(parents=True)
            (source / "bootloader_utility.c").touch()
            reference = root / "reference"
            (reference / "build/bootloader").mkdir(parents=True)
            (reference / "sdkconfig").write_text("CONFIG_ESP_CONSOLE_UART=y\n")
            (reference / "sdkconfig.defaults").touch()
            (reference / "build/bootloader/bootloader.bin").touch()
            evidence = root / "evidence"
            environment = os.environ.copy()
            environment.pop("IDF_PYTHON_ENV_PATH", None)
            result = subprocess.run(["bash", str(helper), str(idf),
                                     str(reference), str(evidence)],
                                    env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("IDF_PYTHON_ENV_PATH", result.stderr)
            self.assertFalse(evidence.exists())

    def test_preserves_uart_setup_and_removes_only_channel_two(self):
        source = pathlib.Path(__file__).parents[2] / "bootloader/diagnostics/no_usb_secondary/console.c"
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "esp_rom_sys.h").write_text(
                "void esp_rom_install_channel_putc(int, void (*)(char));\n")
            (root / "fixture.c").write_text('''
#include <assert.h>
#include <stddef.h>
static int stage;
void __real_esp_rom_install_uart_printf(void) { assert(stage == 0); stage = 1; }
void esp_rom_install_channel_putc(int channel, void (*putc)(char)) {
    assert(stage == 1); assert(channel == 2); assert(putc == NULL); stage = 2;
}
void __wrap_esp_rom_install_uart_printf(void);
int main(void) { __wrap_esp_rom_install_uart_printf(); assert(stage == 2); return 0; }
''')
            binary = root / "fixture"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", "-I", str(root), str(source),
                            str(root / "fixture.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
