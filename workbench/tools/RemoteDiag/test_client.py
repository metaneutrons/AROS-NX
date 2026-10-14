"""Offline tests for the RemoteDiag host client; no USB discovery is performed."""
from __future__ import annotations

import builtins
import os
import struct
import tempfile
import types
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

import client


def response_frame(opcode: int, status: int, request_id: int, session: int,
                   body: bytes = b"") -> bytes:
    prefix = client.HEADER.pack(client.MAGIC, client.VERSION, opcode | 0x8000,
                                status, len(body), request_id, session, 0)
    checksum = zlib.crc32(prefix[:28] + body) & 0xFFFFFFFF
    return prefix[:28] + struct.pack("<I", checksum) + body


def recalc_crc(frame: bytearray) -> None:
    body_length = struct.unpack_from("<I", frame, 12)[0]
    checksum = zlib.crc32(frame[:28] + frame[client.HEADER_SIZE:client.HEADER_SIZE + body_length]) & 0xFFFFFFFF
    struct.pack_into("<I", frame, 28, checksum)


def metadata_body(name: bytes = b"file", *, size: int = 6) -> bytes:
    head = struct.pack("<IIiiii", 0x1234, size, -3, -4, 5, -6)
    return head + name[:108].ljust(108, b"\0") + b"comment".ljust(80, b"\0")


class FakeRemote:
    def __init__(self, *, image: bytes | None = None, drop_op: int | None = None,
                 response_filter=None, capabilities: int = 7):
        self.image = image if image is not None else b"\x00\x10\x20\x30\x40\x50"
        self.file = b"abcdefghij"
        self.session = 0
        self.generation = 9
        self.snapshots: dict[int, bytes] = {}
        self.requests: list[tuple[int, int, int, bytes]] = []
        self.input_actions: list[tuple[int, bytes]] = []
        self.drop_op = drop_op
        self.response_filter = response_filter
        self.capabilities = capabilities

    def handle(self, frame: bytes) -> bytes | None:
        magic, version, opcode, status, length, request_id, session, checksum = client.HEADER.unpack_from(frame)
        body = frame[client.HEADER_SIZE:]
        if (magic != client.MAGIC or version != client.VERSION or status or length != len(body) or
                zlib.crc32(frame[:28] + body) & 0xFFFFFFFF != checksum):
            raise AssertionError("test client sent a malformed frame")
        self.requests.append((opcode, request_id, session, body))
        if opcode == client.RD_HELLO:
            requested_session, = struct.unpack("<Q", body)
            if requested_session and requested_session != self.session:
                if self.session:
                    self.generation += 1
                self.session = requested_session
                reply_status = client.RD_OK
            else:
                reply_status = client.RD_SESSION
            reply_session = self.session
            reply_body = (struct.pack("<IIII", 1, client.DATA_MAX,
                                      client.SNAPSHOT_MAX, self.generation)
                          if reply_status == client.RD_OK else b"")
        else:
            if session != self.session:
                raise AssertionError("client used a stale or invalid session")
            reply_session = session
            reply_status = client.RD_OK
            reply_body = b""
            if opcode == client.RD_STATUS:
                reply_body = struct.pack("<II", self.capabilities, 0)
            elif opcode == client.RD_CAPTURE:
                if len(body) != 16:
                    raise AssertionError("bad CAPTURE body")
                width = 42 if len(self.image) == 42 * 41 * 3 else 2
                height = len(self.image) // (width * 3)
                pixels = self.image
                snapshot_id = 11
                self.snapshots[snapshot_id] = pixels
                reply_body = struct.pack("<IIIII", snapshot_id, len(pixels),
                                         width, height, zlib.crc32(pixels) & 0xFFFFFFFF)
            elif opcode == client.RD_READ:
                offset, wanted = struct.unpack_from("<II", body)
                path = body[8:]
                if not path.endswith(b"\0"):
                    raise AssertionError("READ path is not NUL terminated")
                if offset + wanted > len(self.file):
                    reply_status = client.RD_BAD_REQUEST
                else:
                    contents = self.file[offset:offset + wanted]
                    snapshot_id = 12
                    self.snapshots[snapshot_id] = contents
                    reply_body = struct.pack("<IIIII", snapshot_id, len(contents), 0, 0,
                                             zlib.crc32(contents) & 0xFFFFFFFF)
            elif opcode == client.RD_CHUNK:
                snapshot_id, offset, wanted = struct.unpack("<III", body)
                reply_body = self.snapshots[snapshot_id][offset:offset + wanted]
            elif opcode == client.RD_RELEASE:
                snapshot_id, = struct.unpack("<I", body)
                self.snapshots.pop(snapshot_id, None)
            elif opcode == client.RD_MOVE or opcode == client.RD_CLICK:
                self.input_actions.append((opcode, body))
            elif opcode == client.RD_STAT:
                reply_body = metadata_body(size=len(self.file))
            elif opcode == client.RD_LIST:
                index, = struct.unpack_from("<I", body)
                if index == 0:
                    reply_body = metadata_body()
                else:
                    reply_status = client.RD_END
            else:
                reply_status = client.RD_BAD_REQUEST
        if opcode == self.drop_op:
            return None
        result = response_frame(opcode, reply_status, request_id, reply_session, reply_body)
        return self.response_filter(result) if self.response_filter else result


