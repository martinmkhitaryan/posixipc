import pytest

import posixipc


def test_unbound_acquire_raises():
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    with pytest.raises(RuntimeError):
        mutex.acquire()


def test_attach_missing_timeout(shm_name):
    layout = posixipc.Layout()
    layout.add(posixipc.Mutex)
    with pytest.raises(FileNotFoundError):
        layout.attach(shm_name, timeout=0.2)


def test_layout_sealed_after_create(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    try:
        with pytest.raises(RuntimeError, match="sealed"):
            layout.add(posixipc.Mutex)
    finally:
        mutex.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_closed_shared_memory_methods(shm_name):
    region = posixipc.SharedMemory.create(shm_name, 64)
    region.close()
    with pytest.raises(posixipc.ClosedError):
        _ = region.name
    posixipc.SharedMemory.unlink_name(shm_name)
