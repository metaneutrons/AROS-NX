#!/usr/bin/env python3
"""Check the native OTA codec against the locally pinned protobuf-c oracle.

This compiles only host test sources in a temporary directory. It never edits
the pinned reference tree and never uses a board or network connection.
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
NATIVE_OTA = PORT / "network/hosted_ota.c"
TEST_SOURCE = PORT / "network/test_hosted_ota_reference.c"
OTA_BEGIN_DEFINE = "#define OTA_REQ_BEGIN 272u"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_reference(reference):
    schema = reference / "common/proto/esp_hosted_rpc.proto"
    if sha256(schema) != SCHEMA_SHA256:
        raise SystemExit("refusing a different OTA RPC schema")
    print(f"PIN SHA256 common/proto/esp_hosted_rpc.proto {SCHEMA_SHA256}",
          flush=True)
    for relative, expected in REFERENCE_HASHES.items():
        source = reference / relative
        actual = sha256(source)
        if actual != expected:
            raise SystemExit(f"refusing a different reference file: {relative}")
        print(f"PIN SHA256 {relative} {actual}", flush=True)


def oracle_sources(reference, native_codec):
    return [
        PORT / "network/hosted_rpc.c",
        native_codec,
        TEST_SOURCE,
        reference / "common/proto/esp_hosted_rpc.pb-c.c",
        reference / "common/protobuf-c/protobuf-c/protobuf-c.c",
    ]


def compile_oracle(reference, native_codec, output, optimization, environment):
    sources = oracle_sources(reference, native_codec)
    for source in sources:
        if not source.is_file():
            raise SystemExit(f"missing oracle source: {source}")
    command = shlex.split(environment.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        f"-O{optimization}", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-I", str(PORT / "network"),
        "-I", str(reference / "common/proto"),
        "-I", str(reference / "common/protobuf-c"),
        *map(str, sources), "-o", str(output),
    ]
    print(f"RUN O{optimization} compile: {shlex.join(command)}", flush=True)
    subprocess.run(command, check=True, env=environment, timeout=120)
    return output


def run_positive_oracles(reference, temporary, environment):
    for source in oracle_sources(reference, NATIVE_OTA):
        print(f"SHA256 {source} {sha256(source)}", flush=True)
    for optimization in (0, 2):
        binary = temporary / f"ota-oracle-o{optimization}"
        compile_oracle(reference, NATIVE_OTA, binary, optimization,
                       environment)
        subprocess.run([str(binary)], check=True, env=environment,
                       timeout=90)


def run_negative_control(reference, temporary, environment):
    original = NATIVE_OTA.read_text(encoding="utf-8")
    if original.count(OTA_BEGIN_DEFINE) != 1:
        raise SystemExit("cannot identify one OTA_BEGIN ID for the negative control")
    mutated_text = original.replace(OTA_BEGIN_DEFINE,
                                    "#define OTA_REQ_BEGIN 271u", 1)
    mutated_source = temporary / "hosted_ota_wrong_begin_id.c"
    mutated_source.write_text(mutated_text, encoding="utf-8")
    print(f"NEGATIVE CONTROL mutation: {OTA_BEGIN_DEFINE} -> "
          "#define OTA_REQ_BEGIN 271u", flush=True)

    binary = temporary / "ota-oracle-negative-control"
    compile_oracle(reference, mutated_source, binary, 0, environment)
    result = subprocess.run([str(binary)], env=environment, timeout=90,
                            text=True, capture_output=True)
    if result.returncode != 1 or "OTA reference line" not in result.stderr:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(
            "negative control did not fail at an OTA oracle assertion")
    if "memcmp(native_bytes, reference_bytes, reference_size) == 0" not in result.stderr:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit("negative control failed for a reason other than byte mismatch")
    print("PASS: altered OTA_BEGIN ID was rejected by the independent byte oracle",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_root", type=Path,
                        help="local pinned ESP-Hosted d518 reference tree")
    arguments = parser.parse_args()
    reference = arguments.reference_root.resolve()
    verify_reference(reference)

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="p4-hosted-ota-reference-") as path:
        temporary = Path(path)
        run_positive_oracles(reference, temporary, environment)
        run_negative_control(reference, temporary, environment)
    return 0


if __name__ == "__main__":
    sys.exit(main())
