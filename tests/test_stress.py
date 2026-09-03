import struct
import sys
import time

import pytest

import posixipc
from tests.helpers import procs

pytestmark = pytest.mark.timeout(90)

_FUTEX = pytest.mark.skipif(sys.platform != "linux", reason="FutexQueue is Linux only")


def _queue_pair(shm_name, depth, item_size, use_futex):
    if use_futex:
        layout, queue = procs.make_futex_queue_layout(depth, item_size)
    else:
        layout, queue = procs.make_queue_layout(depth, item_size)
    region = layout.create(shm_name)
    return layout, queue, region


def _cleanup_queue(queue, region, shm_name):
    queue.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


@pytest.mark.parametrize("use_futex", [False, pytest.param(True, marks=_FUTEX)])
def test_queue_two_producers_two_consumers(shm_name, use_futex):
    depth = 16
    item_size = 8
    per = 250
    layout, queue, region = _queue_pair(shm_name, depth, item_size, use_futex)
    ctx = procs.spawn_ctx()
    start = ctx.Event()
    ready = [ctx.Event() for _ in range(4)]
    p1_out, c1 = ctx.Pipe(duplex=False)
    p2_out, c2 = ctx.Pipe(duplex=False)
    producers = [
        ctx.Process(
            target=procs.child_queue_produce_range,
            args=(shm_name, depth, item_size, 0, per, b"Aaaa", ready[0], start, use_futex),
        ),
        ctx.Process(
            target=procs.child_queue_produce_range,
            args=(shm_name, depth, item_size, 0, per, b"Bbbb", ready[1], start, use_futex),
        ),
    ]
    consumers = [
        ctx.Process(
            target=procs.child_queue_consume_n,
            args=(shm_name, depth, item_size, per, ready[2], start, c1, use_futex),
        ),
        ctx.Process(
            target=procs.child_queue_consume_n,
            args=(shm_name, depth, item_size, per, ready[3], start, c2, use_futex),
        ),
    ]
    try:
        for proc in producers + consumers:
            proc.start()
        assert all(ev.wait(5) for ev in ready)
        start.set()
        for proc in producers + consumers:
            assert procs.join_or_kill(proc, timeout=20) == 0
        got = p1_out.recv() + p2_out.recv()
        assert len(got) == per * 2
        assert len(set(got)) == per * 2
        a = {struct.unpack_from("<I", item, 0)[0] for item in got if item[4:] == b"Aaaa"}
        b = {struct.unpack_from("<I", item, 0)[0] for item in got if item[4:] == b"Bbbb"}
        assert a == set(range(per))
        assert b == set(range(per))
        assert queue.qsize() == 0
    finally:
        p1_out.close()
        p2_out.close()
        c1.close()
        c2.close()
        _cleanup_queue(queue, region, shm_name)


