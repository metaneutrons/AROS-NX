#!/usr/bin/env python3
"""Explicit USB host client for the opt-in AROS RemoteDiag service."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Any

MAGIC = b"RDG1"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIQI")
HEADER_SIZE = HEADER.size
REQUEST_MAX = 1024
DATA_MAX = 4096
SNAPSHOT_MAX = 4 * 1024 * 1024
PATH_MAX = 512
FILE_ENUM_MAX = 128
PRODUCT_NAME = "P4 USB echo prototype"
VENDOR_INTERFACE = 0
CDC_CONTROL_INTERFACE = 1
CDC_DATA_INTERFACE = 2
VENDOR_OUT_ENDPOINT = 0x01
VENDOR_IN_ENDPOINT = 0x81
CDC_NOTIFY_ENDPOINT = 0x82
CDC_OUT_ENDPOINT = 0x03
CDC_IN_ENDPOINT = 0x83
USB_BULK_PACKET_SIZES = (64, 512)

RD_HELLO = 1
RD_STATUS = 2
RD_CAPTURE = 3
RD_CHUNK = 4
RD_RELEASE = 5
RD_MOVE = 6
RD_CLICK = 7
RD_STAT = 8
RD_LIST = 9
RD_READ = 10

RD_OK = 0
RD_BAD_REQUEST = 1
RD_DENIED = 2
RD_UNAVAILABLE = 3
RD_IO = 4
RD_CHANGED = 5
RD_LIMIT = 6
RD_SESSION = 7
RD_BUSY = 8
RD_END = 9

CAP_SCREEN = 1
CAP_INPUT = 2
CAP_FILES = 4
CAP_MASK = CAP_SCREEN | CAP_INPUT | CAP_FILES
BUTTONS = {"left": 1, "right": 2, "middle": 3}
STATUS_NAMES = {
    RD_OK: "ok",
    RD_BAD_REQUEST: "bad request",
    RD_DENIED: "denied",
    RD_UNAVAILABLE: "unavailable",
    RD_IO: "I/O error",
    RD_CHANGED: "source changed",
    RD_LIMIT: "limit exceeded",
    RD_SESSION: "invalid session or sequence",
    RD_BUSY: "busy",
    RD_END: "end of listing",
}


class DiagError(RuntimeError):
    """A local, USB, or protocol failure safe to show without payload data."""


class ProtocolError(DiagError):
    pass


class ServerError(DiagError):
    def __init__(self, status: int):
        self.status = status
        super().__init__(f"server returned {STATUS_NAMES.get(status, f'status {status}')}")


def _u32(value: int, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFFFFFF:
        raise DiagError(f"{label} must be an unsigned 32-bit integer")
    return value


def encode_frame(opcode: int, request_id: int, session: int, body: bytes = b"") -> bytes:
    if not 0 <= opcode <= 0x7FFF:
        raise ProtocolError("invalid request opcode")
    if not 0 <= request_id <= 0xFFFFFFFF or not 0 <= session <= 0xFFFFFFFFFFFFFFFF:
        raise ProtocolError("invalid request identity")
    body = bytes(body)
    if len(body) > REQUEST_MAX:
        raise ProtocolError("request body exceeds protocol limit")
    prefix = HEADER.pack(MAGIC, VERSION, opcode, 0, len(body), request_id, session, 0)
    checksum = zlib.crc32(prefix[:28] + body) & 0xFFFFFFFF
    return prefix[:28] + struct.pack("<I", checksum) + body


def encode_path(path: str) -> bytes:
    if not isinstance(path, str):
        raise DiagError("path must be text")
    try:
        raw = path.encode("latin-1", errors="strict")
    except UnicodeEncodeError as exc:
        raise DiagError("path contains a character outside native Latin-1") from exc
    if len(raw) >= PATH_MAX:
        raise DiagError("path exceeds the native path limit")
    if any(byte < 32 or byte == 127 or byte in (ord(":"), ord("\\")) for byte in raw):
        raise DiagError("path contains a forbidden native path character")
    if raw:
        components = raw.split(b"/")
        if any(component in (b"", b".", b"..") for component in components):
            raise DiagError("path must be relative and cannot contain empty, . or .. components")
    return raw + b"\0"


def decode_metadata(body: bytes) -> dict[str, Any]:
    if len(body) != 212:
        raise ProtocolError("metadata response has the wrong length")
    protection, size = struct.unpack_from("<II", body, 0)
    file_type, days, minutes, ticks = struct.unpack_from("<iiii", body, 8)
    name = body[24:132].split(b"\0", 1)[0].decode("latin-1", errors="strict")
    comment = body[132:212].split(b"\0", 1)[0].decode("latin-1", errors="strict")
    return {
        "protection": protection,
        "size": size,
        "type": file_type,
        "days": days,
        "minutes": minutes,
        "ticks": ticks,
        "name": name,
        "comment": comment,
    }


def write_exclusive(path: str | os.PathLike[str], data: bytes) -> None:
    """Create a new local output file without replacing an existing path."""
    created = False
    try:
        with open(path, "xb") as stream:
            created = True
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except Exception:
        if created:
            try:
                Path(path).unlink()
            except OSError:
                pass
        raise


class ProtocolClient:
    """One ordered session over a byte-streaming bulk transport.

    The transport methods are ``write(bytes, timeout_ms) -> count`` and
    ``read(maximum, timeout_ms) -> bytes``.  Any uncertain transfer poisons
    the session; no request is retried or resent.
    """

    def __init__(self, transport: Any, *, timeout: float = 15.0,
                 allow_input: bool = False, clock=time.monotonic,
                 random_cookie=secrets.randbits):
        if not 0 < timeout <= 60:
            raise DiagError("timeout must be between 0 and 60 seconds")
        self.transport = transport
        self.timeout = timeout
        self.allow_input = allow_input
        self.clock = clock
        self.random_cookie = random_cookie
        self.session = 0
        self.sequence = 0
        self.chunk_size = 0
        self.snapshot_max = 0
        self.generation = 0
        self.capabilities: int | None = None
        self.poisoned = False

    def _remaining_ms(self, deadline: float, last: list[float]) -> int:
        now = self.clock()
        if now < last[0] or now >= deadline:
            self.poisoned = True
            raise ProtocolError("USB operation timed out or clock regressed; transfer state is uncertain")
        last[0] = now
        return max(1, int((deadline - now) * 1000))

    def _write_all(self, data: bytes, deadline: float, last: list[float]) -> None:
        offset = 0
        while offset < len(data):
            remaining = self._remaining_ms(deadline, last)
            try:
                count = self.transport.write(data[offset:], remaining)
            except Exception as exc:
                self.poisoned = True
                raise ProtocolError("USB write failed; request will not be replayed") from exc
            if not isinstance(count, int) or count <= 0 or count > len(data) - offset:
                self.poisoned = True
                raise ProtocolError("invalid or zero partial USB write; request will not be replayed")
            offset += count

    def _read_exact(self, count: int, deadline: float, last: list[float]) -> bytes:
        result = bytearray()
        while len(result) < count:
            remaining = self._remaining_ms(deadline, last)
            try:
                part = self.transport.read(count - len(result), remaining)
            except Exception as exc:
                self.poisoned = True
                raise ProtocolError("USB read failed; response state is uncertain") from exc
            if not isinstance(part, (bytes, bytearray, memoryview)):
                self.poisoned = True
                raise ProtocolError("USB read returned a non-byte result")
            part = bytes(part)
            if not part or len(part) > count - len(result):
                self.poisoned = True
                raise ProtocolError("invalid or zero partial USB read")
            result.extend(part)
        return bytes(result)

    def _exchange(self, opcode: int, request_id: int, request_session: int,
                  expected_session: int, body: bytes = b"") -> tuple[int, bytes]:
        if self.poisoned:
            raise ProtocolError("session is closed after an uncertain transfer")
        frame = encode_frame(opcode, request_id, request_session, body)
        now = self.clock()
        deadline = now + self.timeout
        last = [now]
        try:
            self._write_all(frame, deadline, last)
            header = self._read_exact(HEADER_SIZE, deadline, last)
            magic, version, response_op, status, length, response_id, response_session, checksum = HEADER.unpack(header)
            if magic != MAGIC or version != VERSION:
                raise ProtocolError("invalid response magic or version")
            if response_op != (opcode | 0x8000):
                raise ProtocolError("response opcode does not match request")
            if response_id != request_id or response_session != expected_session:
                raise ProtocolError("response session or sequence does not match request")
            if length > DATA_MAX:
                raise ProtocolError("response body exceeds protocol limit")
            payload = self._read_exact(length, deadline, last) if length else b""
            if (zlib.crc32(header[:28] + payload) & 0xFFFFFFFF) != checksum:
                raise ProtocolError("response CRC mismatch")
            if status not in STATUS_NAMES:
                raise ProtocolError("response contains an unknown status")
            self._remaining_ms(deadline, last)
            return status, payload
        except Exception:
            self.poisoned = True
            raise

    def hello(self) -> dict[str, int]:
        if self.session or self.sequence:
            raise DiagError("HELLO has already been completed")
        cookie = self.random_cookie(64)
        if not isinstance(cookie, int) or not 0 < cookie <= 0xFFFFFFFFFFFFFFFF:
            raise DiagError("random session cookie source returned an invalid value")
        status, body = self._exchange(RD_HELLO, 1, 0, cookie, struct.pack("<Q", cookie))
        if status != RD_OK:
            self.poisoned = True
            raise ServerError(status)
        if len(body) != 16:
            self.poisoned = True
            raise ProtocolError("HELLO response has the wrong length")
        version, chunk_size, snapshot_max, generation = struct.unpack("<IIII", body)
        if (version != VERSION or not 0 < chunk_size <= DATA_MAX or
                not 0 < snapshot_max <= SNAPSHOT_MAX or generation == 0):
            self.poisoned = True
            raise ProtocolError("HELLO response advertises invalid limits")
        self.session = cookie
        self.sequence = 1
        self.chunk_size = chunk_size
        self.snapshot_max = snapshot_max
        self.generation = generation
        return {"version": version, "chunk_size": chunk_size,
                "snapshot_max": snapshot_max, "generation": generation}

    def _call(self, opcode: int, body: bytes = b"") -> tuple[int, bytes]:
        if not self.session or not self.sequence:
            raise DiagError("HELLO must complete before protocol operations")
        if self.sequence == 0xFFFFFFFF:
            raise DiagError("request sequence exhausted; start a new USB session")
        request_id = self.sequence + 1
        self.sequence = request_id
        status, payload = self._exchange(opcode, request_id, self.session, self.session, body)
        return status, payload

    def _expect(self, status: int, payload: bytes, expected_length: int | None = None) -> bytes:
        if status != RD_OK:
            if payload:
                self.poisoned = True
                raise ProtocolError("error response unexpectedly has a body")
            raise ServerError(status)
        if expected_length is not None and len(payload) != expected_length:
            self.poisoned = True
            raise ProtocolError("response body has the wrong length")
        return payload

    def status(self) -> dict[str, Any]:
        payload = self._expect(*self._call(RD_STATUS), expected_length=8)
        capabilities, snapshot_id = struct.unpack("<II", payload)
        if capabilities & ~CAP_MASK:
            self.poisoned = True
            raise ProtocolError("STATUS response contains unknown capability bits")
        self.capabilities = capabilities
        return {"capabilities": {
            "screen": bool(capabilities & CAP_SCREEN),
            "input": bool(capabilities & CAP_INPUT),
            "files": bool(capabilities & CAP_FILES),
        }, "snapshot_id": snapshot_id}

    def _require_input(self) -> None:
        if not self.allow_input:
            raise DiagError("move/click requires the explicit --allow-input flag")
        if self.capabilities is None:
            self.status()
        if not self.capabilities & CAP_INPUT:
            raise DiagError("server has not enabled input capability")

    def move(self, x: int, y: int) -> None:
        self._require_input()
        payload = struct.pack("<II", _u32(x, "x"), _u32(y, "y"))
        self._expect(*self._call(RD_MOVE, payload), expected_length=0)

    def click(self, x: int, y: int, button: str | int) -> None:
        self._require_input()
        button_number = BUTTONS.get(button, button) if isinstance(button, str) else button
        if (isinstance(button_number, bool) or not isinstance(button_number, int) or
                button_number not in (1, 2, 3)):
            raise DiagError("button must be left, right, or middle")
        payload = struct.pack("<III", _u32(x, "x"), _u32(y, "y"), button_number)
        self._expect(*self._call(RD_CLICK, payload), expected_length=0)

    def _snapshot_metadata(self, opcode: int, request_body: bytes,
                           *, capture: bool, expected_length: int | None = None) -> tuple[int, int, int, int, int]:
        status, payload = self._call(opcode, request_body)
        self._expect(status, payload, 20)
        snapshot_id, size, width, height, checksum = struct.unpack("<IIIII", payload)
        if (snapshot_id == 0 or size > self.snapshot_max or
                (expected_length is not None and size != expected_length)):
            self.poisoned = True
            raise ProtocolError("snapshot metadata exceeds or contradicts requested bounds")
        if capture:
            if width == 0 or height == 0 or width * height * 3 != size:
                self.poisoned = True
                raise ProtocolError("capture metadata is not a complete RGB24 image")
        elif width or height:
            self.poisoned = True
            raise ProtocolError("file snapshot unexpectedly has image dimensions")
        return snapshot_id, size, width, height, checksum

    def _release(self, snapshot_id: int) -> None:
        self._expect(*self._call(RD_RELEASE, struct.pack("<I", snapshot_id)), expected_length=0)

    def _read_snapshot(self, metadata: tuple[int, int, int, int, int]) -> bytes:
        snapshot_id, size, _width, _height, expected_crc = metadata
        data = bytearray()
        try:
            while len(data) < size:
                count = min(self.chunk_size, size - len(data))
                body = struct.pack("<III", snapshot_id, len(data), count)
                chunk = self._expect(*self._call(RD_CHUNK, body))
                if len(chunk) != count:
                    self.poisoned = True
                    raise ProtocolError("snapshot chunk has the wrong length")
                data.extend(chunk)
            if (zlib.crc32(data) & 0xFFFFFFFF) != expected_crc:
                self.poisoned = True
                raise ProtocolError("complete snapshot CRC mismatch")
        except Exception:
            if not self.poisoned:
                try:
                    self._release(snapshot_id)
                except Exception:
                    pass
            raise
        self._release(snapshot_id)
        return bytes(data)

    def capture(self, x: int = 0, y: int = 0, width: int = 0,
                height: int = 0) -> tuple[int, int, bytes]:
        coords = tuple(_u32(value, label) for value, label in
                       ((x, "x"), (y, "y"), (width, "width"), (height, "height")))
        if (width == 0 or height == 0) and coords != (0, 0, 0, 0):
            raise DiagError("crop width and height must both be nonzero")
        metadata = self._snapshot_metadata(RD_CAPTURE, struct.pack("<IIII", *coords), capture=True)
        return metadata[2], metadata[3], self._read_snapshot(metadata)

    def stat(self, path: str) -> dict[str, Any]:
        payload = self._expect(*self._call(RD_STAT, encode_path(path)), expected_length=212)
        return decode_metadata(payload)

    def list_one(self, path: str, index: int) -> dict[str, Any] | None:
        index = _u32(index, "index")
        body = struct.pack("<I", index) + encode_path(path)
        status, payload = self._call(RD_LIST, body)
        if status == RD_END:
            if payload:
                self.poisoned = True
                raise ProtocolError("end-of-list response unexpectedly has a body")
            return None
        return decode_metadata(self._expect(status, payload, expected_length=212))

    def read_file(self, path: str, offset: int, length: int) -> bytes:
        offset = _u32(offset, "offset")
        length = _u32(length, "length")
        if length > self.snapshot_max or offset + length > 0x7FFFFFFF:
            raise DiagError("READ range exceeds the snapshot limit")
        request = struct.pack("<II", offset, length) + encode_path(path)
        metadata = self._snapshot_metadata(RD_READ, request, capture=False,
                                           expected_length=length)
        return self._read_snapshot(metadata)


class USBBulkTransport:
    def __init__(self, device: Any, usb_util: Any, interface_number: int,
                 out_endpoint: int, in_endpoint: int):
        self.device = device
        self.usb_util = usb_util
        self.interface_number = interface_number
        self.out_endpoint = out_endpoint
        self.in_endpoint = in_endpoint
        self.claimed = False

    def open(self) -> None:
        # Deliberately do not set configuration, detach a driver, or reset.
        try:
            self.usb_util.claim_interface(self.device, self.interface_number)
            self.claimed = True
        except Exception:
            self.usb_util.dispose_resources(self.device)
            raise

    def write(self, data: bytes, timeout_ms: int) -> int:
        return self.device.write(self.out_endpoint, data, timeout=timeout_ms)

    def read(self, maximum: int, timeout_ms: int) -> bytes:
        return bytes(self.device.read(self.in_endpoint, maximum, timeout=timeout_ms))

    def close(self) -> None:
        try:
            if self.claimed:
                self.usb_util.release_interface(self.device, self.interface_number)
                self.claimed = False
        finally:
            self.usb_util.dispose_resources(self.device)

    def __enter__(self) -> "USBBulkTransport":
        self.open()
        return self

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        self.close()


def _configuration_descriptors(device: Any) -> bytes:
    """Read configuration 0 without changing device configuration or claiming an interface."""
    header = bytes(device.ctrl_transfer(0x80, 0x06, 0x0200, 0, 9, timeout=1000))
    if len(header) != 9 or header[0] != 9 or header[1] != 0x02:
        raise DiagError("unexpected USB configuration descriptor header")
    total_length = int.from_bytes(header[2:4], "little")
    if total_length != 98:
        raise DiagError("unexpected USB composite configuration length")
    descriptor = bytes(device.ctrl_transfer(0x80, 0x06, 0x0200, 0,
                                            total_length, timeout=1000))
    if len(descriptor) != total_length:
        raise DiagError("incomplete USB configuration descriptor")
    return descriptor


def _validate_configuration_descriptor(descriptor: bytes) -> int:
    """Validate the complete fixed vendor-bulk plus CDC ACM descriptor tree."""
    if (len(descriptor) != 98 or descriptor[:2] != b"\x09\x02" or
            int.from_bytes(descriptor[2:4], "little") != len(descriptor) or
            descriptor[4:9] != bytes((3, 1, 0, 0x80, 50))):
        raise DiagError("unexpected USB composite configuration header")

    entries: list[bytes] = []
    offset = 0
    while offset < len(descriptor):
        if offset + 2 > len(descriptor):
            raise DiagError("truncated USB configuration descriptor")
        length = descriptor[offset]
        if length < 2 or offset + length > len(descriptor):
            raise DiagError("invalid USB configuration descriptor length")
        entries.append(descriptor[offset:offset + length])
        offset += length
    if offset != len(descriptor) or len(entries) != 14:
        raise DiagError("unexpected USB composite descriptor count")

    bulk_packet_size = int.from_bytes(entries[2][4:6], "little")
    if bulk_packet_size not in USB_BULK_PACKET_SIZES:
        raise DiagError("unexpected USB bulk packet size")

    def endpoint(address: int, transfer_type: int, packet_size: int,
                 interval: int) -> bytes:
        return bytes((7, 5, address, transfer_type, packet_size & 0xFF,
                      packet_size >> 8, interval))

    expected = [
        bytes((9, 2, 98, 0, 3, 1, 0, 0x80, 50)),
        bytes((9, 4, VENDOR_INTERFACE, 0, 2, 0xFF, 0, 0, 0)),
        endpoint(VENDOR_OUT_ENDPOINT, 2, bulk_packet_size, 0),
        endpoint(VENDOR_IN_ENDPOINT, 2, bulk_packet_size, 0),
        bytes((8, 0x0B, CDC_CONTROL_INTERFACE, 2, 2, 2, 0, 0)),
        bytes((9, 4, CDC_CONTROL_INTERFACE, 0, 1, 2, 2, 0, 0)),
        bytes((5, 0x24, 0, 0x20, 0x01)),
        bytes((5, 0x24, 1, 0, CDC_DATA_INTERFACE)),
        bytes((4, 0x24, 2, 6)),
        bytes((5, 0x24, 6, CDC_CONTROL_INTERFACE, CDC_DATA_INTERFACE)),
        endpoint(CDC_NOTIFY_ENDPOINT, 3, 8, 1),
        bytes((9, 4, CDC_DATA_INTERFACE, 0, 2, 0x0A, 0, 0, 0)),
        endpoint(CDC_OUT_ENDPOINT, 2, bulk_packet_size, 0),
        endpoint(CDC_IN_ENDPOINT, 2, bulk_packet_size, 0),
    ]
    if entries != expected:
        raise DiagError("unexpected USB interface, CDC function, or endpoint descriptors")
    return bulk_packet_size


def _validate_interface_metadata(configuration: Any, packet_size: int) -> None:
    """Cross-check PyUSB's parsed view before making the vendor claim."""
    if (getattr(configuration, "bConfigurationValue", None) != 1 or
            getattr(configuration, "bNumInterfaces", None) != 3 or
            getattr(configuration, "wTotalLength", None) != 98):
        raise DiagError("unexpected active USB configuration metadata")
    interfaces = list(configuration)
    if len(interfaces) != 3:
        raise DiagError("unexpected number of active USB interfaces")
    expected = {
        VENDOR_INTERFACE: (0xFF, 0, 0, {
            VENDOR_OUT_ENDPOINT: (2, packet_size),
            VENDOR_IN_ENDPOINT: (2, packet_size),
        }),
        CDC_CONTROL_INTERFACE: (2, 2, 0, {CDC_NOTIFY_ENDPOINT: (3, 8)}),
        CDC_DATA_INTERFACE: (0x0A, 0, 0, {
            CDC_OUT_ENDPOINT: (2, packet_size),
            CDC_IN_ENDPOINT: (2, packet_size),
        }),
    }
    seen: set[int] = set()
    for interface in interfaces:
        number = interface.bInterfaceNumber
        if (number not in expected or number in seen or
                interface.bAlternateSetting != 0):
            raise DiagError("unexpected active USB interface numbering or alternate setting")
        seen.add(number)
        class_code, subclass, protocol, expected_endpoints = expected[number]
        endpoint_list = list(interface)
        if (interface.bInterfaceClass != class_code or
                interface.bInterfaceSubClass != subclass or
                interface.bInterfaceProtocol != protocol or
                getattr(interface, "bNumEndpoints", None) != len(expected_endpoints) or
                len(endpoint_list) != len(expected_endpoints)):
            raise DiagError("unexpected active USB interface class or endpoint count")
        endpoints = {endpoint.bEndpointAddress: endpoint for endpoint in endpoint_list}
        if len(endpoints) != len(endpoint_list) or set(endpoints) != set(expected_endpoints):
            raise DiagError("unexpected active USB endpoint addresses")
        for address, endpoint in endpoints.items():
            transfer_type, endpoint_packet_size = expected_endpoints[address]
            if ((endpoint.bmAttributes & 0x03) != transfer_type or
                    endpoint.wMaxPacketSize != endpoint_packet_size):
                raise DiagError("unexpected active USB endpoint type or packet size")
    if seen != set(expected):
        raise DiagError("USB composite interface is incomplete")


