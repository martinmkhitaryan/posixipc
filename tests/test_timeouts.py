import threading

import pytest

import posixipc


def test_timeout_zero_is_nonblocking():
    mutex = posixipc.Mutex()
    mutex.acquire()
    result = []

    def worker():
        result.append(mutex.acquire(timeout=0))
        result.append(mutex.acquire(blocking=False))

    try:
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        assert result == [False, False]
    finally:
        mutex.release()
        mutex.close()


def test_negative_timeout_valueerror():
    mutex = posixipc.Mutex()
    try:
        with pytest.raises(ValueError):
            mutex.acquire(timeout=-1)
        with pytest.raises(ValueError):
            mutex.acquire(timeout=float("nan"))
    finally:
        mutex.close()


def test_blocking_false_with_timeout_valueerror():
    mutex = posixipc.Mutex()
    try:
        with pytest.raises(ValueError):
            mutex.acquire(timeout=1.0, blocking=False)
    finally:
        mutex.close()


def test_timeout_expiry_returns_false():
    mutex = posixipc.Mutex()
    mutex.acquire()
    result = []

    def worker():
        result.append(mutex.acquire(timeout=0.2))

    try:
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        assert result == [False]
    finally:
        mutex.release()
        mutex.close()


def test_timeout_expiry_on_shared(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    mutex.acquire()
    result = []

    def worker():
        result.append(mutex.acquire(timeout=0.15))

    try:
        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        assert result == [False]
    finally:
        mutex.release()
        mutex.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
