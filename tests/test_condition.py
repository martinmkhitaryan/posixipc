import threading
import time

import pytest

import posixipc
from tests.helpers import procs


def test_private_condition_notify():
    mutex = posixipc.Mutex()
    cond = posixipc.Condition(mutex)
    box = {"v": 0}
    ready = threading.Event()

    def waiter():
        with mutex:
            ready.set()
            while box["v"] == 0:
                cond.wait()

    try:
        thread = threading.Thread(target=waiter)
        thread.start()
        assert ready.wait(2)
        with mutex:
            box["v"] = 1
            cond.notify()
        thread.join(2)
        assert not thread.is_alive()
    finally:
        cond.close()
        mutex.close()


def test_condition_timeout_false():
    mutex = posixipc.Mutex()
    cond = posixipc.Condition(mutex)
    try:
        mutex.acquire()
        assert cond.wait(timeout=0.1) is False
        mutex.release()
    finally:
        cond.close()
        mutex.close()


def test_condition_wait_rejects_nonblocking():
    mutex = posixipc.Mutex()
    cond = posixipc.Condition(mutex)
    try:
        mutex.acquire()
        with pytest.raises(ValueError, match="non-blocking"):
            cond.wait(blocking=False)
        mutex.release()
    finally:
        cond.close()
        mutex.close()


def test_condition_wait_requires_mutex():
    mutex = posixipc.Mutex()
    cond = posixipc.Condition(mutex)
    try:
        with pytest.raises(RuntimeError):
            cond.wait(timeout=0.1)
    finally:
        cond.close()
        mutex.close()


def test_condition_requires_mutex_in_layout():
    layout = posixipc.Layout()
    other = posixipc.Mutex()
    try:
        with pytest.raises(ValueError):
            layout.add(posixipc.Condition, mutex=other)
    finally:
        other.close()


def _cond_child(name, ready):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    blob = layout.add_bytes(8)
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    with mutex:
        while buf[0] == 0:
            cond.wait()
    buf.release()
    cond.close()
    mutex.close()
    blob.close()


def test_condition_spawn_notify(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    blob = layout.add_bytes(8)
    region = layout.create(shm_name)
    buf = memoryview(blob)
    buf[0] = 0
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=_cond_child, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        time.sleep(0.1)
        with mutex:
            buf[0] = 1
            cond.notify()
        assert procs.join_or_kill(proc) == 0
    finally:
        buf.release()
        cond.close()
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def _cond_wait_child(name, ready):
    def recover(mutex):
        del mutex

    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    cond.wait()
    mutex.release()
    cond.close()
    mutex.close()


def _hold_robust_mutex(name, ready):
    def recover(mutex):
        del mutex

    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    layout.add(posixipc.Condition, mutex=mutex)
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def test_condition_wait_owner_died(shm_name):
    def recover(mutex):
        del mutex

    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    waiting = ctx.Event()
    held = ctx.Event()
    waiter = ctx.Process(target=_cond_wait_child, args=(shm_name, waiting))
    holder = ctx.Process(target=_hold_robust_mutex, args=(shm_name, held))
    waiter.start()
    try:
        assert waiting.wait(5)
        holder.start()
        assert held.wait(5)
        holder.kill()
        procs.join_or_kill(holder, timeout=2)
        cond.notify()
        assert procs.join_or_kill(waiter) == 0
    finally:
        cond.close()
        mutex.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
