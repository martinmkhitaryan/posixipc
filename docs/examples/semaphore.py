"""Unnamed process-shared semaphore as a gate between two processes."""

from __future__ import annotations

import os
import uuid

import posixipc


def child(name: str) -> None:
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=0)
    layout.attach(name, timeout=2.0)
    assert isinstance(sem, posixipc.Semaphore)
    if not sem.acquire(timeout=2.0):
        raise SystemExit("timed out waiting for post")
    sem.close()


def main() -> None:
    name = f"/posixipc-ex-sem-{uuid.uuid4()}"
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=0)
    region = layout.create(name)
    assert isinstance(sem, posixipc.Semaphore)
    pid = os.fork()
    if pid == 0:
        child(name)
        os._exit(0)
    sem.release()
    _, status = os.waitpid(pid, 0)
    if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise SystemExit(f"child failed: {status}")
    sem.close()
    region.close()
    posixipc.SharedMemory.unlink_name(name)


if __name__ == "__main__":
    main()
