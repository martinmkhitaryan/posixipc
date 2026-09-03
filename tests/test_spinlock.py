import pytest

import posixipc
from tests.helpers import procs

pytestmark = pytest.mark.skipif(not posixipc.features.spinlock, reason="SpinLock not detected")


def test_private_spinlock():
    from posixipc.spinlock import SpinLock

    lock = SpinLock()
    try:
        assert lock.acquire(blocking=False) is True
        assert lock.acquire(blocking=False) is False
        lock.release()
        with lock as same:
            assert same is lock
    finally:
        lock.close()


def test_spinlock_process_shared_requires_layout():
    from posixipc.spinlock import SpinLock

    with pytest.raises(ValueError):
        SpinLock(process_shared=True)


def _spin_child(name, n, ready, start):
    import struct

    from posixipc.spinlock import SpinLock

    layout = posixipc.Layout()
    lock = layout.add(SpinLock)
    blob = layout.add_bytes(8)
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        with lock:
            value = struct.unpack_from("<Q", buf, 0)[0]
            struct.pack_into("<Q", buf, 0, value + 1)
    buf.release()
    lock.close()
    blob.close()


def test_spinlock_spawn_counter(shm_name):
    import struct

    from posixipc.spinlock import SpinLock

    layout = posixipc.Layout()
    lock = layout.add(SpinLock)
    blob = layout.add_bytes(8)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    start = ctx.Event()
    n = 200
    p1 = ctx.Process(target=_spin_child, args=(shm_name, n, ready1, start))
    p2 = ctx.Process(target=_spin_child, args=(shm_name, n, ready2, start))
    p1.start()
    p2.start()
    try:
        assert ready1.wait(5) and ready2.wait(5)
        start.set()
        assert procs.join_or_kill(p1) == 0
        assert procs.join_or_kill(p2) == 0
        assert struct.unpack_from("<Q", buf, 0)[0] == n * 2
    finally:
        buf.release()
        lock.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
