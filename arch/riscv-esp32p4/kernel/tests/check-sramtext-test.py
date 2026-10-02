#!/usr/bin/env python3
"""Synthetic objdump regression; no firmware or hardware writes."""
import pathlib
import subprocess
import tempfile
import unittest

CHECKER = pathlib.Path(__file__).resolve().parents[1] / "check-sramtext.sh"


class ResidencyTest(unittest.TestCase):
    def check_line(self, line, expected):
        with tempfile.TemporaryDirectory() as directory:
            dump = pathlib.Path(directory) / "objdump"
            dump.write_text("#!/bin/sh\ncat <<'EOF'\n" + line + "\nEOF\n")
            dump.chmod(0o700)
            result = subprocess.run(["sh", str(CHECKER), str(dump), "fixture"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stderr)

    def test_stale_absolute_version_increment(self):
        self.check_line("4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <__aros_libreq_SysBase.50+0x3fffffcf>", 0)

    def test_real_xip_arithmetic(self):
        self.check_line("4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <flash_data+0x1>", 1)

    def test_stale_absolute_version_load(self):
        self.check_line("4ff01f56:\t0105a803\tlw\ta6,16(a1) # 40000010 <__aros_libreq_SysBase.50+0x3fffffde>", 0)

    def test_real_absolute_materialization_not_exempt(self):
        self.check_line("4ff00850:\t40000737\tlui\ta4,0x40000\n"
                        "4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <__aros_libreq_SysBase.50+0x3fffffcf>", 1)

    def test_load_overwrite_clears_stale_lui(self):
        self.check_line("4ff00850:\t40000737\tlui\ta4,0x40000\n"
                        "4ff00856:\t4798\tlw\ta4,8(a5)\n"
                        "4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <__aros_libreq_SysBase.50+0x3fffffcf>", 0)

    def test_real_absolute_load_not_exempt(self):
        self.check_line("4ff00850:\t400005b7\tlui\ta1,0x40000\n"
                        "4ff01f56:\t0105a803\tlw\ta6,16(a1) # 40000010 <__aros_libreq_SysBase.50+0x3fffffde>", 1)

    def test_local_offset_label_preserves_provenance(self):
        for label in ("foo+0x10", ".Lblock"):
            with self.subTest(label=label):
                self.check_line("4ff00850:\t40000737\tlui\ta4,0x40000\n"
                                "4ff00854 <" + label + ">:\n"
                                "4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <__aros_libreq_SysBase.50+0x3fffffcf>", 1)

    def test_auipc_sram_alias_dereference_comment_is_stale(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00000:\t00004797\tauipc\ta5,0x4\n"
                        "4ff00004:\tffc78793\taddi\ta5,a5,-4\n"
                        "4ff00008:\t8b3e\tmv\ts6,a5\n"
                        "4ff0000a:\t038b2703\tlw\ta4,56(s6) # 40000038 <aros_app_desc+0x18>", 0)

    def test_auipc_xip_alias_dereference_remains_rejected(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00000:\tf0100797\tauipc\ta5,0xf0100\n"
                        "4ff00004:\t00078793\taddi\ta5,a5,0\n"
                        "4ff00008:\t8b3e\tmv\ts6,a5\n"
                        "4ff0000a:\t038b2703\tlw\ta4,56(s6) # 40000038 <aros_app_desc+0x18>", 1)

    def test_auipc_addi_without_move_does_not_prove_dereference(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00000:\t00004797\tauipc\ta5,0x4\n"
                        "4ff00004:\tffc78793\taddi\ta5,a5,-4\n"
                        "4ff00008:\t0387a703\tlw\ta4,56(a5) # 40000038 <aros_app_desc+0x18>", 1)

    def test_unknown_base_real_xip_dereference_comment_remains_rejected(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00020:\t038b2703\tlw\ta4,56(s6) # 40000038 <aros_app_desc+0x18>", 1)

    def test_call_invalidates_caller_saved_but_preserves_sram_saved_register(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00000:\t00004797\tauipc\ta5,0x4\n"
                        "4ff00004:\tffc78793\taddi\ta5,a5,-4\n"
                        "4ff00008:\t8b3e\tmv\ts6,a5\n"
                        "4ff0000a:\t000000ef\tjal\tra,4ff00100 <callee>\n"
                        "4ff0000e:\t038b2703\tlw\ta4,56(s6) # 40000038 <aros_app_desc+0x18>", 0)

    def test_call_loses_caller_saved_sram_proof(self):
        self.check_line("4ff00000 <probe>:\n"
                        "4ff00000:\t00004797\tauipc\ta5,0x4\n"
                        "4ff00004:\tffc78793\taddi\ta5,a5,-4\n"
                        "4ff00008:\t038000ef\tjal\tra,4ff00040 <callee>\n"
                        "4ff0000c:\t038ba703\tlw\ta4,56(a5) # 40000038 <aros_app_desc+0x18>", 1)

    def test_sram_alias_add_is_not_xip_materialization(self):
        self.check_line("4ff00850:\t40000737\tlui\ta4,0x40000\n"
                        "4ff00854:\t973e\tadd\ta4,a4,a5\n"
                        "4ff0085a:\t0705\taddi\ta4,a4,1 # 40000001 <__aros_libreq_SysBase.50+0x3fffffcf>", 0)

    def test_calls_loads_stores_and_relocations_remain_rejected(self):
        for operation in ("jal\tra,40000001 <__aros_libreq_SysBase.50+0x3fffffcf>",
                          "lw\ta4,0(a5) # 40000001 <flash_data+0x1>",
                          "sw\ta4,0(a5) # 40000001 <flash_data+0x1>",
                          "R_RISCV_CALL\t40000001 <__aros_libreq_SysBase.50+0x3fffffcf>"):
            with self.subTest(operation=operation):
                self.check_line("4ff0085a:\t0705\t" + operation, 1)


if __name__ == "__main__":
    unittest.main()
