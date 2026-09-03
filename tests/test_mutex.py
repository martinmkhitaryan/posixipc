import pytest

import posixipc


def test_private_mutex_context_manager():
    mutex = posixipc.Mutex()
    try:
        with mutex:
            assert mutex.acquire(blocking=False) is False
        mutex.acquire()
        mutex.release()
    finally:
        mutex.close()


def test_double_release_raises():
    mutex = posixipc.Mutex()
    try:
        mutex.acquire()
        mutex.release()
        with pytest.raises(RuntimeError):
            mutex.release()
    finally:
        mutex.close()


def test_robust_is_mutex():
    def recover(m):
        del m

    mutex = posixipc.RobustMutex(on_owner_died=recover)
    try:
        assert isinstance(mutex, posixipc.Mutex)
        with mutex:
            pass
    finally:
        mutex.close()


def test_private_reacquire_errorcheck():
    mutex = posixipc.Mutex()
    try:
        mutex.acquire()
        with pytest.raises(RuntimeError):
            mutex.acquire()
        mutex.release()
    finally:
        mutex.close()
