#!/usr/bin/env python3
"""Explicitly selected vendor-bulk echo probe. Never flashes or resets a board."""
from __future__ import annotations
import argparse
import hashlib
import random
import time
import unittest

MAX_BYTES = 16 * 1024 * 1024

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
        if usb.util.get_string(target,target.iProduct)!="P4 USB echo prototype":
            raise ProbeError("unexpected product; refusing interface claim")
        # Do not detach drivers or change configuration to force access.
        config=target.get_active_configuration()
        interface=config[(0,0)]
        endpoints={e.bEndpointAddress:e for e in interface}
        if (interface.bInterfaceClass!=0xff or set(endpoints)!={0x01,0x81} or
            any(e.bmAttributes & 3 != 2 for e in endpoints.values()) or
            endpoints[1].wMaxPacketSize not in (64,512) or
            endpoints[1].wMaxPacketSize!=endpoints[0x81].wMaxPacketSize):
            raise ProbeError("unexpected interface/endpoint descriptors")
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