class FragmentedTransport:
    def __init__(self, remote: FakeRemote, *, max_write: int = 7, max_read: int = 5,
                 fail_write_call: int | None = None, empty_read: bool = False):
        self.remote = remote
        self.max_write = max_write
        self.max_read = max_read
        self.fail_write_call = fail_write_call
        self.empty_read = empty_read
        self.write_calls = 0
        self.accepted = bytearray()
        self.responses = bytearray()
        self.expected: int | None = None

    def write(self, data: bytes, _timeout_ms: int) -> int:
        self.write_calls += 1
        if self.fail_write_call == self.write_calls:
            raise TimeoutError("injected ambiguous write")
        count = min(self.max_write, len(data))
        self.accepted.extend(data[:count])
        if self.expected is None and len(self.accepted) >= client.HEADER_SIZE:
            self.expected = client.HEADER_SIZE + struct.unpack_from("<I", self.accepted, 12)[0]
        if self.expected is not None and len(self.accepted) >= self.expected:
            request = bytes(self.accepted[:self.expected])
            del self.accepted[:self.expected]
            self.expected = None
            reply = self.remote.handle(request)
            if reply is not None:
                self.responses.extend(reply)
        return count

    def read(self, maximum: int, _timeout_ms: int) -> bytes:
        if self.empty_read:
            return b""
        if not self.responses:
            raise TimeoutError("injected response timeout")
        count = min(maximum, self.max_read, len(self.responses))
        result = bytes(self.responses[:count])
        del self.responses[:count]
        return result


class FakeEndpoint:
    def __init__(self, address: int, packet_size: int = 64, attributes: int = 2):
        self.bEndpointAddress = address
        self.wMaxPacketSize = packet_size
        self.bmAttributes = attributes


class FakeInterface(list):
    bInterfaceNumber = 0
    bAlternateSetting = 0
    bInterfaceClass = 0xFF


class FakeUSBDevice:
    bus = 1
    address = 2
    iProduct = 2

    def __init__(self, *, product: str = client.PRODUCT_NAME, interface=None):
        self.product = product
        self.interface = interface or FakeInterface((FakeEndpoint(0x01), FakeEndpoint(0x81)))
        self.actions: list[str] = []

    def get_active_configuration(self):
        self.actions.append("active_configuration")
        return [self.interface]


def make_client(*, remote: FakeRemote | None = None, transport: FragmentedTransport | None = None,
                allow_input: bool = False) -> tuple[client.ProtocolClient, FakeRemote, FragmentedTransport]:
    remote = remote or FakeRemote()
    transport = transport or FragmentedTransport(remote)
    protocol_client = client.ProtocolClient(transport, allow_input=allow_input,
                                            random_cookie=lambda _bits: 0x0102030405060708)
    protocol_client.hello()
    return protocol_client, remote, transport


