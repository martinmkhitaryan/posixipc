import pytest

import posixipc
from tests.helpers import procs


def test_robust_requires_on_owner_died():
    with pytest.raises(TypeError):
        posixipc.RobustMutex()
    layout = posixipc.Layout()
    with pytest.raises(TypeError):
        layout.add(posixipc.RobustMutex)


def test_zero_arg_callable_typeerror_on_construct_ok_but_call_fails():
    def recover():
        return None

    mutex = posixipc.RobustMutex(on_owner_died=recover)
    mutex.close()


def test_zero_arg_callable_on_owner_died(shm_name):
    def recover():
        return None

    layout, mutex, blob = procs.make_robust_layout(recover)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.hold_robust_until_killed, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        with pytest.raises(TypeError):
            mutex.acquire(timeout=2.0)
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_recovery_function_gets_same_handle(shm_name):
    seen = []

    def recover(mutex):
        seen.append(mutex)

    layout, mutex, blob = procs.make_robust_layout(recover)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.hold_robust_until_killed, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        with mutex:
            assert seen == [mutex]
            memoryview(blob)[0] = 7
        assert seen[0] is mutex
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_with_body_exception_still_unlocks():
    mutex = posixipc.Mutex()
    try:
        with pytest.raises(RuntimeError):
            with mutex:
                raise RuntimeError("body")
        mutex.acquire()
        mutex.release()
    finally:
        mutex.close()


def test_poison_if_recovery_raises(shm_name):
    def recover(mutex):
        del mutex
        raise RuntimeError("repair failed")

    layout, mutex, blob = procs.make_robust_layout(recover)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.hold_robust_until_killed, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        with pytest.raises(RuntimeError, match="repair failed"):
            mutex.acquire(timeout=2.0)
        with pytest.raises(posixipc.NotRecoverableError):
            mutex.acquire(timeout=2.0)
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_nonrobust_control_wedges(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.hold_plain_until_killed, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert mutex.acquire(timeout=0.3) is False
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


@pytest.mark.timeout(180)
def test_robust_recovery_repeatable(shm_name):
    for round_id in range(100):
        seen = []

        def recover(mutex, seen=seen):
            seen.append(mutex)

        name = f"{shm_name}-{round_id}"
        layout, mutex, blob = procs.make_robust_layout(recover)
        region = layout.create(name)
        ctx = procs.spawn_ctx()
        ready = ctx.Event()
        proc = ctx.Process(target=procs.hold_robust_until_killed, args=(name, ready))
        proc.start()
        try:
            assert ready.wait(5)
            proc.kill()
            procs.join_or_kill(proc, timeout=2)
            assert mutex.acquire(timeout=2.0) is True
            assert seen == [mutex]
            mutex.release()
        finally:
            mutex.close()
            blob.close()
            region.close()
            posixipc.SharedMemory.unlink_name(name)
