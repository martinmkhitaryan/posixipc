import gc

import pytest

import posixipc


def test_close_region_with_live_handle_buffererror(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    with pytest.raises(BufferError):
        region.close()
    mutex.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_close_succeeds_after_handle_dropped(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    mutex.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_methods_after_handle_close(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    mutex.close()
    with pytest.raises(posixipc.ClosedError):
        mutex.acquire()
    with pytest.raises(posixipc.ClosedError):
        mutex.release()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_attacher_gc_does_not_destroy(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)

    def attach_and_drop():
        child_layout = posixipc.Layout()
        child_mutex = child_layout.add(posixipc.Mutex)
        attached = child_layout.attach(shm_name, timeout=2.0)
        child_mutex.acquire()
        child_mutex.release()
        del child_mutex
        del child_layout
        gc.collect()
        attached.close()

    attach_and_drop()
    mutex.acquire()
    mutex.release()
    mutex.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_resourcewarning_on_leaked_private_mutex():
    with pytest.warns(ResourceWarning, match="unclosed posixipc.Mutex"):
        mutex = posixipc.Mutex()
        mutex.acquire()
        del mutex
        gc.collect()
