#!/usr/bin/env python3
"""Explicit development-core recovery through P4 ROM USB-OTG.

`plan` is entirely offline. `flash --execute` is an opt-in hardware operation;
it never releases devserial, resets, retries the whole flash operation, erases
metadata or boots a chip. The devserial ownership check is point-in-time; it is
not an atomic interprocess lease.
Only the verified legacy ota_0 partition is supported. This is not an OTA or
signed-release updater, and a readback pass is not a successful boot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess


SECTOR = 4096
CORE_OFFSET = 0x20000
CORE_SIZE = 0x800000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def regular_bytes(path, maximum):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError("missing, linked or oversized input")
    data = path.read_bytes()
    if len(data) > maximum:
        raise ValueError("input grew past its limit")
    return data


def esptool_python(esptool_path):
    """Select the explicit interpreter from an esptool console-script shebang."""
    script = Path(esptool_path)
    if (script.is_symlink() or not script.is_file()
            or not os.access(script, os.X_OK)):
        raise ValueError("esptool must be a regular executable console script")
    lines = regular_bytes(script, 4096).splitlines()
    if not lines or not lines[0].startswith(b"#!"):
        raise ValueError("esptool must have a direct Python shebang")
    try:
        words = shlex.split(lines[0][2:].decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError("invalid esptool Python shebang") from error
    if len(words) != 1:
        raise ValueError("esptool shebang must name one absolute interpreter")
    interpreter = Path(words[0])
    if (not interpreter.is_absolute() or not interpreter.is_file()
            or not os.access(interpreter, os.X_OK)):
        raise ValueError("esptool Python interpreter is unavailable")
    return str(interpreter)


def validate_image(data, expected_sha):
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha) or digest(data) != expected_sha:
        raise ValueError("image SHA-256 mismatch")
    if not 24 <= len(data) <= CORE_SIZE or data[0] != 0xE9:
        raise ValueError("not a bounded Espressif image")
    if struct.unpack_from("<H", data, 12)[0] != 18:
        raise ValueError("image is not for ESP32-P4")
    segments = data[1]
    if not 1 <= segments <= 16 or data[23] != 1:
        raise ValueError("invalid segments or missing appended image digest")
    pos, checksum = 24, 0xEF
    for _ in range(segments):
        if pos + 8 > len(data):
            raise ValueError("truncated segment header")
        _, size = struct.unpack_from("<II", data, pos)
        pos += 8
        if size > len(data) - pos:
            raise ValueError("truncated segment")
        for byte in data[pos:pos + size]:
            checksum ^= byte
        pos += size
    checksum_pos = ((pos + 16) // 16) * 16 - 1
    end = checksum_pos + 1
    if end + 32 != len(data) or data[checksum_pos] != checksum:
        raise ValueError("image checksum/length mismatch")
    if hashlib.sha256(data[:end]).digest() != data[end:]:
        raise ValueError("appended image digest mismatch")


def validate_table(table, flash_size):
    if len(table) != SECTOR or flash_size not in (16 << 20, 32 << 20):
        raise ValueError("unsupported flash/table size")
    entries, md5_seen = [], False
    for pos in range(0, 0xC00, 32):
        record = table[pos:pos + 32]
        magic = struct.unpack_from("<H", record)[0]
        if magic == 0xEBEB:
            if record[2:16] != b"\xff" * 14:
                raise ValueError("invalid table digest marker")
            if hashlib.md5(table[:pos]).digest() != record[16:]:
                raise ValueError("partition-table digest mismatch")
            if table[pos + 32:] != b"\xff" * (len(table) - pos - 32):
                raise ValueError("unexpected trailing table data")
            md5_seen = True
            break
        if magic != 0x50AA:
            raise ValueError("missing table checksum or malformed entry")
        _, kind, subtype, offset, size, raw_name, flags = struct.unpack(
            "<HBBII16sI", record)
        if not size or offset < 0x9000 or offset % SECTOR or size % SECTOR:
            raise ValueError("invalid partition extent")
        if offset + size > flash_size:
            raise ValueError("partition extends beyond flash")
        name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="strict")
        if not name or any(name == entry[0] for entry in entries):
            raise ValueError("empty or duplicate partition name")
        if any(offset < entry[3] + entry[4] and entry[3] < offset + size
               for entry in entries):
            raise ValueError("overlapping partitions")
        entries.append((name, kind, subtype, offset, size, flags))
    if not md5_seen:
        raise ValueError("partition-table checksum is required")
    core = [entry for entry in entries if entry[:3] == ("ota_0", 0, 0x10)]
    if len(core) != 1 or core[0][3:] != (CORE_OFFSET, CORE_SIZE, 0):
        raise ValueError("only unencrypted legacy ota_0 is supported; no layout migration")
    if any(entry[1] == 0 and entry != core[0] for entry in entries):
        raise ValueError("multiple application slots require a different updater")


def payload_for(image, before, table, flash_size):
    validate_table(table, flash_size)
    span = ((len(image) + SECTOR - 1) // SECTOR) * SECTOR
    if not image or span > CORE_SIZE or len(before) != span:
        raise ValueError("pre-read must cover exactly the affected erase sectors")
    return image + before[len(image):]


def verify_identity(output, mac, security=False):
    macs = {item.lower() for item in re.findall(
        r"^MAC:\s*([0-9a-fA-F:]{17})\s*$", output, re.M)}
    modes = set(re.findall(r"^USB mode:\s*(.*?)\s*$", output, re.M))
    if macs != {mac} or modes != {"USB-OTG"}:
        raise ValueError("wrong/ambiguous chip identity or not native ROM USB-OTG")
    if not re.search(r"^Chip type:\s+ESP32-P4 \(", output, re.M):
        raise ValueError("not an identified ESP32-P4")
    if security:
        flags = re.findall(r"^Flags:\s*(0x[0-9a-fA-F]+)", output, re.M)
        if len(flags) != 1 or int(flags[0], 16) != 0:
            raise ValueError("secured/unknown download state; no writes permitted")
        for label in ("Secure Boot", "Flash Encryption"):
            if re.findall(r"^" + label + r":\s*(.*?)\s*$", output, re.M) != ["Disabled"]:
                raise ValueError("security state must be unambiguously disabled")


class Runner:
    def __init__(self, esptool, port, directory, python=None):
        self.esptool, self.port, self.directory = esptool, port, directory
        self.python = python

    def idle(self):
        # This refuses a current devserial owner; the check does not reserve the
        # port against another process claiming it immediately afterward.
        completed = subprocess.run(["devserial", "list", "--json"], check=True,
                                   capture_output=True, text=True, timeout=15)
        state = json.loads(completed.stdout)
        if not isinstance(state.get("managed"), list):
            raise ValueError("devserial ownership unavailable")
        if any(item.get("name") == self.port for item in state["managed"]):
            raise ValueError("port belongs to devserial; request an explicit scoped release first")

    def command(self, label, arguments, no_stub=False):
        self.idle()
        interpreter = self.python or esptool_python(self.esptool)
        wrapper = Path(__file__).with_name("hs_esptool_once.py")
        command = [interpreter, str(wrapper), "--chip", "esp32p4",
                   "--port", self.port, "--before", "no-reset",
                   "--after", "no-reset"]
        if no_stub:
            command.append("--no-stub")
        command.extend(arguments)
        # Exclusive log files and no shell, timeout or automatic whole-write retry.
        with (self.directory / (label + ".log")).open("xb") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT,
                           timeout=120, env={**os.environ, "LC_ALL": "C"})
        return regular_bytes(self.directory / (label + ".log"), 1 << 20).decode(
            "utf-8", errors="strict").replace("\r", "")


def recover(runner, image, expected_sha, mac, directory):
    """Runner is injectable for tests; it must not reset/retry the whole write."""
    validate_image(image, expected_sha)
    report = {"status": "NOT_VERIFIED", "write_attempted": False,
              "image_sha256": expected_sha, "mac": mac,
              "offset": CORE_OFFSET, "boot_verified": False}
    try:
        info = runner.command("security", ["get-security-info"], no_stub=True)
        verify_identity(info, mac, security=True)
        info = runner.command("flash-id", ["flash-id"], no_stub=True)
        verify_identity(info, mac)
        sizes = re.findall(r"^Detected flash size:\s*(16|32)MB\s*$", info, re.M)
        if len(sizes) != 1:
            raise ValueError("flash size is unknown or ambiguous")
        flash_size = int(sizes[0]) << 20
        table_path, before_path = directory / "table.bin", directory / "before.bin"
        span = ((len(image) + SECTOR - 1) // SECTOR) * SECTOR
        for label, offset, size, path in (
                ("table", 0x8000, SECTOR, table_path),
                ("before", CORE_OFFSET, span, before_path)):
            info = runner.command(label, ["read-flash", hex(offset), hex(size), str(path)])
            verify_identity(info, mac)
            if len(regular_bytes(path, size)) != size:
                raise ValueError("incomplete pre-read")
            if label == "table":
                validate_table(regular_bytes(path, SECTOR), flash_size)
        table = regular_bytes(table_path, SECTOR)
        before = regular_bytes(before_path, span)
        payload = payload_for(image, before, table, flash_size)
        payload_path = directory / "payload.bin"
        with payload_path.open("xb") as stream:
            stream.write(payload)
        report.update(span=span, before_sha256=digest(before),
                      table_sha256=digest(table), payload_sha256=digest(payload))
        # Verify the table again immediately before the sole write attempt.
        table_check = directory / "table-check.bin"
        info = runner.command("table-check", ["read-flash", "0x8000", "0x1000", str(table_check)])
        verify_identity(info, mac)
        if regular_bytes(table_check, SECTOR) != table:
            raise ValueError("live layout changed; refusing write")
        before_check = directory / "before-check.bin"
        info = runner.command(
            "before-check",
            ["read-flash", hex(CORE_OFFSET), hex(span), str(before_check)])
        verify_identity(info, mac)
        if regular_bytes(before_check, span) != before:
            raise ValueError("live core sectors changed; refusing write")
        report["write_attempted"] = True
        info = runner.command("write", ["write-flash", hex(CORE_OFFSET), str(payload_path)])
        verify_identity(info, mac)
        if info.count("Hash of data verified") != 1:
            raise ValueError("writer did not report exactly one verified range")
        after_path = directory / "after.bin"
        info = runner.command("after", ["read-flash", hex(CORE_OFFSET), hex(span), str(after_path)])
        verify_identity(info, mac)
        after = regular_bytes(after_path, span)
        if after != payload:
            raise ValueError("full independent readback mismatch")
        report.update(status="VERIFIED_READBACK", readback_sha256=digest(after))
    except Exception:
        # Ambiguous/partial writes are not retried as whole operations or rolled back.
        report["status"] = "WRITE_UNCERTAIN" if report["write_attempted"] else "REFUSED"
        raise
    finally:
        with (directory / "result.json").open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    for name in ("plan", "flash"):
        sub = commands.add_parser(name)
        sub.add_argument("--image", type=Path, required=True)
        sub.add_argument("--sha256", required=True)
        sub.add_argument("--output", type=Path, required=True, help="new evidence directory")
        if name == "plan":
            sub.add_argument("--table", type=Path, required=True)
            sub.add_argument("--before", type=Path, required=True)
            sub.add_argument("--flash-mib", type=int, choices=(16, 32), required=True)
        else:
            sub.add_argument("--port", required=True)
            sub.add_argument("--mac", required=True)
            sub.add_argument(
                "--esptool", required=True,
                help="absolute esptool 5.5.0 console script in its pinned Python environment")
            sub.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    image = regular_bytes(args.image, CORE_SIZE)
    validate_image(image, args.sha256)
    if args.mode == "flash":
        if not args.execute or not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", args.mac):
            parser.error("flash requires --execute and an explicit lowercase expected MAC")
        if not Path(args.esptool).is_absolute():
            parser.error("--esptool must be an explicit absolute executable path")
        try:
            python = esptool_python(args.esptool)
        except (OSError, ValueError) as error:
            parser.error(str(error))
    args.output.mkdir(exist_ok=False)
    if args.mode == "plan":
        table = regular_bytes(args.table, SECTOR)
        before = regular_bytes(args.before, CORE_SIZE)
        payload = payload_for(image, before, table, args.flash_mib << 20)
        with (args.output / "payload.bin").open("xb") as stream:
            stream.write(payload)
        result = {"status": "OFFLINE_PLAN_ONLY", "offset": CORE_OFFSET,
                  "span": len(payload), "image_sha256": digest(image),
                  "payload_sha256": digest(payload), "hardware_verified": False}
        with (args.output / "result.json").open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2)
    else:
        result = recover(Runner(args.esptool, args.port, args.output, python), image,
                         args.sha256, args.mac, args.output)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
