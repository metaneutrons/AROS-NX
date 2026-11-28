#!/usr/bin/env python3
"""Compare the independent envelope codec to a local pinned protobuf-c tree.

This compiles host-only test dependencies in a temporary directory. It never
changes the reference tree and never invokes a board or network connection.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SCHEMA_SHA256 = "bbdbc9bb69c2b1793de6441ea55051c3ceb3f7b30994227b8ff1e995b0137dec"
REFERENCE_HASHES = {
    "common/proto/esp_hosted_rpc.pb-c.c": "693efbce5db14bf6f2416f5fcf746049a459764ec9f8571a45f2b6a7bdffcc69",
    "common/proto/esp_hosted_rpc.pb-c.h": "ad4f03ade1ca38d88354cc3104f1f4838f7c25004197e19a8fac395c1ad05e85",
    "common/protobuf-c/protobuf-c/protobuf-c.c": "5af483efb77b6c60afce921ff361cff5320b2a80698f6e7fb0356c49b48933a7",
    "common/protobuf-c/protobuf-c/protobuf-c.h": "e1c6306390090e4468472dbed3928e6983d1be3306d2006ea3fa49696aef6946",
}
PORT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_root", type=Path,
                        help="local pinned ESP-Hosted reference tree")
    arguments = parser.parse_args()
    reference = arguments.reference_root.resolve()
    schema = reference / "common/proto/esp_hosted_rpc.proto"
    if hashlib.sha256(schema.read_bytes()).hexdigest() != SCHEMA_SHA256:
        raise SystemExit("refusing a different RPC schema")
    for relative, expected in REFERENCE_HASHES.items():
        source = reference / relative
        actual = hashlib.sha256(source.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"refusing a different reference file: {relative}")
        print(f"PIN SHA256 {relative} {actual}", flush=True)
    sources = [
        PORT / "network/hosted_rpc.c",
        PORT / "network/test_hosted_rpc_reference.c",
        reference / "common/proto/esp_hosted_rpc.pb-c.c",
        reference / "common/protobuf-c/protobuf-c/protobuf-c.c",
    ]
    for source in sources:
        print(f"SHA256 {source} {hashlib.sha256(source.read_bytes()).hexdigest()}", flush=True)
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="p4-rpc-reference-") as temporary:
        for optimization in (0, 2):
            binary = Path(temporary) / f"oracle-o{optimization}"
            command = shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", f"-O{optimization}",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(PORT / "network"),
                "-I", str(reference / "common/proto"),
                "-I", str(reference / "common/protobuf-c"),
                *map(str, sources), "-o", str(binary),
            ]
            print(f"RUN O{optimization} compile: {shlex.join(command)}", flush=True)
            subprocess.run(command, check=True, env=environment, timeout=60)
            subprocess.run([str(binary)], check=True, env=environment, timeout=60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