def open_usb_transport(vid: int, pid: int, bus: int, address: int) -> USBBulkTransport:
    for value, label in ((vid, "VID"), (pid, "PID"), (bus, "bus"), (address, "address")):
        if isinstance(value, bool) or not isinstance(value, int):
            raise DiagError(f"explicit integer USB {label} is required")
    if (not 0 < vid < 0xFFFF or not 0 < pid < 0xFFFF or
            not 0 <= bus <= 255 or not 0 < address <= 127 or
            (vid, pid) == (0x303A, 0x1001)):
        raise DiagError("USB selection is outside its valid range")
    # PyUSB is intentionally imported only after explicit device coordinates
    # have been supplied; import and test discovery never enumerate hardware.
    try:
        import usb.core  # type: ignore[import-not-found]
        import usb.util  # type: ignore[import-not-found]
    except ImportError as exc:
        raise DiagError("PyUSB is required for hardware access") from exc
    try:
        devices = list(usb.core.find(find_all=True, idVendor=vid, idProduct=pid) or [])
    except Exception as exc:
        raise DiagError("USB device discovery failed") from exc
    matches = [device for device in devices
               if getattr(device, "bus", None) == bus and getattr(device, "address", None) == address]
    if len(matches) != 1:
        raise DiagError("explicit USB target is absent or ambiguous; no interface was claimed")
    device = matches[0]
    try:
        product = usb.util.get_string(device, device.iProduct)
        if product != PRODUCT_NAME:
            raise DiagError("unexpected USB product descriptor; refusing interface claim")
        if (device.bDeviceClass != 0xEF or device.bDeviceSubClass != 0x02 or
                device.bDeviceProtocol != 0x01 or device.bNumConfigurations != 1):
            raise DiagError("unexpected USB composite device class; refusing interface claim")
        packet_size = _validate_configuration_descriptor(_configuration_descriptors(device))
        configuration = device.get_active_configuration()
        _validate_interface_metadata(configuration, packet_size)
    except DiagError:
        usb.util.dispose_resources(device)
        raise
    except Exception as exc:
        usb.util.dispose_resources(device)
        raise DiagError("could not verify active USB interface descriptors") from exc
    return USBBulkTransport(device, usb.util, VENDOR_INTERFACE,
                            VENDOR_OUT_ENDPOINT, VENDOR_IN_ENDPOINT)


