import copy
import pickle

import pytest

import posixipc
from tests.helpers import procs


def test_copy_raises_private():
    mutex = posixipc.Mutex()
    try:
        with pytest.raises(TypeError):
            copy.copy(mutex)
        with pytest.raises(TypeError):
            copy.deepcopy(mutex)
        with pytest.raises(TypeError):
            pickle.dumps(mutex)
    finally:
        mutex.close()


def test_copy_raises_shared(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    try:
        with pytest.raises(TypeError):
            copy.copy(mutex)
        with pytest.raises(TypeError):
            copy.deepcopy(mutex)
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_pickle_shared_roundtrip(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    try:
        payload = pickle.dumps(mutex)
        restored = pickle.loads(payload)
        assert restored.process_shared is True
        assert restored.slot == mutex.slot
        assert restored.kind == mutex.kind
        assert restored.digest == mutex.digest
        restored.acquire()
        restored.release()
        restored.close()
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_pickle_digest_mismatch(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    payload = pickle.dumps(mutex)
    mutex.close()
    blob.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)

    other = posixipc.Layout()
    first = other.add(posixipc.Mutex)
    second = other.add(posixipc.Mutex)
    region2 = other.create(shm_name)
    try:
        with pytest.raises(posixipc.LayoutMismatchError):
            pickle.loads(payload)
    finally:
        first.close()
        second.close()
        region2.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_unbound_pickle_raises():
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    with pytest.raises(TypeError):
        pickle.dumps(mutex)


def test_pickle_rwlock_roundtrip(shm_name):
    layout = posixipc.Layout()
    lock = layout.add(posixipc.RWLock)
    region = layout.create(shm_name)
    try:
        restored = pickle.loads(pickle.dumps(lock))
        assert restored.slot == lock.slot
        assert restored.kind == lock.kind
        with restored.write():
            pass
        restored.close()
    finally:
        lock.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_pickle_condition_roundtrip(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    region = layout.create(shm_name)
    try:
        restored = pickle.loads(pickle.dumps(cond))
        assert restored.slot == cond.slot
        assert restored.kind == cond.kind
        associated = restored.mutex
        associated.acquire()
        assert restored.wait(timeout=0.05) is False
        associated.release()
        restored.close()
        associated.close()
    finally:
        cond.close()
        mutex.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_pickle_semaphore_roundtrip(shm_name):
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=1)
    region = layout.create(shm_name)
    try:
        restored = pickle.loads(pickle.dumps(sem))
        assert restored.slot == sem.slot
        assert restored.kind == sem.kind
        assert restored.acquire(blocking=False) is True
        restored.release()
        restored.close()
    finally:
        sem.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
