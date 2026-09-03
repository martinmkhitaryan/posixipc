import os
import signal
import time

import posixipc
from tests.helpers import procs


def test_sigint_interrupts_blocked_acquire(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    mutex.acquire()
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.blocked_acquire_interruptible, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        time.sleep(0.2)
        os.kill(proc.pid, signal.SIGINT)
        code = procs.join_or_kill(proc, timeout=2)
        assert code == 0
    finally:
        mutex.release()
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_interruptible_false_needs_sigkill(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    mutex.acquire()
    ctx = procs.spawn_ctx()
    ready = ctx.Event()
    proc = ctx.Process(target=procs.blocked_acquire_raw, args=(shm_name, ready))
    proc.start()
    try:
        assert ready.wait(5)
        time.sleep(0.2)
        os.kill(proc.pid, signal.SIGINT)
        proc.join(0.4)
        assert proc.is_alive()
        proc.kill()
        procs.join_or_kill(proc, timeout=2)
    finally:
        mutex.release()
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