def _print_json(value: Any) -> None:
    print(json.dumps(value, ensure_ascii=True, sort_keys=True))


def _metadata_view(metadata: dict[str, Any]) -> dict[str, Any]:
    return {
        "protection": metadata["protection"], "size": metadata["size"],
        "type": metadata["type"],
        "date": {"days": metadata["days"], "minutes": metadata["minutes"],
                 "ticks": metadata["ticks"]},
        "name": metadata["name"], "comment": metadata["comment"],
    }


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run offline protocol tests without USB discovery")
    parser.add_argument("--vid", type=lambda value: int(value, 0))
    parser.add_argument("--pid", type=lambda value: int(value, 0))
    parser.add_argument("--bus", type=int)
    parser.add_argument("--address", type=int)
    parser.add_argument("--seconds", type=float, default=15.0,
                        help="per-frame transfer deadline (1..60 seconds)")
    parser.add_argument("--allow-input", action="store_true",
                        help="explicitly permit move/click operations; server must also enable input")
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("status", help="show server capabilities")
    screenshot = commands.add_parser("screenshot", help="capture a PPM image without replacing output")
    screenshot.add_argument("output")
    screenshot.add_argument("--crop", nargs=4, type=int, metavar=("X", "Y", "WIDTH", "HEIGHT"),
                            default=(0, 0, 0, 0))
    move = commands.add_parser("move", help="move the pointer")
    move.add_argument("x", type=int)
    move.add_argument("y", type=int)
    click = commands.add_parser("click", help="click at a point")
    click.add_argument("x", type=int)
    click.add_argument("y", type=int)
    click.add_argument("--button", choices=tuple(BUTTONS), default="left")
    stat = commands.add_parser("stat", help="print native DOS metadata")
    stat.add_argument("path", help="relative AROS DOS path, encoded as strict Latin-1")
    listing = commands.add_parser("list", help="list a native directory")
    listing.add_argument("path", help="relative AROS DOS path; empty string denotes the configured root")
    listing.add_argument("--max-items", type=int, default=FILE_ENUM_MAX)
    download = commands.add_parser("download", help="write a raw file or range without replacing output")
    download.add_argument("path", help="relative AROS DOS path")
    download.add_argument("output")
    download.add_argument("--offset", type=int, default=0)
    download.add_argument("--length", type=int)
    return parser


