import threading

import pytest

import posixipc
from tests.helpers import procs

pytestmark = pytest.mark.skipif(not posixipc.features.barrier, reason="Barrier not detected")


def test_barrier_requires_parties_in_layout():
    layout = posixipc.Layout()
    with pytest.raises(TypeError):
        layout.add(posixipc.Barrier)


def test_barrier_process_shared_requires_layout():
    with pytest.raises(ValueError):
        posixipc.Barrier(2, process_shared=True)


def test_private_barrier_one_serial():
    parties = 4
    bar = posixipc.Barrier(parties)
    serial = []
    lock = threading.Lock()

    def worker():
        if bar.wait():
            with lock:
                serial.append(1)

    try:
        threads = [threading.Thread(target=worker) for _ in range(parties)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        assert len(serial) == 1
    finally:
        bar.close()


def _barrier_child(name, ready, start):
    layout = posixipc.Layout()
    bar = layout.add(posixipc.Barrier, parties=3)
    layout.attach(name)
    ready.set()
    start.wait()
    won = bar.wait()
    bar.close()
    raise SystemExit(1 if won else 0)


def test_barrier_spawn_one_serial(shm_name):
    layout = posixipc.Layout()
    bar = layout.add(posixipc.Barrier, parties=3)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    start = ctx.Event()
    p1 = ctx.Process(target=_barrier_child, args=(shm_name, ready1, start))
    p2 = ctx.Process(target=_barrier_child, args=(shm_name, ready2, start))
    p1.start()
    p2.start()
    try:
        assert ready1.wait(5) and ready2.wait(5)
        start.set()
        won = bar.wait()
        c1 = procs.join_or_kill(p1)
        c2 = procs.join_or_kill(p2)
        winners = int(won) + int(c1 == 1) + int(c2 == 1)
        assert winners == 1
        assert {c1, c2, 0 if won else 1} <= {0, 1}
    finally:
        bar.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
