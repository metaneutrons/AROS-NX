#!/usr/bin/env python3
"""Explicitly selected vendor-bulk echo probe. Never flashes or resets a board."""
from __future__ import annotations
import argparse
import hashlib
import random
import time
import unittest

MAX_BYTES = 16 * 1024 * 1024
PRODUCT_NAME = "P4 USB echo prototype"

class ProbeError(RuntimeError):
    pass

def exchange(write, read, payload: bytes, *, deadline: float, clock=time.monotonic) -> bytes:
    """Complete partial transfers under ONE budget. No reconnect/retry of a write."""
    if not 0 < len(payload) <= MAX_BYTES:
        raise ProbeError("invalid payload size")
    start = clock()
    last = start
    if not start < deadline or deadline - start > 60:
        raise ProbeError("invalid deadline")
    def remaining():
        nonlocal last
        now = clock()
        if now < last or now >= deadline:
            raise ProbeError("deadline expired or clock regressed; transfer state uncertain")
        last = now
        return max(1, int((deadline - now) * 1000))
    result = bytearray()
    offset = 0
    # Keep each outstanding request <=512 bytes. This is a correctness probe,
    # not sustained throughput qualification or a negotiated RC protocol.
    while offset < len(payload):
        chunk = payload[offset:offset + 512]
        sent = 0
        while sent < len(chunk):
            n = write(chunk[sent:], remaining())
            if not isinstance(n, int) or not 0 < n <= len(chunk) - sent:
                raise ProbeError("invalid/zero partial write; transfer state uncertain")
            sent += n
        received = bytearray()
        while len(received) < len(chunk):
            data = bytes(read(len(chunk) - len(received), remaining()))
            if not data or len(data) > len(chunk) - len(received):
                raise ProbeError("invalid/zero partial read")
            received.extend(data)
        if received != chunk:
            raise ProbeError(f"byte mismatch in chunk at offset {offset}")
        result.extend(received)
        offset += len(chunk)
    remaining()  # late completion is not within-budget success
    return bytes(result)

def select(devices, bus, address):
    matches = [d for d in devices if d.bus == bus and d.address == address]
    if len(matches) != 1:
        raise ProbeError("target absent or ambiguous; no device opened")
    return matches[0]

def verify_composite_configuration(configuration):
    """Return vendor interface 0 after checking the fixed CDC+bulk layout."""
    if (getattr(configuration, "bConfigurationValue", None) != 1 or
        getattr(configuration, "bNumInterfaces", None) != 3 or
        getattr(configuration, "wTotalLength", None) != 98):
        raise ProbeError("unexpected USB composite configuration")
    interfaces = list(configuration)
    if len(interfaces) != 3:
        raise ProbeError("unexpected USB interface count")
    expected = {
        0: (0xFF, 0, 0, {0x01: (2, None), 0x81: (2, None)}),
        1: (2, 2, 0, {0x82: (3, 8)}),
        2: (0x0A, 0, 0, {0x03: (2, None), 0x83: (2, None)}),
    }
    seen = set()
    bulk_packet_size = None
    vendor_interface = None
    for interface in interfaces:
        number = interface.bInterfaceNumber
        if number not in expected or number in seen or interface.bAlternateSetting != 0:
            raise ProbeError("unexpected USB interface numbering or alternate setting")
        seen.add(number)
        class_code, subclass, protocol, expected_endpoints = expected[number]
        endpoint_list = list(interface)
        if (interface.bInterfaceClass != class_code or
            interface.bInterfaceSubClass != subclass or
            interface.bInterfaceProtocol != protocol or
            getattr(interface, "bNumEndpoints", None) != len(expected_endpoints) or
            len(endpoint_list) != len(expected_endpoints)):
            raise ProbeError("unexpected USB interface class or endpoint count")
        endpoints = {endpoint.bEndpointAddress: endpoint for endpoint in endpoint_list}
        if len(endpoints) != len(endpoint_list) or set(endpoints) != set(expected_endpoints):
            raise ProbeError("unexpected USB endpoint addresses")
        for address, endpoint in endpoints.items():
            transfer_type, packet_size = expected_endpoints[address]
            if ((endpoint.bmAttributes & 3) != transfer_type or
                (packet_size is not None and endpoint.wMaxPacketSize != packet_size)):
                raise ProbeError("unexpected USB endpoint type or packet size")
            if transfer_type == 2:
                if endpoint.wMaxPacketSize not in (64, 512):
                    raise ProbeError("unexpected USB bulk packet size")
                if bulk_packet_size is None:
                    bulk_packet_size = endpoint.wMaxPacketSize
                elif endpoint.wMaxPacketSize != bulk_packet_size:
                    raise ProbeError("inconsistent USB bulk packet sizes")
        if number == 0:
            vendor_interface = interface
    if seen != set(expected) or vendor_interface is None:
        raise ProbeError("USB composite interfaces are incomplete")
    return vendor_interface

