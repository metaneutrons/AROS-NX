"""Offline Python client to real C parser/dispatch, through fragmented pipes."""
import os
import select
import subprocess
import sys
from client import ProtocolClient

class Pipe:
    def __init__(self, process):
        self.process = process
    def write(self, data, timeout_ms):
        return os.write(self.process.stdin.fileno(), data[:7])
    def read(self, count, timeout_ms):
        if not select.select([self.process.stdout], [], [], timeout_ms / 1000)[0]:
            raise TimeoutError("offline pipe deadline")
        return os.read(self.process.stdout.fileno(), min(count, 13))

process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
try:
    pipe = Pipe(process)
    client = ProtocolClient(pipe, allow_input=True)
    client.hello()
    assert client.status()["capabilities"] == {"screen": True, "input": True, "files": True}
    width, height, data = client.capture()
    assert (width, height, data) == (2, 1, b"RGBRGB")
    client.move(0, 0)
    client.click(1, 0, "left")
    assert client.stat("file")["protection"] == 0xfacebeef
    assert client.read_file("file", 0, 6) == bytes(6)
    assert client.read_file("file", 0, 0) == b""
    assert client.list_one("", 1) is None
    replacement = ProtocolClient(pipe)
    replacement.hello()
    assert replacement.status()["snapshot_id"] == 0
    try:
        replacement.move(0, 0)
    except Exception as error:
        assert "input" in str(error).lower()
    else:
        raise AssertionError("missing host input gate")
finally:
    process.stdin.close()
    process.stdout.close()
    assert process.wait(timeout=5) == 0
print("Python/C fragmented protocol+dispatch pipeline passes (fake native backends)")
