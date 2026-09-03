import pickle

import pytest

import posixipc


def test_add_array_digest_matches_repeated_add():
    twice = posixipc.Layout()
    twice.add(posixipc.Mutex)
    twice.add(posixipc.Mutex)
    arrayed = posixipc.Layout()
    arrayed.add_array(posixipc.Mutex, 2)
    assert twice.digest == arrayed.digest
    assert len(twice.slots) == len(arrayed.slots) == 2
    assert twice.slots == arrayed.slots


def test_add_array_then_bytes_offsets(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 4)
    blob = layout.add_bytes(64)
    mutex = posixipc.Layout()
    mutex.add(posixipc.Mutex)
    mutex.add(posixipc.Mutex)
    mutex.add(posixipc.Mutex)
    mutex.add(posixipc.Mutex)
    mutex.add_bytes(64)
    assert layout.digest == mutex.digest
    region = layout.create(shm_name)
    try:
        assert len(locks) == 4
        locks.acquire(0)
        locks.release(0)
        with locks[2]:
            pass
        blob_view = memoryview(blob)
        blob_view[0] = 7
        blob_view.release()
    finally:
        blob.close()
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_array_one_pin(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 8)
    region = layout.create(shm_name)
    with pytest.raises(BufferError):
        region.close()
    item = locks[0]
    with pytest.raises(BufferError):
        region.close()
    del item
    locks.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_array_index_errors(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 2)
    region = layout.create(shm_name)
    try:
        with pytest.raises(IndexError):
            locks.acquire(2)
        with pytest.raises(IndexError):
            locks.acquire(-1)
        with pytest.raises(IndexError):
            locks[2]
    finally:
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_array_pickle_roundtrip(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 3)
    region = layout.create(shm_name)
    try:
        payload = pickle.dumps(locks)
        restored = pickle.loads(payload)
        assert restored.slot == locks.slot
        assert restored.kind == locks.kind
        assert len(restored) == 3
        restored.acquire(1)
        restored.release(1)
        restored.close()
    finally:
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_array_capsule_pins(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 2)
    region = layout.create(shm_name)
    capsule = locks.as_capsule(1)
    locks.close()
    with pytest.raises(BufferError):
        region.close()
    del capsule
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_robust_array_requires_callback():
    layout = posixipc.Layout()
    with pytest.raises(TypeError):
        layout.add_array(posixipc.RobustMutex, 2)


def test_robust_array_lock(shm_name):
    def recover(item):
        del item

    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.RobustMutex, 2, on_owner_died=recover)
    region = layout.create(shm_name)
    try:
        locks.acquire(0)
        locks.release(0)
        with locks[1]:
            pass
    finally:
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_condition_after_array_uses_slot_index(shm_name):
    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.Mutex, 2)
    mutex = layout.add(posixipc.Mutex)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    region = layout.create(shm_name)
    try:
        mutex.acquire()
        cond.notify()
        mutex.release()
    finally:
        cond.close()
        mutex.close()
        locks.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