def test_robust_mutex_recover_then_three_bumpers(shm_name):
    n = 200
    layout, mutex, blob = procs.make_robust_layout()
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    held = ctx.Event()
    start = ctx.Event()
    ready = [ctx.Event() for _ in range(3)]
    holder = ctx.Process(target=procs.hold_robust_until_killed, args=(shm_name, held))
    bumpers = [ctx.Process(target=procs.child_robust_bump, args=(shm_name, n, ready[i], start)) for i in range(3)]
    try:
        holder.start()
        assert held.wait(5)
        for proc in bumpers:
            proc.start()
        assert all(ev.wait(5) for ev in ready)
        holder.kill()
        procs.join_or_kill(holder, timeout=2)
        start.set()
        for proc in bumpers:
            assert procs.join_or_kill(proc, timeout=20) == 0
        assert struct.unpack_from("<Q", buf, 0)[0] == n * 3
    finally:
        buf.release()
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_condition_notify_all_three_waiters(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    blob = layout.add_bytes(8)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    buf[0] = 0
    ctx = procs.spawn_ctx()
    ready = [ctx.Event() for _ in range(3)]
    waiters = [ctx.Process(target=procs.child_cond_wait_flag, args=(shm_name, ready[i])) for i in range(3)]
    try:
        for proc in waiters:
            proc.start()
        assert all(ev.wait(5) for ev in ready)
        time.sleep(0.05)
        with mutex:
            buf[0] = 1
            cond.notify_all()
        for proc in waiters:
            assert procs.join_or_kill(proc) == 0
    finally:
        buf.release()
        cond.close()
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_condition_two_waiters_owner_died(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=procs.recover_noop)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    waiting = [ctx.Event() for _ in range(2)]
    held = ctx.Event()
    waiters = [ctx.Process(target=procs.child_robust_cond_wait, args=(shm_name, waiting[i])) for i in range(2)]
    holder = ctx.Process(target=procs.child_hold_robust_cond, args=(shm_name, held))
    try:
        for proc in waiters:
            proc.start()
        assert all(ev.wait(5) for ev in waiting)
        holder.start()
        assert held.wait(5)
        holder.kill()
        procs.join_or_kill(holder, timeout=2)
        cond.notify_all()
        for proc in waiters:
            assert procs.join_or_kill(proc) == 0
    finally:
        cond.close()
        mutex.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_mutex_array_kill_one_index(shm_name):
    layout, locks = procs.make_robust_array_layout(2)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.child_hold_array_index, args=(shm_name, 0, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert locks.acquire(0, timeout=2.0) is True
        locks.release(0)
        assert locks.acquire(1, blocking=False) is True
        locks.release(1)
    finally:
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_open_or_create_stampede(shm_name):
    n = 80
    workers = 4
    ctx = procs.spawn_ctx()
    start = ctx.Event()
    ready = [ctx.Event() for _ in range(workers)]
    procs_ = [
        ctx.Process(target=procs.child_open_or_create_bump, args=(shm_name, n, ready[i], start)) for i in range(workers)
    ]
    layout, mutex, blob = procs.make_plain_layout()
    region = None
    buf = None
    try:
        for proc in procs_:
            proc.start()
        assert all(ev.wait(8) for ev in ready)
        start.set()
        for proc in procs_:
            assert procs.join_or_kill(proc, timeout=20) == 0
        region = layout.open_or_create(shm_name, timeout=5.0)
        buf = memoryview(blob)
        assert struct.unpack_from("<Q", buf, 0)[0] == n * workers
    finally:
        if buf is not None:
            buf.release()
        mutex.close()
        blob.close()
        if region is not None:
            region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_semaphore_four_processes(shm_name):
    n = 100
    value = 1
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=value)
    blob = layout.add_bytes(8)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    struct.pack_into("<Q", buf, 0, 0)
    ctx = procs.spawn_ctx()
    start = ctx.Event()
    ready = [ctx.Event() for _ in range(4)]
    workers = [ctx.Process(target=procs.child_sem_bump, args=(shm_name, n, ready[i], start, value)) for i in range(4)]
    try:
        for proc in workers:
            proc.start()
        assert all(ev.wait(5) for ev in ready)
        start.set()
        for proc in workers:
            assert procs.join_or_kill(proc, timeout=20) == 0
        assert struct.unpack_from("<Q", buf, 0)[0] == n * 4
    finally:
        buf.release()
        sem.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


@pytest.mark.skipif(not posixipc.features.named_message_queue, reason="NamedMessageQueue not detected")
def test_named_mq_two_producers(shm_name):
    per = 40
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=8, msgsize=32)
    ctx = procs.spawn_ctx()
    start = ctx.Event()
    ready1 = ctx.Event()
    ready2 = ctx.Event()
    p1 = ctx.Process(target=procs.child_mq_put_range, args=(shm_name, 0, per, ready1, start))
    p2 = ctx.Process(target=procs.child_mq_put_range, args=(shm_name, 1000, per, ready2, start))
    try:
        p1.start()
        p2.start()
        assert ready1.wait(5) and ready2.wait(5)
        start.set()
        got = set()
        for _ in range(per * 2):
            item = mq.get(timeout=10.0)
            assert item is not None
            got.add(struct.unpack_from("<I", item, 0)[0])
        assert procs.join_or_kill(p1, timeout=5) == 0
        assert procs.join_or_kill(p2, timeout=5) == 0
        assert got == set(range(per)) | set(range(1000, 1000 + per))
        assert mq.get(blocking=False) is None
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)


@pytest.mark.skipif(not posixipc.features.named_message_queue, reason="NamedMessageQueue not detected")
@pytest.mark.filterwarnings("ignore::pytest.PytestUnraisableExceptionWarning")
def test_named_mq_notify_exception_unraisable(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=8)
    try:

        def boom(_arg):
            raise RuntimeError("notify boom")

        mq.request_notification((boom, None))
        assert mq.put(b"ping") is True
        time.sleep(0.2)
        assert mq.get(timeout=2.0) == b"ping"
        mq.request_notification()
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)
