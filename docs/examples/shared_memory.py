"""Named shared memory: Layout bytes payload and a process-private view."""

from __future__ import annotations

import uuid

import posixipc


def main() -> None:
    name = f"/posixipc-ex-shm-{uuid.uuid4()}"
    layout = posixipc.Layout()
    blob = layout.add_bytes(64)
    region = layout.create(name)
    view = memoryview(blob)
    view[:5] = b"hello"
    view.release()
    attached = posixipc.Layout()
    other = attached.add_bytes(64)
    attached_region = attached.attach(name, timeout=2.0)
    other_view = memoryview(other)
    if bytes(other_view[:5]) != b"hello":
        raise SystemExit("payload mismatch")
    other_view.release()
    other.close()
    attached_region.close()
    blob.close()
    region.close()
    posixipc.SharedMemory.unlink_name(name)


if __name__ == "__main__":
    main()
