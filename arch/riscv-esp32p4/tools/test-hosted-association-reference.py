#!/usr/bin/env python3
"""Compare the association codec with pinned ESP-Hosted protobuf-c.

The default mode compiles with the local C compiler. Pass --colima to run the
same pinned-source O0/O2 sanitizer oracle in the existing Colima image with
networking disabled. Selected source files are streamed into an ephemeral
container snapshot and marked non-writable for the test run; this is not an
immutable filesystem guarantee. Build products stay under the container's
temporary filesystem.
"""
import argparse
import hashlib
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

SCHEMA_SHA256 = "bbdbc9bb69c2b1793de6441ea55051c3ceb3f7b30994227b8ff1e995b0137dec"
REFERENCE_HASHES = {
    "common/proto/esp_hosted_rpc.pb-c.c": "693efbce5db14bf6f2416f5fcf746049a459764ec9f8571a45f2b6a7bdffcc69",
    "common/proto/esp_hosted_rpc.pb-c.h": "ad4f03ade1ca38d88354cc3104f1f4838f7c25004197e19a8fac395c1ad05e85",
    "common/protobuf-c/protobuf-c/protobuf-c.c": "5af483efb77b6c60afce921ff361cff5320b2a80698f6e7fb0356c49b48933a7",
    "common/protobuf-c/protobuf-c/protobuf-c.h": "e1c6306390090e4468472dbed3928e6983d1be3306d2006ea3fa49696aef6946",
}
PORT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = "aros-esp32p4-builder:latest"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_reference(reference):
    schema = reference / "common/proto/esp_hosted_rpc.proto"
    if sha256(schema) != SCHEMA_SHA256:
        raise SystemExit("refusing a different RPC schema")
    print(f"PIN SHA256 common/proto/esp_hosted_rpc.proto {SCHEMA_SHA256}",
          flush=True)
    for relative, expected in REFERENCE_HASHES.items():
        source = reference / relative
        actual = sha256(source)
        if actual != expected:
            raise SystemExit(f"refusing a different reference file: {relative}")
        print(f"PIN SHA256 {relative} {actual}", flush=True)


def run_oracle(reference):
    sources = [
        PORT / "network/hosted_rpc.c",
        PORT / "network/hosted_association.c",
        PORT / "network/test_hosted_association_reference.c",
        reference / "common/proto/esp_hosted_rpc.pb-c.c",
        reference / "common/protobuf-c/protobuf-c/protobuf-c.c",
    ]
    for source in sources:
        if not source.is_file():
            raise SystemExit(f"missing oracle source: {source}")
        print(f"SHA256 {source} {sha256(source)}", flush=True)

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="p4-hosted-association-reference-") as temporary:
        for optimization in (0, 2):
            binary = Path(temporary) / f"oracle-o{optimization}"
            command = shlex.split(environment.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-O{optimization}", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I", str(PORT / "network"),
                "-I", str(reference / "common/proto"),
                "-I", str(reference / "common/protobuf-c"),
                *map(str, sources), "-o", str(binary),
            ]
            print(f"RUN O{optimization} compile: {shlex.join(command)}",
                  flush=True)
            subprocess.run(command, check=True, env=environment, timeout=180)
            subprocess.run([str(binary)], check=True, env=environment,
                           timeout=90)


def run_in_colima(reference, image):
    docker = shutil.which("docker")
    if docker is None:
        raise SystemExit("docker is required for --colima")
    context = subprocess.run([docker, "context", "show"], check=True,
                             text=True, capture_output=True).stdout.strip()
    if context != "colima":
        raise SystemExit(f"refusing non-Colima Docker context: {context!r}")
    subprocess.run([docker, "image", "inspect", image], check=True,
                   stdout=subprocess.DEVNULL)

    port_files = [
        "network/hosted_rpc.c",
        "network/hosted_rpc.h",
        "network/hosted_association.c",
        "network/hosted_association.h",
        "network/test_hosted_association_reference.c",
        "tools/test-hosted-association-reference.py",
    ]
    reference_files = ["common/proto/esp_hosted_rpc.proto",
                       *REFERENCE_HASHES.keys()]
    archive_buffer = io.BytesIO()
    with tarfile.open(fileobj=archive_buffer, mode="w") as archive:
        for relative in port_files:
            archive.add(PORT / relative, arcname=f"port/{relative}")
        for relative in reference_files:
            archive.add(reference / relative,
                        arcname=f"reference/{relative}")
    archive_bytes = archive_buffer.getvalue()

    command = [
        docker, "run", "--rm", "-i", "--network", "none",
        "--tmpfs", "/tmp:rw,nosuid,nodev,exec,size=128m",
        image, "sh", "-c",
        "mkdir -p /work && tar -xf - -C /work && "
        "chmod -R a-w /work/port /work/reference && "
        "python3 /work/port/tools/test-hosted-association-reference.py "
        "/work/reference --inside-colima",
    ]
    print("RUN isolated Colima oracle: " + shlex.join(command), flush=True)
    subprocess.run(command, input=archive_bytes, check=True, timeout=360)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_root", type=Path,
                        help="local pinned ESP-Hosted d518 reference tree")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--colima", action="store_true",
                      help="run in the current Colima Docker context, --network none")
    mode.add_argument("--inside-colima", action="store_true",
                      help=argparse.SUPPRESS)
    parser.add_argument("--image", default=os.environ.get(
        "P4_HOSTED_ORACLE_IMAGE", DEFAULT_IMAGE),
        help="existing local Linux builder image used by --colima")
    arguments = parser.parse_args()
    reference = arguments.reference_root.resolve()
    verify_reference(reference)

    if arguments.colima:
        run_in_colima(reference, arguments.image)
        return 0
    if arguments.inside_colima and not sys.platform.startswith("linux"):
        raise SystemExit("--inside-colima requires Linux")
    if arguments.inside_colima and os.environ.get("P4_HOSTED_ORACLE_COLIMA"):
        raise SystemExit("nested Colima invocation refused")
    run_oracle(reference)
    return 0


if __name__ == "__main__":
    sys.exit(main())
