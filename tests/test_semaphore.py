import pytest

import posixipc
from tests.helpers import procs


def test_private_semaphore():
    sem = posixipc.Semaphore(1)
    try:
        assert sem.acquire(blocking=False) is True
        assert sem.acquire(blocking=False) is False
        sem.release()
        with sem as same:
            assert same is sem
            assert sem.acquire(timeout=0) is False
    finally:
        sem.close()


def test_semaphore_timeout():
    sem = posixipc.Semaphore(0)
    try:
        assert sem.acquire(timeout=0.15) is False
    finally:
        sem.close()


def test_semaphore_process_shared_requires_layout():
    with pytest.raises(ValueError):
        posixipc.Semaphore(process_shared=True)


def _sem_child(name, n, ready, start):
    import struct

    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=1)
    blob = layout.add_bytes(8)
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        sem.acquire()
        value = struct.unpack_from("<Q", buf, 0)[0]
        struct.pack_into("<Q", buf, 0, value + 1)
        sem.release()
    buf.release()
    sem.close()
    blob.close()


def test_semaphore_spawn_counter(shm_name):
    import struct

    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=1)
    blob = layout.add_bytes(8)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    start = ctx.Event()
    n = 200
    p1 = ctx.Process(target=_sem_child, args=(shm_name, n, ready1, start))
    p2 = ctx.Process(target=_sem_child, args=(shm_name, n, ready2, start))
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
        sem.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_semaphore_value_affects_digest():
    a = posixipc.Layout()
    a.add(posixipc.Semaphore, value=1)
    b = posixipc.Layout()
    b.add(posixipc.Semaphore, value=2)
    assert a.digest != b.digest


def _sem_trywait_child(name, conn):
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=1)
    layout.attach(name)
    conn.send(sem.acquire(blocking=False))
    conn.close()
    sem.close()


def test_semaphore_attach_does_not_reinit(shm_name):
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=1)
    region = layout.create(shm_name)
    assert sem.acquire(blocking=False) is True
    ctx = procs.spawn_ctx()
    parent, child = ctx.Pipe()
    proc = ctx.Process(target=_sem_trywait_child, args=(shm_name, child))
    proc.start()
    try:
        assert parent.recv() is False
        assert procs.join_or_kill(proc) == 0
    finally:
        parent.close()
        sem.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
