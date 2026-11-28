"""Pure-file and fake-runner tests; never opens a USB or serial device."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import hs_recovery as recovery
import hs_esptool_once as esptool_once


MAC = "80:f1:b2:d3:3b:a6"
IDENTITY = ("Chip type:          ESP32-P4 (revision v1.3)\n"
            "USB mode:           USB-OTG\nMAC:                " + MAC + "\n")
SECURITY = ("Flags: 0x00000000 (0b0)\nSecure Boot: Disabled\n"
            "Flash Encryption: Disabled\n")


def image_bytes(content=b"verified-image"):
    header = bytearray(24)
    header[0:2] = b"\xe9\x01"
    header[12:14] = struct.pack("<H", 18)
    header[23] = 1
    image = header + struct.pack("<II", 0x40000000, len(content)) + content
    checksum = 0xEF
    for byte in content:
        checksum ^= byte
    image += b"\0" * (15 - len(image) % 16) + bytes([checksum])
    return bytes(image) + hashlib.sha256(image).digest()


def table_bytes(entries=None):
    if entries is None:
        entries = [("ota_0", 0, 0x10, 0x20000, 0x800000, 0),
                   ("arosbsp", 0x40, 0, 0x820000, 0x7E0000, 0)]
    records = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype,
                                    offset, size, name.encode("ascii"), flags)
                       for name, kind, subtype, offset, size, flags in entries)
    records += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(records).digest()
    return records + b"\xff" * (4096 - len(records))


class FakeRunner:
    def __init__(self, directory, failure=None):
        self.directory, self.failure = directory, failure
        self.calls = []
        self.before = bytes(range(256)) * 16
        self.table = table_bytes()

    def command(self, label, arguments, no_stub=False):
        self.calls.append((label, arguments, no_stub))
        if self.failure == label:
            raise subprocess.CalledProcessError(1, ["fake-esptool"])
        output = IDENTITY
        if label == "security":
            if self.failure == "wrong-mac":
                output = output.replace(MAC, "00:11:22:33:44:55")
            if self.failure == "not-hs":
                output = output.replace("USB-OTG", "USB-Serial/JTAG")
            output += SECURITY.replace("0x00000000", "0x00000001") if self.failure == "secure" else SECURITY
        elif label == "flash-id":
            output += "Detected flash size: 16MB\n"
        elif label in ("table", "table-check"):
            data = self.table
            if self.failure == "changed-table" and label == "table-check":
                data = bytes([data[0] ^ 1]) + data[1:]
            Path(arguments[-1]).write_bytes(data)
        elif label == "before":
            data = self.before[:-1] if self.failure == "short-pre-read" else self.before
            Path(arguments[-1]).write_bytes(data)
        elif label == "before-check":
            data = self.before
            if self.failure == "changed-before":
                data = bytes([data[0] ^ 1]) + data[1:]
            Path(arguments[-1]).write_bytes(data)
        elif label == "write":
            output += "Hash of data verified.\n" if self.failure != "no-write-hash" else "No verification\n"
        elif label == "after":
            data = (self.directory / "payload.bin").read_bytes()
            if self.failure == "corrupt-readback":
                data = bytes([data[0] ^ 1]) + data[1:]
            if self.failure == "short-readback":
                data = data[:-1]
            Path(arguments[-1]).write_bytes(data)
        else:
            raise AssertionError("unexpected command")
        return output


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.image = image_bytes()

    def run_case(self, failure=None):
        runner = FakeRunner(self.directory, failure)
        try:
            result = recovery.recover(runner, self.image, recovery.digest(self.image),
                                      MAC, self.directory)
        except Exception:
            result = json.loads((self.directory / "result.json").read_text())
            return runner, result, False
        return runner, result, True

    def test_valid_image_and_exact_sector_tail(self):
        recovery.validate_image(self.image, recovery.digest(self.image))
        before = bytes(range(256)) * 16
        payload = recovery.payload_for(self.image, before, table_bytes(), 16 << 20)
        self.assertEqual(payload[:len(self.image)], self.image)
        self.assertEqual(payload[len(self.image):], before[len(self.image):])
        self.assertEqual(len(payload), 4096)

    def test_image_checksum_hash_chip_and_length_rejected(self):
        for index in (0, 1, 12, 23, 35, len(self.image) - 1):
            with self.subTest(index=index):
                data = bytearray(self.image)
                data[index] ^= 1
                with self.assertRaises(ValueError):
                    recovery.validate_image(data, recovery.digest(data))
        with self.assertRaises(ValueError):
            recovery.validate_image(self.image, "0" * 64)
        with self.assertRaises(ValueError):
            recovery.validate_image(self.image + b"\xff", recovery.digest(self.image + b"\xff"))

    def test_table_checksum_and_trailing_data(self):
        for index in (0, 32, 80, 200):
            table = bytearray(table_bytes())
            table[index] ^= 1
            with self.subTest(index=index), self.assertRaises(ValueError):
                recovery.validate_table(table, 16 << 20)

    def test_table_rejects_overlap_flags_ab_and_wrong_offsets(self):
        for entries in (
            [("ota_0", 0, 0x10, 0x40000, 0x80000, 0)],
            [("ota_0", 0, 0x10, 0x20000, 0x800000, 1)],
            [("ota_0", 0, 0x10, 0x20000, 0x800000, 0), ("other", 1, 0, 0x30000, 0x1000, 0)],
            [("ota_0", 0, 0x10, 0x20000, 0x800000, 0), ("ota_1", 0, 0x11, 0x820000, 0x1000, 0)],
            [("ota_0", 0, 0x10, 0x20000, 0x800000, 0), ("large", 1, 0, 0x820000, 0x1000000, 0)],
        ):
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                recovery.validate_table(table_bytes(entries), 16 << 20)

    def test_exact_pre_read_required(self):
        for length in (0, 4095, 4097, 8192):
            with self.subTest(length=length), self.assertRaises(ValueError):
                recovery.payload_for(self.image, b"\0" * length, table_bytes(), 16 << 20)

    def test_only_one_write_full_readback_and_no_boot_claim(self):
        runner, result, success = self.run_case()
        self.assertTrue(success)
        self.assertEqual(result["status"], "VERIFIED_READBACK")
        self.assertFalse(result["boot_verified"])
        writes = [args for label, args, _ in runner.calls if label == "write"]
        self.assertEqual(writes, [["write-flash", "0x20000", str(self.directory / "payload.bin")]])
        self.assertEqual([label for label, _, _ in runner.calls],
                         ["security", "flash-id", "table", "before", "table-check",
                          "before-check", "write", "after"])
        before_check = next(args for label, args, _ in runner.calls
                            if label == "before-check")
        self.assertEqual(before_check, ["read-flash", "0x20000", "0x1000",
                                        str(self.directory / "before-check.bin")])

    def test_prewrite_failures_never_write(self):
        for failure in ("wrong-mac", "not-hs", "secure", "security", "table",
                        "before", "short-pre-read", "changed-table", "before-check",
                        "changed-before"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                original = self.directory
                self.directory = Path(tmp)
                runner, result, success = self.run_case(failure)
                self.directory = original
                self.assertFalse(success)
                self.assertEqual(result["status"], "REFUSED")
                self.assertFalse(any(label == "write" for label, _, _ in runner.calls))

    def test_write_failure_is_uncertain_and_never_rolled_back(self):
        for failure in ("write", "no-write-hash", "after", "corrupt-readback", "short-readback"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                original = self.directory
                self.directory = Path(tmp)
                runner, result, success = self.run_case(failure)
                self.directory = original
                self.assertFalse(success)
                self.assertEqual(result["status"], "WRITE_UNCERTAIN")
                self.assertEqual(sum(label == "write" for label, _, _ in runner.calls), 1)
                self.assertTrue((Path(tmp) / "before.bin").exists())

    def test_ambiguous_security_or_identity_refused(self):
        for output in (IDENTITY + "MAC: 00:11:22:33:44:55\n" + SECURITY,
                       IDENTITY + SECURITY + "Flags: 0x0\n",
                       IDENTITY + SECURITY.replace("Disabled", "Enabled"),
                       IDENTITY.replace("USB-OTG", "unknown") + SECURITY):
            with self.subTest(output=output), self.assertRaises(ValueError):
                recovery.verify_identity(output, MAC, security=True)

    def test_devserial_ownership_is_never_released(self):
        runner = recovery.Runner("/unused/esptool", "/dev/test", self.directory)
        with patch.object(recovery.subprocess, "run", return_value=subprocess.CompletedProcess(
                [], 0, json.dumps({"managed": [{"name": "/dev/test"}]}), "")) as run:
            with self.assertRaises(ValueError):
                runner.command("refused", ["write-flash", "0x20000", "unused"])
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0], ["devserial", "list", "--json"])

    def test_command_has_no_reset_or_shell_and_has_a_deadline(self):
        runner = recovery.Runner("/unused/esptool", "/dev/test", self.directory,
                                 python="/test/python")
        with patch.object(runner, "idle"), patch.object(recovery.subprocess, "run") as run:
            runner.command("fake", ["get-security-info"], no_stub=True)
            command = run.call_args.args[0]
            self.assertEqual(command[:2], ["/test/python", str(
                Path(recovery.__file__).with_name("hs_esptool_once.py"))])
            self.assertEqual(command[command.index("--before") + 1], "no-reset")
            self.assertEqual(command[command.index("--after") + 1], "no-reset")
            self.assertIn("--no-stub", command)
            self.assertEqual(run.call_args.kwargs["timeout"], 120)
            self.assertNotIn("shell", run.call_args.kwargs)

    def test_esptool_shebang_must_select_absolute_interpreter(self):
        interpreter = self.directory / "python"
        interpreter.write_text("#!/bin/sh\nexit 0\n")
        interpreter.chmod(0o755)
        script = self.directory / "esptool"
        script.write_text(f"#!{interpreter}\nimport esptool\n")
        script.chmod(0o755)
        self.assertEqual(recovery.esptool_python(script), str(interpreter))

        script.write_text("#!/usr/bin/env python3\n")
        with self.assertRaises(ValueError):
            recovery.esptool_python(script)

    def test_esptool_wrapper_pins_version_and_sets_one_whole_write_attempt(self):
        class Loader:
            WRITE_FLASH_ATTEMPTS = 2

        esptool = SimpleNamespace(__version__="5.5.0")
        loader = SimpleNamespace(ESPLoader=Loader)
        with patch.object(esptool_once.metadata, "version", return_value="5.5.0"):
            esptool_once.configure_single_write_attempt(esptool, loader)
        self.assertEqual(Loader.WRITE_FLASH_ATTEMPTS, 1)

        Loader.WRITE_FLASH_ATTEMPTS = 2
        with patch.object(esptool_once.metadata, "version", return_value="5.5.1"):
            with self.assertRaises(RuntimeError):
                esptool_once.configure_single_write_attempt(esptool, loader)
        self.assertEqual(Loader.WRITE_FLASH_ATTEMPTS, 2)

    def test_symlink_and_oversize_inputs_refused(self):
        source = self.directory / "source"
        source.write_bytes(b"abc")
        link = self.directory / "link"
        link.symlink_to(source)
        for path, limit in ((link, 100), (source, 2)):
            with self.assertRaises(ValueError):
                recovery.regular_bytes(path, limit)


if __name__ == "__main__":
    unittest.main()
