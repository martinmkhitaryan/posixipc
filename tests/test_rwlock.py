import threading

import pytest

import posixipc
from tests.helpers import procs


def test_private_rwlock_read_write():
    lock = posixipc.RWLock()
    try:
        with lock.write() as same:
            assert same is lock
        with lock.read():
            pass
        assert lock.acquire_write(timeout=0) is True
        lock.release()
    finally:
        lock.close()


def test_rwlock_timeout():
    lock = posixipc.RWLock()
    lock.acquire_write()
    result = []

    def worker():
        result.append(lock.acquire_write(timeout=0.15))

    try:
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        assert result == [False]
    finally:
        lock.release()
        lock.close()


def test_rwlock_concurrent_readers():
    lock = posixipc.RWLock()
    entered = threading.Event()
    release = threading.Event()

    def reader():
        with lock.read():
            entered.set()
            release.wait(2)

    try:
        thread = threading.Thread(target=reader)
        thread.start()
        assert entered.wait(2)
        assert lock.acquire_read(timeout=0.5) is True
        lock.release()
        release.set()
        thread.join()
    finally:
        lock.close()


def test_rwlock_process_shared_requires_layout():
    with pytest.raises(ValueError):
        posixipc.RWLock(process_shared=True)


def _rwlock_bump(name, n, ready, start):
    import struct

    layout = posixipc.Layout()
    lock = layout.add(posixipc.RWLock)
    blob = layout.add_bytes(64)
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        with lock.write():
            value = struct.unpack_from("<Q", buf, 0)[0]
            struct.pack_into("<Q", buf, 0, value + 1)
    buf.release()
    lock.close()
    blob.close()


def test_rwlock_spawn_counter(shm_name):
    import struct

    layout = posixipc.Layout()
    lock = layout.add(posixipc.RWLock)
    blob = layout.add_bytes(64)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    start = ctx.Event()
    n = 200
    p1 = ctx.Process(target=_rwlock_bump, args=(shm_name, n, ready1, start))
    p2 = ctx.Process(target=_rwlock_bump, args=(shm_name, n, ready2, start))
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


def _hold_write_until_killed(name, ready):
    layout = posixipc.Layout()
    lock = layout.add(posixipc.RWLock)
    layout.attach(name)
    lock.acquire_write(interruptible=False)
    ready.set()
    while True:
        import time

        time.sleep(1)


def test_rwlock_crash_wedges_write(shm_name):
    layout = posixipc.Layout()
    lock = layout.add(posixipc.RWLock)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=_hold_write_until_killed, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert lock.acquire_write(timeout=0.3) is False
    finally:
        lock.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
