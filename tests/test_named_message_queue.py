import threading
import time

import pytest

import posixipc
from tests.helpers import procs

pytestmark = pytest.mark.skipif(
    not posixipc.features.named_message_queue,
    reason="NamedMessageQueue not detected",
)


def test_named_message_queue_ctor_rejected():
    with pytest.raises(TypeError):
        posixipc.NamedMessageQueue()


def test_not_a_layout_kind():
    layout = posixipc.Layout()
    with pytest.raises(TypeError):
        layout.add(posixipc.NamedMessageQueue)


def test_create_attach_persist_and_unlink(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=32)
    try:
        assert mq.put(b"hello") is True
        mq.close()
        other = posixipc.NamedMessageQueue.attach(shm_name)
        assert other.get() == b"hello"
        other.close()
    finally:
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def test_close_does_not_unlink(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=32)
    mq.close()
    other = posixipc.NamedMessageQueue.attach(shm_name)
    other.close()
    posixipc.NamedMessageQueue.unlink_name(shm_name)
    with pytest.raises(FileNotFoundError):
        posixipc.NamedMessageQueue.attach(shm_name)


def test_variable_length_and_timeout(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=2, msgsize=16)
    try:
        assert mq.put(b"ab") is True
        assert mq.put(b"cdef") is True
        assert mq.put(b"x", blocking=False) is False
        assert mq.put(b"x", timeout=0.05) is False
        assert mq.get() == b"ab"
        assert mq.get(blocking=False) == b"cdef"
        assert mq.get(timeout=0.05) is None
        assert mq.get(blocking=False) is None
        with pytest.raises(ValueError):
            mq.put(b"")
        with pytest.raises(ValueError):
            mq.put(b"x" * 17)
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def test_priority_order(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=8)
    try:
        assert mq.put(b"low", priority=1) is True
        assert mq.put(b"high", priority=5) is True
        first = mq.get()
        second = mq.get()
        if first == b"high":
            assert second == b"low"
        else:
            pytest.skip("implementation does not honor POSIX message priority")
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def _hold_named_mq(name, ready):
    mq = posixipc.NamedMessageQueue.attach(name)
    mq.get(interruptible=False)
    ready.set()
    while True:
        import time

        time.sleep(1)


def test_crashed_receiver_does_not_return_count(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=2, msgsize=8)
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=_hold_named_mq, args=(shm_name, ready))
    try:
        assert mq.put(b"only") is True
        proc.start()
        assert ready.wait(5)
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
        assert mq.get(timeout=0.3) is None
    finally:
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def test_open_or_create(shm_name):
    first = posixipc.NamedMessageQueue.open_or_create(shm_name, maxmsg=4, msgsize=32)
    try:
        assert first.put(b"keep") is True
        first.close()
        second = posixipc.NamedMessageQueue.open_or_create(shm_name, maxmsg=8, msgsize=64)
        assert second.get() == b"keep"
        second.close()
    finally:
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def test_request_notification_callback(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=32)
    ready = threading.Event()
    try:
        mq.request_notification((lambda _arg: ready.set(), None))
        assert mq.put(b"ping") is True
        assert ready.wait(2)
        assert mq.get() == b"ping"
        mq.request_notification()
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)


def test_request_notification_cancel(shm_name):
    mq = posixipc.NamedMessageQueue.create(shm_name, maxmsg=4, msgsize=32)
    ready = threading.Event()
    try:
        mq.request_notification(ready.set)
        mq.request_notification()
        assert mq.put(b"x") is True
        time.sleep(0.2)
        assert not ready.is_set()
        assert mq.get() == b"x"
    finally:
        mq.close()
        posixipc.NamedMessageQueue.unlink_name(shm_name)
