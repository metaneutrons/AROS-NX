import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("ab_bsp", Path(__file__).resolve().parents[1] / "make-ab-bsp.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EnvelopeTests(unittest.TestCase):
    def setUp(self):
        self.core = bytearray(64)
        self.core[0] = 0xE9
        struct.pack_into("<H", self.core, 12, 18)
        self.pkg = b"PKG\x01" + struct.pack(">I", 8)

    def test_real_hashes_and_contract(self):
        result = module.envelope(self.core, self.pkg)
        self.assertEqual(len(result), 136)
        self.assertEqual(result[:8], b"P4ABPKG\x01")
        self.assertEqual(result[16:48], hashlib.sha256(self.core).digest())
        self.assertEqual(result[48:80], hashlib.sha256(self.pkg).digest())
        self.assertEqual(result[80:112], hashlib.sha256(result[:80]).digest())
        self.assertEqual(result[112:128], bytes(16))
        self.assertEqual(result[128:], self.pkg)

    def test_bounds_and_target(self):
        for core, pkg in [(bytes(24), self.pkg), (self.core + bytes(module.CORE_LIMIT), self.pkg),
                          (self.core, self.pkg + b"x"), (self.core, b"bad")]:
            with self.assertRaises(ValueError):
                module.envelope(core, pkg)
        self.core[12] = 13
        with self.assertRaises(ValueError):
            module.envelope(self.core, self.pkg)

    def test_envelope_counts_against_limit(self):
        size = module.BSP_LIMIT - 128
        pkg = b"PKG\x01" + struct.pack(">I", size) + bytes(size - 8)
        self.assertEqual(len(module.envelope(self.core, pkg)), module.BSP_LIMIT)
        pkg = b"PKG\x01" + struct.pack(">I", size + 1) + bytes(size - 7)
        with self.assertRaises(ValueError):
            module.envelope(self.core, pkg)


if __name__ == "__main__":
    unittest.main()
