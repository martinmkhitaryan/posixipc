"""Two processes share a Layout mutex and a counter in payload bytes."""

from __future__ import annotations

import os
import struct
import time
import uuid

import posixipc


def child(name: str) -> None:
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    blob = layout.add_bytes(8)
    layout.attach(name, timeout=2.0)
    assert isinstance(mutex, posixipc.Mutex)
    for _ in range(1000):
        mutex.acquire()
        view = memoryview(blob)
        (value,) = struct.unpack_from("<Q", view)
        struct.pack_into("<Q", view, 0, value + 1)
        view.release()
        mutex.release()
    mutex.close()
    blob.close()


def main() -> None:
    name = f"/posixipc-ex-psm-{uuid.uuid4()}"
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    blob = layout.add_bytes(8)
    region = layout.create(name)
    assert isinstance(mutex, posixipc.Mutex)
    pid = os.fork()
    if pid == 0:
        child(name)
        os._exit(0)
    for _ in range(1000):
        mutex.acquire()
        view = memoryview(blob)
        (value,) = struct.unpack_from("<Q", view)
        struct.pack_into("<Q", view, 0, value + 1)
        view.release()
        mutex.release()
    _, status = os.waitpid(pid, 0)
    if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise SystemExit(f"child failed: {status}")
    mutex.acquire()
    view = memoryview(blob)
    (value,) = struct.unpack_from("<Q", view)
    view.release()
    mutex.release()
    mutex.close()
    blob.close()
    region.close()
    posixipc.SharedMemory.unlink_name(name)
    if value != 2000:
        raise SystemExit(f"expected 2000, got {value}")
    time.sleep(0)


if __name__ == "__main__":
    main()
