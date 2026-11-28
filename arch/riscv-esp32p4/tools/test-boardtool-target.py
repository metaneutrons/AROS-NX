#!/usr/bin/env python3
"""Evaluate the actual generated ESP32Board makefile for allowed/denied targets.

Minimal temporary config isolates conditional recipe selection; this is not a
cross-platform build. The real RV32 build remains a separate required gate.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_root", type=Path)
    args = parser.parse_args()
    generated = args.build_root.resolve() / "workbench/tools/ESP32Board/mmakefile"
    mmake = args.build_root.resolve() / "bin/darwin-aarch64/tools/mmake"
    source = Path(__file__).resolve().parents[3] / "workbench/tools/ESP32Board/mmakefile.src"
    text = source.read_text()
    dependency = ("#MM- workbench-tools : workbench-tools-esp32board-"
                  "$(AROS_TARGET_ARCH)-$(AROS_TARGET_CPU)-enabled")
    if dependency not in text or ("#MM- workbench-tools-esp32board-esp32p4-"
                                  "riscv-enabled : workbench-tools-esp32board") not in text:
        raise SystemExit("missing architecture/CPU-specific aggregate registration")
    if "#MM- workbench-tools : workbench-tools-esp32board\n" in text:
        raise SystemExit("unconditional aggregate registration")
    make = shlex.split(os.environ.get("MAKE", "gmake"))
    with tempfile.TemporaryDirectory(prefix="boardtool-target-") as temp:
        root = Path(temp)
        (root / "config").mkdir()
        (root / "config/aros.cfg").write_text("")
        (root / "config/make.tail").write_text("")
        harness = root / "probe.mk"
        harness.write_text(f"include {generated}\n"
            "probe:\n\t@echo PROG=$(workbench-tools-esp32board_PROGNAME)\n")
        for arch, cpu, allowed in [("esp32p4", "riscv", True),
                ("raspi", "arm", False), ("pc", "x86_64", False),
                ("raspi", "riscv", False), ("esp32p4", "arm", False),
                ("esp32p4", "riscv64", False)]:
            common = make + ["--no-print-directory", "-f", str(harness),
                f"SRCDIR={root}", f"AROS_TARGET_ARCH={arch}",
                f"AROS_TARGET_CPU={cpu}"]
            result = subprocess.run(common + ["probe"], capture_output=True,
                text=True, timeout=30, cwd=root)
            if result.returncode or ("PROG=ESP32Board" in result.stdout) != allowed:
                raise SystemExit(f"wrong program rule selection {arch}/{cpu}:\n"
                                 + result.stdout + result.stderr)
            if not allowed:
                for target in ("workbench-tools-esp32board",
                               "workbench-tools-esp32board-quick"):
                    result = subprocess.run(common + [target], capture_output=True,
                        text=True, timeout=30, cwd=root)
                    if result.returncode != 2 or "ESP32Board requires" not in result.stderr:
                        raise SystemExit(f"non-ESP32 direct target did not refuse {target}")
            # #MM- edges are interpreted by MetaMake, not GNU make. Exercise
            # its real aggregate traversal with the generated file, recording
            # selected program targets instead of compiling fixture objects.
            meta = root / f"meta-{arch}-{cpu}"
            meta.mkdir()
            (meta / "mmakefile").write_text(generated.read_text())
            (meta / "vars.cfg").write_text(
                f"AROS_TARGET_ARCH = {arch}\nAROS_TARGET_CPU = {cpu}\n"
                f"ARCH = {arch}\nCPU = {cpu}\nFAMILY = fixture\n"
                "AROS_TARGET_VARIANT = smp\nAROS_TOOLCHAIN_RELEASE = fixture\n")
            (meta / "mmake.config").write_text(
                "[PROBE]\n"
                f"top {meta}\n"
                "defaultmakefilename mmakefile\n"
                "maketool echo INVOKED $(TARGET)\n"
                f"globalvarfile {meta / 'vars.cfg'}\n")
            env = os.environ.copy()
            env["MMAKE_CONFIG"] = str(meta / "mmake.config")
            result = subprocess.run([str(mmake), "-v", f"--srcdir={meta}",
                f"--builddir={meta}", "PROBE.workbench-tools"],
                capture_output=True, text=True, timeout=30, cwd=meta, env=env)
            selected = "INVOKED workbench-tools-esp32board" in result.stdout
            if result.returncode or selected != allowed:
                raise SystemExit(f"wrong MetaMake aggregate selection {arch}/{cpu}:\n"
                                 + result.stdout + result.stderr)
            print(f"PASS {arch}/{cpu}: {'enabled' if allowed else 'not registered/refused'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
