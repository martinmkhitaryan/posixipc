import struct

import pytest

import posixipc
from tests.helpers import procs


def test_cannot_construct_shared_mutex_without_layout():
    with pytest.raises(ValueError):
        posixipc.Mutex(process_shared=True)


def test_spawned_counter_total(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    start = ctx.Event()
    n = 500
    p1 = ctx.Process(target=procs.bump_counter, args=(shm_name, n, ready1, start))
    p2 = ctx.Process(target=procs.bump_counter, args=(shm_name, n, ready2, start))
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
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
