import pickle
import struct
import time

import pytest

import posixipc
from posixipc.linux import FutexQueue
from tests.helpers import procs


def _make(shm_name, depth=4, item_size=8):
    layout, queue = procs.make_futex_queue_layout(depth, item_size)
    region = layout.create(shm_name)
    return layout, queue, region


def _cleanup(queue, region, shm_name):
    queue.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_futex_queue_ctor_rejected():
    with pytest.raises(TypeError):
        FutexQueue()


def test_futex_queue_requires_depth_and_item_size():
    layout = posixipc.Layout()
    with pytest.raises(ValueError):
        layout.add(FutexQueue)
    with pytest.raises(ValueError):
        layout.add(FutexQueue, depth=4)
    with pytest.raises(TypeError):
        layout.add(FutexQueue, depth=4, item_size=8, on_owner_died=lambda m: None)


def test_put_get_full_empty_timeout(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=4)
    try:
        assert queue.depth == 2
        assert queue.item_size == 4
        assert queue.qsize() == 0
        assert queue.get_nowait() is None
        assert queue.get(timeout=0.05) is None
        assert queue.put(b"abcd") is True
        assert queue.qsize() == 1
        assert queue.put(b"efgh") is True
        assert queue.qsize() == 2
        assert queue.put_nowait(b"xxxx") is False
        assert queue.put(b"xxxx", timeout=0.05) is False
        assert queue.get() == b"abcd"
        assert queue.get_nowait() == b"efgh"
        assert queue.qsize() == 0
    finally:
        _cleanup(queue, region, shm_name)


def test_wrong_item_size(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=4)
    try:
        with pytest.raises(ValueError):
            queue.put(b"abc")
        with pytest.raises(ValueError):
            queue.put(b"abcde")
        assert queue.put(memoryview(b"wxyz")) is True
        assert queue.get() == b"wxyz"
    finally:
        _cleanup(queue, region, shm_name)


def test_close_pin(shm_name):
    layout, queue, region = _make(shm_name)
    with pytest.raises(BufferError):
        region.close()
    queue.close()
    with pytest.raises(posixipc.ClosedError):
        queue.put(b"xxxxxxxx")
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_same_depth_same_digest():
    a = posixipc.Layout()
    b = posixipc.Layout()
    a.add(FutexQueue, depth=8, item_size=16)
    b.add(FutexQueue, depth=8, item_size=16)
    assert a.digest == b.digest
    c = posixipc.Layout()
    c.add(FutexQueue, depth=8, item_size=32)
    assert c.digest != a.digest


def test_cannot_attach_queue_digest(shm_name):
    layout, queue, region = _make(shm_name, depth=4, item_size=8)
    other = posixipc.Layout()
    other.add(posixipc.Queue, depth=4, item_size=8)
    try:
        assert other.digest != layout.digest
        with pytest.raises(posixipc.LayoutMismatchError):
            other.attach(shm_name, timeout=1.0)
    finally:
        _cleanup(queue, region, shm_name)


def test_cannot_attach_futex_queue_as_queue(shm_name):
    q_layout = posixipc.Layout()
    q = q_layout.add(posixipc.Queue, depth=4, item_size=8)
    region = q_layout.create(shm_name)
    other = posixipc.Layout()
    other.add(FutexQueue, depth=4, item_size=8)
    try:
        assert other.digest != q_layout.digest
        with pytest.raises(posixipc.LayoutMismatchError):
            other.attach(shm_name, timeout=1.0)
    finally:
        q.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_pickle_roundtrip(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=4)
    try:
        assert queue.put(b"data") is True
        restored = pickle.loads(pickle.dumps(queue))
        assert restored.slot == queue.slot
        assert restored.depth == 2
        assert restored.item_size == 4
        assert restored.get() == b"data"
        restored.close()
    finally:
        _cleanup(queue, region, shm_name)


def test_two_process_put_get(shm_name):
    layout, queue, region = _make(shm_name, depth=4, item_size=8)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    start = ctx.Event()
    parent, child = ctx.Pipe(duplex=False)
    proc = ctx.Process(
        target=procs.child_futex_queue_get,
        args=(shm_name, 4, 8, ready, start, child),
    )
    proc.start()
    try:
        assert ready.wait(5)
        assert queue.put(b"abcdefgh") is True
        start.set()
        assert parent.recv() == b"abcdefgh"
        assert procs.join_or_kill(proc) == 0
    finally:
        parent.close()
        child.close()
        _cleanup(queue, region, shm_name)


def test_killed_waiter_on_full_queue(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=8)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.child_futex_queue_block_put, args=(shm_name, 2, 8, ready))
    try:
        assert queue.put(b"11111111") is True
        assert queue.put(b"22222222") is True
        proc.start()
        assert ready.wait(5)
        time.sleep(0.3)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert queue.get(timeout=2.0) == b"11111111"
        assert queue.get(timeout=2.0) == b"22222222"
    finally:
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        _cleanup(queue, region, shm_name)


def _queue_bytes(name, queue, region):
    return posixipc._posixipc._attach_slot(name, queue.slot + 2, posixipc._posixipc.KIND_BYTES, region.digest)


def _state_off():
    cl = posixipc._posixipc.CACHELINE_BYTES
    prefix = cl
    ctrl = 24
    ctrl_pad = ((ctrl + cl - 1) // cl) * cl
    return prefix + ctrl_pad


def test_recover_reserved_put_drops_partial(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=8)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(
        target=procs.child_hold_slot_mutex,
        args=(
            shm_name,
            queue.slot,
            posixipc._posixipc.KIND_ROBUST_MUTEX,
            region.digest,
            ready,
        ),
    )
    blob = None
    try:
        assert queue.put(b"11111111") is True
        assert queue.put(b"22222222") is True
        blob = _queue_bytes(shm_name, queue, region)
        mv = memoryview(blob)
        struct.pack_into("<I", mv, _state_off(), 1)
        proc.start()
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert queue.get(timeout=2.0) == b"22222222"
        assert queue.get_nowait() is None
    finally:
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        if blob is not None:
            blob.close()
        _cleanup(queue, region, shm_name)


def test_recover_reserved_get_keeps_item(shm_name):
    layout, queue, region = _make(shm_name, depth=2, item_size=8)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(
        target=procs.child_hold_slot_mutex,
        args=(
            shm_name,
            queue.slot,
            posixipc._posixipc.KIND_ROBUST_MUTEX,
            region.digest,
            ready,
        ),
    )
    blob = None
    try:
        assert queue.put(b"keepitem") is True
        blob = _queue_bytes(shm_name, queue, region)
        mv = memoryview(blob)
        struct.pack_into("<I", mv, _state_off(), 2)
        proc.start()
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert queue.get(timeout=2.0) == b"keepitem"
    finally:
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        if blob is not None:
            blob.close()
        _cleanup(queue, region, shm_name)