class ProbeTests(unittest.TestCase):
    def test_partial_exact(self):
        rng = random.Random(1985)
        for n in (1, 63, 64, 65, 511, 512, 513, 4096, 32769):
            payload = bytes(rng.randrange(256) for _ in range(n))
            fifo = bytearray()
            def wr(data, timeout):
                part = min(17, len(data)); fifo.extend(data[:part]); return part
            def rd(size, timeout):
                part = min(11, size); data = fifo[:part]; del fifo[:part]; return data
            self.assertEqual(exchange(wr, rd, payload, deadline=5, clock=lambda: 0), payload)
            self.assertFalse(fifo)
    def test_write_failure_never_replayed(self):
        calls = []
        def wr(data, timeout):
            calls.append(data); raise TimeoutError("ambiguous write")
        with self.assertRaises(TimeoutError): exchange(wr, lambda n,t:b"", b"abc", deadline=5, clock=lambda:0)
        self.assertEqual(calls, [b"abc"])
    def test_invalid_transfers(self):
        for n in (0, -1, 4):
            with self.assertRaises(ProbeError): exchange(lambda d,t:n, lambda n,t:b"abc", b"abc", deadline=5, clock=lambda:0)
        for value in (b"", b"wrong", b"abd"):
            with self.assertRaises(ProbeError): exchange(lambda d,t:len(d), lambda n,t:value, b"abc", deadline=5, clock=lambda:0)
    def test_deadline_and_regression(self):
        for times in ((0,6), (1,0)):
            it = iter(times)
            with self.assertRaises(ProbeError): exchange(lambda d,t:len(d), lambda n,t:b"abc", b"abc", deadline=5, clock=lambda:next(it))
        for data in (b"", bytes(MAX_BYTES + 1)):
            with self.assertRaises(ProbeError): exchange(None, None, data, deadline=5, clock=lambda:0)
    def test_unique_selection(self):
        class Device:
            bus=1; address=2
        target=Device()
        self.assertIs(select([target],1,2),target)
        for devices in ([],[target,target]):
            with self.assertRaises(ProbeError): select(devices,1,2)
    def test_composite_layout_keeps_vendor_zero_and_rejects_shape_changes(self):
        class Endpoint:
            def __init__(self, address, packet_size, attributes):
                self.bEndpointAddress=address
                self.wMaxPacketSize=packet_size
                self.bmAttributes=attributes
        class Interface(list):
            def __init__(self, number, class_code, subclass, protocol, endpoints):
                super().__init__(endpoints)
                self.bInterfaceNumber=number
                self.bAlternateSetting=0
                self.bInterfaceClass=class_code
                self.bInterfaceSubClass=subclass
                self.bInterfaceProtocol=protocol
                self.bNumEndpoints=len(endpoints)
        class Configuration(list):
            bConfigurationValue=1
            bNumInterfaces=3
            wTotalLength=98
        def fixture(packet_size=64):
            return Configuration((
                Interface(0,0xff,0,0,(Endpoint(0x01,packet_size,2),Endpoint(0x81,packet_size,2))),
                Interface(1,2,2,0,(Endpoint(0x82,8,3),)),
                Interface(2,0x0a,0,0,(Endpoint(0x03,packet_size,2),Endpoint(0x83,packet_size,2))),
            ))
        vendor=verify_composite_configuration(fixture())
        self.assertEqual(vendor.bInterfaceNumber,0)
        self.assertEqual(verify_composite_configuration(fixture(512)).bInterfaceNumber,0)
        bad=fixture()
        bad[2].bInterfaceClass=0xff
        with self.assertRaises(ProbeError): verify_composite_configuration(bad)
        missing=Configuration(fixture()[:2])
        with self.assertRaises(ProbeError): verify_composite_configuration(missing)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--vid", type=lambda x:int(x,0))
    p.add_argument("--pid", type=lambda x:int(x,0))
    p.add_argument("--bus", type=int)
    p.add_argument("--address", type=int)
    p.add_argument("--bytes", type=int, default=32769)
    p.add_argument("--seconds", type=float, default=10)
    a=p.parse_args()
    if a.self_test:
        return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProbeTests)).wasSuccessful() else 1
    if (a.vid is None or a.pid is None or a.bus is None or a.address is None or
        not 0 < a.vid < 65535 or not 0 < a.pid < 65535 or
        (a.vid,a.pid)==(0x303a,0x1001) or not 0 < a.bytes <= MAX_BYTES or not 0 < a.seconds <=60):
        p.error("explicit prototype VID/PID/bus/address and bounded size/time required")
    import usb.core
    import usb.util
    target=select(list(usb.core.find(find_all=True,idVendor=a.vid,idProduct=a.pid)),a.bus,a.address)
    claimed=False
    try:
        if usb.util.get_string(target,target.iProduct)!=PRODUCT_NAME:
            raise ProbeError("unexpected product; refusing interface claim")
        if (target.bDeviceClass!=0xef or target.bDeviceSubClass!=0x02 or
            target.bDeviceProtocol!=0x01 or target.bNumConfigurations!=1):
            raise ProbeError("unexpected composite device class; refusing interface claim")
        # Do not detach drivers or change configuration to force access.
        config=target.get_active_configuration()
        interface=verify_composite_configuration(config)
        endpoints={e.bEndpointAddress:e for e in interface}
        usb.util.claim_interface(target,0); claimed=True
        payload=random.Random(time.time_ns()).randbytes(a.bytes)
        received=exchange(lambda d,t:target.write(0x01,d,timeout=t),
                          lambda n,t:target.read(0x81,n,timeout=t),payload,
                          deadline=time.monotonic()+a.seconds)
        print(f"echo PASS bytes={len(received)} sha256={hashlib.sha256(received).hexdigest()}")
    finally:
        if claimed: usb.util.release_interface(target,0)
        usb.util.dispose_resources(target)
    return 0

if __name__=="__main__":
    raise SystemExit(main())
