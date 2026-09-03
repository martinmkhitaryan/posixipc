import pytest

import posixipc
from tests.helpers import procs

pytestmark = pytest.mark.skipif(not posixipc.features.named_semaphore, reason="NamedSemaphore not detected")


def test_named_semaphore_ctor_rejected():
    with pytest.raises(TypeError):
        posixipc.NamedSemaphore()


def test_create_attach_persist_and_unlink(shm_name):
    sem = posixipc.NamedSemaphore.create(shm_name, value=1)
    try:
        assert sem.acquire(blocking=False) is True
        assert sem.acquire(blocking=False) is False
        sem.close()
        other = posixipc.NamedSemaphore.attach(shm_name)
        assert other.acquire(blocking=False) is False
        other.release()
        other.close()
    finally:
        posixipc.NamedSemaphore.unlink_name(shm_name)


def test_close_does_not_unlink(shm_name):
    sem = posixipc.NamedSemaphore.create(shm_name, value=1)
    sem.close()
    other = posixipc.NamedSemaphore.attach(shm_name)
    other.close()
    posixipc.NamedSemaphore.unlink_name(shm_name)
    with pytest.raises(FileNotFoundError):
        posixipc.NamedSemaphore.attach(shm_name)


def test_name_too_long_on_glibc():
    if posixipc.__build_info__.get("libc") != "glibc":
        pytest.skip("glibc sem. prefix limit")
    name = "/" + ("a" * 252)
    with pytest.raises(OSError):
        posixipc.NamedSemaphore.create(name)


def _hold_named(name, ready):
    sem = posixipc.NamedSemaphore.attach(name)
    sem.acquire(interruptible=False)
    ready.set()
    while True:
        import time

        time.sleep(1)


def test_crashed_holder_does_not_release(shm_name):
    sem = posixipc.NamedSemaphore.create(shm_name, value=1)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=_hold_named, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert sem.acquire(timeout=0.3) is False
    finally:
        sem.close()
        posixipc.NamedSemaphore.unlink_name(shm_name)
