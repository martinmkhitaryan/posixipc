"""Crash while holding a robust mutex; on_owner_died repairs application state."""

from __future__ import annotations

import os
import signal
import struct
import uuid
from typing import Any

import posixipc


def child(name: str, ready_fd: int) -> None:
    def recover(mutex: posixipc.RobustMutex) -> None:
        del mutex

    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    layout.add_bytes(8)
    layout.attach(name, timeout=2.0)
    assert isinstance(mutex, posixipc.RobustMutex)
    mutex.acquire(interruptible=False)
    os.write(ready_fd, b"x")
    os.kill(os.getpid(), signal.SIGKILL)


def main() -> None:
    name = f"/posixipc-ex-crash-{uuid.uuid4()}"
    blob_cell: list[Any] = []

    def recover(mutex: posixipc.RobustMutex) -> None:
        del mutex
        view = memoryview(blob_cell[0])
        struct.pack_into("<I", view, 0, 0)
        view.release()

    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    blob = layout.add_bytes(8)
    blob_cell.append(blob)
    region = layout.create(name)
    assert isinstance(mutex, posixipc.RobustMutex)
    view = memoryview(blob)
    struct.pack_into("<I", view, 0, 7)
    view.release()
    ready_r, ready_w = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(ready_r)
        child(name, ready_w)
        os._exit(1)
    os.close(ready_w)
    if os.read(ready_r, 1) != b"x":
        raise SystemExit("child did not publish ready")
    os.close(ready_r)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    if os.WIFEXITED(status) and os.WEXITSTATUS(status) != 0:
        raise SystemExit("child exited instead of being killed")
    mutex.acquire(timeout=2.0)
    view = memoryview(blob)
    (value,) = struct.unpack_from("<I", view)
    view.release()
    mutex.release()
    mutex.close()
    blob.close()
    region.close()
    posixipc.SharedMemory.unlink_name(name)
    if value != 0:
        raise SystemExit(f"recovery did not reset counter, got {value}")


if __name__ == "__main__":
    main()