class ClientTests(unittest.TestCase):
    def test_import_and_validation_do_not_import_or_probe_usb(self):
        original_import = builtins.__import__

        def blocked_import(name, *args, **kwargs):
            if name.startswith("usb"):
                raise AssertionError("offline path imported PyUSB")
            return original_import(name, *args, **kwargs)

        builtins.__import__ = blocked_import
        try:
            with self.assertRaises(client.DiagError):
                client.open_usb_transport(0, 1, 1, 1)
        finally:
            builtins.__import__ = original_import

    def test_byte_fragmentation_short_io_and_hello_status(self):
        protocol_client, remote, _transport = make_client()
        self.assertEqual(protocol_client.generation, 9)
        self.assertEqual(protocol_client.status(), {
            "capabilities": {"screen": True, "input": True, "files": True},
            "snapshot_id": 0,
        })
        self.assertEqual([request[0] for request in remote.requests],
                         [client.RD_HELLO, client.RD_STATUS])
        self.assertEqual(remote.requests[0][1:3], (1, 0))
        self.assertEqual(remote.requests[1][1:3], (2, 0x0102030405060708))

    def test_new_client_hello_replaces_idle_session_without_usb_reset(self):
        remote = FakeRemote()
        transport = FragmentedTransport(remote)
        first = client.ProtocolClient(transport, random_cookie=lambda _bits: 101)
        self.assertEqual(first.hello()["generation"], 9)
        second = client.ProtocolClient(transport, random_cookie=lambda _bits: 202)
        self.assertEqual(second.hello()["generation"], 10)
        self.assertEqual(remote.session, 202)
        self.assertEqual([request[0] for request in remote.requests],
                         [client.RD_HELLO, client.RD_HELLO])
        duplicate = client.ProtocolClient(transport, random_cookie=lambda _bits: 202)
        with self.assertRaises(client.ServerError):
            duplicate.hello()
        self.assertTrue(duplicate.poisoned)
        self.assertEqual(remote.generation, 10)

    def test_response_crc_version_length_session_and_id_are_checked(self):
        mutations = (
            lambda frame: frame.__setitem__(28, frame[28] ^ 1),
            lambda frame: (struct.pack_into("<H", frame, 4, 2), recalc_crc(frame)),
            lambda frame: (struct.pack_into("<I", frame, 12, client.DATA_MAX + 1), recalc_crc(frame)),
            lambda frame: (struct.pack_into("<Q", frame, 20, 99), recalc_crc(frame)),
            lambda frame: (struct.pack_into("<I", frame, 16, 7), recalc_crc(frame)),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                def transform(response, mutate=mutate):
                    changed = bytearray(response)
                    mutate(changed)
                    return bytes(changed)
                remote = FakeRemote(response_filter=transform)
                protocol_client = client.ProtocolClient(FragmentedTransport(remote),
                                                        random_cookie=lambda _bits: 7)
                with self.assertRaises(client.ProtocolError):
                    protocol_client.hello()
                self.assertTrue(protocol_client.poisoned)

    def test_write_failure_and_ambiguous_mutation_are_never_replayed(self):
        remote = FakeRemote()
        transport = FragmentedTransport(remote, fail_write_call=2)
        protocol_client = client.ProtocolClient(transport, random_cookie=lambda _bits: 7)
        with self.assertRaises(client.ProtocolError):
            protocol_client.hello()
        self.assertEqual(transport.write_calls, 2)
        self.assertEqual(remote.requests, [])
        with self.assertRaises(client.ProtocolError):
            protocol_client.hello()
        self.assertEqual(transport.write_calls, 2)

        remote = FakeRemote(drop_op=client.RD_MOVE)
        transport = FragmentedTransport(remote)
        protocol_client, _remote, _transport = make_client(
            remote=remote, transport=transport, allow_input=True)
        protocol_client.status()
        before = len(remote.requests)
        with self.assertRaises(client.ProtocolError):
            protocol_client.move(10, 20)
        self.assertEqual(len(remote.requests), before + 1)
        self.assertEqual(remote.requests[-1][0], client.RD_MOVE)
        calls_after_failure = transport.write_calls
        with self.assertRaises(client.ProtocolError):
            protocol_client.move(10, 20)
        self.assertEqual(transport.write_calls, calls_after_failure)

    def test_zero_or_invalid_partial_io_poison_session(self):
        for transport in (
            FragmentedTransport(FakeRemote(), max_write=0),
            FragmentedTransport(FakeRemote(), empty_read=True),
        ):
            protocol_client = client.ProtocolClient(transport, random_cookie=lambda _bits: 9)
            with self.assertRaises(client.ProtocolError):
                protocol_client.hello()
            self.assertTrue(protocol_client.poisoned)

    def test_response_finishing_at_deadline_is_not_success(self):
        times = iter((0.0, 0.0, 0.0, 0.0, 5.0))
        transport = FragmentedTransport(FakeRemote(), max_write=128, max_read=128)
        protocol_client = client.ProtocolClient(transport, timeout=5.0,
                                                clock=lambda: next(times),
                                                random_cookie=lambda _bits: 11)
        with self.assertRaises(client.ProtocolError):
            protocol_client.hello()
        self.assertTrue(protocol_client.poisoned)

    def test_capture_chunks_crc_and_exclusive_ppm_output(self):
        pixels = bytes((index * 17) & 0xFF for index in range(42 * 41 * 3))
        protocol_client, remote, _transport = make_client(remote=FakeRemote(image=pixels))
        width, height, actual = protocol_client.capture()
        self.assertEqual((width, height), (42, 41))
        self.assertEqual(actual, pixels)
        chunk_requests = [request for request in remote.requests if request[0] == client.RD_CHUNK]
        self.assertEqual([(struct.unpack("<III", request[3])[1:]) for request in chunk_requests],
                         [(0, client.DATA_MAX), (client.DATA_MAX, len(pixels) - client.DATA_MAX)])
        self.assertEqual(remote.snapshots, {})
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.ppm"
            ppm = f"P6\n{width} {height}\n255\n".encode("ascii") + actual
            client.write_exclusive(output, ppm)
            self.assertEqual(output.read_bytes(), ppm)
            with self.assertRaises(FileExistsError):
                client.write_exclusive(output, b"replacement")
            self.assertEqual(output.read_bytes(), ppm)

    def test_bad_snapshot_crc_stops_transfer_without_release_or_output(self):
        image = b"abcDEF"
        remote = FakeRemote(image=image)
        original_handle = remote.handle

        def bad_crc(frame: bytes) -> bytes | None:
            result = original_handle(frame)
            if result is not None and struct.unpack_from("<H", frame, 6)[0] == client.RD_CAPTURE:
                changed = bytearray(result)
                struct.pack_into("<I", changed, client.HEADER_SIZE + 16, 0)
                recalc_crc(changed)
                return bytes(changed)
            return result

        remote.handle = bad_crc  # type: ignore[method-assign]
        protocol_client, _remote, _transport = make_client(remote=remote)
        with self.assertRaises(client.ProtocolError):
            protocol_client.capture()
        self.assertNotIn(client.RD_RELEASE, [request[0] for request in remote.requests])
        self.assertTrue(protocol_client.poisoned)

    def test_read_ranges_native_paths_and_empty_file(self):
        protocol_client, remote, _transport = make_client()
        self.assertEqual(protocol_client.read_file("dir/file", 2, 4), b"cdef")
        read_request = next(request for request in remote.requests if request[0] == client.RD_READ)
        self.assertEqual(read_request[3], struct.pack("<II", 2, 4) + b"dir/file\0")
        self.assertEqual(protocol_client.read_file("empty", 0, 0), b"")
        self.assertEqual(remote.snapshots, {})
        for path in ("/absolute", "../parent", "a/../b", "a//b", "a/", "a:b", "a\\b", "x\n"):
            with self.subTest(path=path), self.assertRaises(client.DiagError):
                client.encode_path(path)
        with self.assertRaises(client.DiagError):
            client.encode_path("emoji-\U0001f600")
        self.assertEqual(client.encode_path("dir/caf\u00e9"), b"dir/caf\xe9\0")

    def test_native_metadata_signed_fields_and_list_end(self):
        protocol_client, remote, _transport = make_client()
        metadata = protocol_client.stat("file")
        self.assertEqual(metadata["protection"], 0x1234)
        self.assertEqual(metadata["size"], len(remote.file))
        self.assertEqual(metadata["type"], -3)
        self.assertEqual((metadata["days"], metadata["minutes"], metadata["ticks"]), (-4, 5, -6))
        self.assertEqual(metadata["name"], "file")
        self.assertEqual(metadata["comment"], "comment")
        self.assertEqual(protocol_client.list_one("", 0)["name"], "file")
        self.assertIsNone(protocol_client.list_one("", 1))

    def test_input_requires_explicit_host_gate_and_server_capability(self):
        protocol_client, remote, _transport = make_client()
        with self.assertRaises(client.DiagError):
            protocol_client.move(1, 2)
        self.assertFalse(remote.input_actions)
        remote = FakeRemote()
        protocol_client, remote, _transport = make_client(remote=remote, allow_input=True)
        protocol_client.status()
        protocol_client.move(1, 2)
        protocol_client.click(3, 4, "middle")
        self.assertEqual([item[0] for item in remote.input_actions], [client.RD_MOVE, client.RD_CLICK])
        self.assertEqual(struct.unpack("<III", remote.input_actions[-1][1]), (3, 4, 3))
        self.assertEqual(len([request for request in remote.requests
                             if request[0] in (client.RD_MOVE, client.RD_CLICK)]), 2)
        remote = FakeRemote(capabilities=client.CAP_SCREEN | client.CAP_FILES)
        protocol_client, remote, _transport = make_client(remote=remote, allow_input=True)
        with self.assertRaises(client.DiagError):
            protocol_client.move(1, 2)
        self.assertFalse(remote.input_actions)

    def test_device_guard_rejects_shared_debug_identity(self):
        with self.assertRaises(client.DiagError):
            client.open_usb_transport(0x303A, 0x1001, 1, 1)
        with self.assertRaises(client.DiagError):
            client.open_usb_transport(0xFFFF, 1, 1, 1)
        with self.assertRaises(client.DiagError):
            client.open_usb_transport(0x1111, 0x2222, -1, 1)

    def test_usb_selection_claims_only_verified_interface_without_reset_or_reconfigure(self):
        device = FakeUSBDevice()
        device.bus = 0
        core = types.ModuleType("usb.core")
        core.find = lambda **_kwargs: [device]
        util = types.ModuleType("usb.util")
        util.get_string = lambda target, _index: target.product
        util.claim_interface = lambda target, number: target.actions.append(f"claim:{number}")
        util.release_interface = lambda target, number: target.actions.append(f"release:{number}")
        util.dispose_resources = lambda target: target.actions.append("dispose")
        usb = types.ModuleType("usb")
        usb.core = core
        usb.util = util
        with patch.dict("sys.modules", {"usb": usb, "usb.core": core, "usb.util": util}):
            transport = client.open_usb_transport(0x1111, 0x2222, 0, 2)
            with transport:
                self.assertEqual(device.actions, ["active_configuration", "claim:0"])
            self.assertEqual(device.actions[-2:], ["release:0", "dispose"])
        self.assertNotIn("reset", device.actions)
        self.assertNotIn("set_configuration", device.actions)
        self.assertNotIn("detach_kernel_driver", device.actions)

        wrong = FakeUSBDevice(product="unrelated adapter")
        core.find = lambda **_kwargs: [wrong]
        with patch.dict("sys.modules", {"usb": usb, "usb.core": core, "usb.util": util}):
            with self.assertRaises(client.DiagError):
                client.open_usb_transport(0x1111, 0x2222, 1, 2)
        self.assertNotIn("claim:0", wrong.actions)


if __name__ == "__main__":
    unittest.main(verbosity=2)
