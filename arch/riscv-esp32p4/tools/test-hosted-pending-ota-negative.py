#!/usr/bin/env python3
"""Prove that accidentally allowlisting OTA makes the transport gate fail.

Only temporary host fixtures are changed. No network, device or flash access.
Run the normal pending suite separately; a broken compiler is not a pass here.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
NETWORK = PORT / "network"


def main():
    original = (NETWORK / "hosted_pending.c").read_text()
    marker = "return request_id == 257u || request_id == 260u ||"
    if original.count(marker) != 1:
        raise SystemExit("allowlist changed; update the explicit negative fixture")
    mutant = original.replace(marker,
        "return (request_id >= 272u && request_id <= 274u) || "
        "request_id == 257u || request_id == 260u ||", 1)
    names = ["hosted_fifo", "sdio_commands", "hosted_wire", "hosted_tx",
             "hosted_rpc", "hosted_ota", "hosted_serial", "hosted_request_ids",
             "hosted_association", "test_hosted_pending"]
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="pending-ota-negative-") as temp:
        source = Path(temp) / "hosted_pending.c"
        source.write_text(mutant)
        binary = Path(temp) / "test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(NETWORK), str(source),
            *[str(NETWORK / (name + ".c")) for name in names],
            "-o", str(binary)]
        subprocess.run(command, check=True, timeout=60, env=environment)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                timeout=60, env=environment)
        if result.returncode != 1 or ": !accepted" not in result.stderr:
            raise SystemExit("negative control failed at an unexpected boundary:\n"
                             + result.stdout + result.stderr)
        print("PASS: allowing OTA in normal pending transport is rejected by the test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
