import threading
import time

import pytest

import posixipc
from posixipc.linux import Futex


def test_futex_wait_changed_value():
    f = Futex()
    assert f.value == 0
    assert f.wait(1, timeout=0.05) is True
    f.value = 1
    assert f.wait(1, timeout=0.05) is False
    f.close()
    with pytest.raises(posixipc.ClosedError):
        _ = f.value


def test_futex_wake_waiter():
    f = Futex()
    done = threading.Event()

    def waiter():
        assert f.wait(0, timeout=2.0) is True
        done.set()

    t = threading.Thread(target=waiter)
    t.start()
    time.sleep(0.05)
    f.value = 1
    f.wake(1)
    assert done.wait(2)
    t.join(2)
    f.close()
