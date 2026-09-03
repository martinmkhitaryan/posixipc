"""Bounded process-shared byte queue."""

from __future__ import annotations

import os
import uuid

import posixipc


def child(name: str) -> None:
    layout = posixipc.Layout()
    q = layout.add(posixipc.Queue, depth=4, item_size=8)
    layout.attach(name, timeout=2.0)
    assert isinstance(q, posixipc.Queue)
    item = q.get(timeout=2.0)
    if item != b"payload!":
        raise SystemExit("unexpected item")
    q.close()


def main() -> None:
    name = f"/posixipc-ex-queue-{uuid.uuid4()}"
    layout = posixipc.Layout()
    q = layout.add(posixipc.Queue, depth=4, item_size=8)
    region = layout.create(name)
    assert isinstance(q, posixipc.Queue)
    pid = os.fork()
    if pid == 0:
        child(name)
        os._exit(0)
    try:
        if not q.put(b"payload!"):
            raise SystemExit("put failed")
        _pid, status = os.waitpid(pid, 0)
        if status != 0:
            raise SystemExit(status)
    finally:
        q.close()
        region.close()
        posixipc.SharedMemory.unlink_name(name)


if __name__ == "__main__":
    main()