def _run_command(args: argparse.Namespace, transport: Any) -> int:
    client = ProtocolClient(transport, timeout=args.seconds, allow_input=args.allow_input)
    hello = client.hello()
    if args.command == "status":
        _print_json({"hello": hello, "status": client.status()})
    elif args.command == "screenshot":
        x, y, width, height = args.crop
        image_width, image_height, pixels = client.capture(x, y, width, height)
        ppm = f"P6\n{image_width} {image_height}\n255\n".encode("ascii") + pixels
        write_exclusive(args.output, ppm)
        _print_json({"width": image_width, "height": image_height,
                     "bytes": len(pixels), "sha256": hashlib.sha256(pixels).hexdigest()})
    elif args.command == "move":
        client.move(args.x, args.y)
        print("move complete")
    elif args.command == "click":
        client.click(args.x, args.y, args.button)
        print("click complete")
    elif args.command == "stat":
        _print_json(_metadata_view(client.stat(args.path)))
    elif args.command == "list":
        if not 1 <= args.max_items <= FILE_ENUM_MAX:
            raise DiagError(f"--max-items must be between 1 and {FILE_ENUM_MAX}")
        items = []
        for index in range(args.max_items):
            entry = client.list_one(args.path, index)
            if entry is None:
                break
            items.append(_metadata_view(entry))
        _print_json({"entries": items, "limit_reached": len(items) == args.max_items})
    elif args.command == "download":
        offset = _u32(args.offset, "offset")
        if args.length is None:
            metadata = client.stat(args.path)
            if offset > metadata["size"]:
                raise DiagError("download offset is past the file size")
            length = metadata["size"] - offset
        else:
            length = _u32(args.length, "length")
        if length > client.snapshot_max:
            raise DiagError("download length must be between 0 bytes and 4 MiB")
        contents = client.read_file(args.path, offset, length)
        write_exclusive(args.output, contents)
        _print_json({"bytes": len(contents), "sha256": hashlib.sha256(contents).hexdigest()})
    else:
        raise DiagError("a command is required")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.self_test:
        import unittest
        test_dir = Path(__file__).resolve().parent
        suite = unittest.defaultTestLoader.discover(str(test_dir), pattern="test_client.py")
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.command is None:
        parser.error("a command is required")
    if (args.vid is None or args.pid is None or args.bus is None or args.address is None):
        parser.error("explicit --vid, --pid, --bus, and --address are required")
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be greater than 0 and at most 60")
    if args.command in ("move", "click") and not args.allow_input:
        parser.error("move/click requires --allow-input")
    try:
        transport = open_usb_transport(args.vid, args.pid, args.bus, args.address)
        with transport:
            return _run_command(args, transport)
    except Exception as exc:
        print(f"RemoteDiag: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
