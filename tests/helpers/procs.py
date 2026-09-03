import multiprocessing
import os
import struct
import time

import posixipc


def spawn_ctx():
    return multiprocessing.get_context("spawn")


def join_or_kill(proc, timeout=10):
    proc.join(timeout)
    if proc.is_alive():
        proc.kill()
        proc.join(2)
        raise RuntimeError(f"process {proc.pid} hung")
    return proc.exitcode


def recover_noop(mutex):
    del mutex


def make_robust_layout(recover=recover_noop):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    blob = layout.add_bytes(64)
    return layout, mutex, blob


def make_plain_layout():
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    blob = layout.add_bytes(64)
    return layout, mutex, blob


def hold_robust_until_killed(name, ready):
    layout, mutex, _blob = make_robust_layout()
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def hold_plain_until_killed(name, ready):
    layout, mutex, _blob = make_plain_layout()
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def blocked_acquire_interruptible(name, ready):
    layout, mutex, _blob = make_plain_layout()
    layout.attach(name)
    ready.set()
    try:
        mutex.acquire()
    except KeyboardInterrupt:
        os._exit(0)
    os._exit(2)


def blocked_acquire_raw(name, ready):
    layout, mutex, _blob = make_plain_layout()
    layout.attach(name)
    ready.set()
    mutex.acquire(interruptible=False)
    os._exit(3)


def bump_counter(name, n, ready, start):
    layout, mutex, blob = make_plain_layout()
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        with mutex:
            value = struct.unpack_from("<Q", buf, 0)[0]
            struct.pack_into("<Q", buf, 0, value + 1)
    buf.release()


def child_layout_report(name, conn):
    layout, mutex, blob = make_plain_layout()
    layout.attach(name)
    conn.send(
        {
            "digest": layout.digest,
            "mutex_offset": mutex.offset,
            "blob_offset": blob.offset,
            "slots": layout.slots,
        }
    )
    conn.close()


def child_pickle_acquire(payload, name, recover_ok):
    import pickle

    mutex = pickle.loads(payload)
    if recover_ok:
        mutex.on_owner_died = recover_noop
    with mutex:
        pass
    os._exit(0)


def make_queue_layout(depth=4, item_size=8):
    layout = posixipc.Layout()
    queue = layout.add(posixipc.Queue, depth=depth, item_size=item_size)
    return layout, queue


def make_futex_queue_layout(depth=4, item_size=8):
    from posixipc.linux import FutexQueue

    layout = posixipc.Layout()
    queue = layout.add(FutexQueue, depth=depth, item_size=item_size)
    return layout, queue


def child_queue_put(name, depth, item_size, data, ready, start):
    layout, queue = make_queue_layout(depth, item_size)
    layout.attach(name)
    ready.set()
    start.wait()
    if not queue.put(data, timeout=5.0):
        os._exit(2)
    queue.close()
    os._exit(0)


def child_queue_get(name, depth, item_size, ready, start, conn):
    layout, queue = make_queue_layout(depth, item_size)
    layout.attach(name)
    ready.set()
    start.wait()
    item = queue.get(timeout=5.0)
    conn.send(item)
    queue.close()
    os._exit(0)


def child_queue_block_put(name, depth, item_size, ready):
    layout, queue = make_queue_layout(depth, item_size)
    layout.attach(name)
    if queue.put(b"\x00" * item_size, blocking=False):
        os._exit(4)
    ready.set()
    queue.put(b"\x00" * item_size, interruptible=False)
    os._exit(3)


def child_queue_block_get(name, depth, item_size, ready):
    layout, queue = make_queue_layout(depth, item_size)
    layout.attach(name)
    if queue.get(blocking=False) is not None:
        os._exit(4)
    ready.set()
    queue.get(interruptible=False)
    os._exit(3)


def child_futex_queue_get(name, depth, item_size, ready, start, conn):
    layout, queue = make_futex_queue_layout(depth, item_size)
    layout.attach(name)
    ready.set()
    start.wait()
    item = queue.get(timeout=5.0)
    conn.send(item)
    queue.close()
    os._exit(0)


def child_futex_queue_block_put(name, depth, item_size, ready):
    layout, queue = make_futex_queue_layout(depth, item_size)
    layout.attach(name)
    if queue.put(b"\x00" * item_size, blocking=False):
        os._exit(4)
    ready.set()
    queue.put(b"\x00" * item_size, interruptible=False)
    os._exit(3)


def child_memfd_read(region, conn):
    mv = memoryview(region)
    conn.send(bytes(mv[:5]))
    mv.release()
    region.close()
    os._exit(0)


def child_eventfd_read(efd, conn):
    conn.send(efd.read())
    efd.close()
    os._exit(0)


def child_hold_slot_mutex(name, slot, kind, digest, ready):
    mutex = posixipc._posixipc._attach_slot(name, slot, kind, digest)
    mutex.acquire(interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def child_extra_add_attach(name, conn):
    layout, _mutex, _blob = make_plain_layout()
    layout.add(posixipc.Mutex)
    try:
        layout.attach(name, timeout=1.0)
    except posixipc.LayoutMismatchError:
        conn.send("mismatch")
    except Exception as exc:
        conn.send(type(exc).__name__)
    else:
        conn.send("ok")
    conn.close()


def _attach_queue(name, depth, item_size, use_futex):
    if use_futex:
        layout, queue = make_futex_queue_layout(depth, item_size)
    else:
        layout, queue = make_queue_layout(depth, item_size)
    layout.attach(name)
    return queue


def child_queue_produce_range(name, depth, item_size, start_id, count, tag, ready, start, use_futex):
    queue = _attach_queue(name, depth, item_size, use_futex)
    ready.set()
    start.wait()
    for i in range(count):
        if not queue.put(struct.pack("<I4s", start_id + i, tag), timeout=15.0):
            os._exit(2)
    queue.close()
    os._exit(0)


def child_queue_consume_n(name, depth, item_size, count, ready, start, conn, use_futex):
    queue = _attach_queue(name, depth, item_size, use_futex)
    ready.set()
    start.wait()
    got = []
    for _ in range(count):
        item = queue.get(timeout=15.0)
        if item is None:
            os._exit(2)
        got.append(item)
    conn.send(got)
    queue.close()
    os._exit(0)


def child_robust_bump(name, n, ready, start):
    layout, mutex, blob = make_robust_layout()
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        with mutex:
            value = struct.unpack_from("<Q", buf, 0)[0]
            struct.pack_into("<Q", buf, 0, value + 1)
    buf.release()
    mutex.close()
    blob.close()


def child_open_or_create_bump(name, n, ready, start):
    layout, mutex, blob = make_plain_layout()
    layout.open_or_create(name, timeout=5.0)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        with mutex:
            value = struct.unpack_from("<Q", buf, 0)[0]
            struct.pack_into("<Q", buf, 0, value + 1)
    buf.release()
    mutex.close()
    blob.close()


def make_robust_array_layout(count=2):
    def recover(item):
        del item

    layout = posixipc.Layout()
    locks = layout.add_array(posixipc.RobustMutex, count, on_owner_died=recover)
    return layout, locks


def child_hold_array_index(name, index, ready):
    layout, locks = make_robust_array_layout()
    layout.attach(name)
    locks.acquire(index, interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def child_cond_wait_flag(name, ready):
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


def child_robust_cond_wait(name, ready):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover_noop)
    cond = layout.add(posixipc.Condition, mutex=mutex)
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    cond.wait()
    mutex.release()
    cond.close()
    mutex.close()


def child_hold_robust_cond(name, ready):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover_noop)
    layout.add(posixipc.Condition, mutex=mutex)
    layout.attach(name)
    mutex.acquire(interruptible=False)
    ready.set()
    while True:
        time.sleep(1)


def child_sem_bump(name, n, ready, start, value=2):
    layout = posixipc.Layout()
    sem = layout.add(posixipc.Semaphore, value=value)
    blob = layout.add_bytes(8)
    layout.attach(name)
    buf = memoryview(blob)
    ready.set()
    start.wait()
    for _ in range(n):
        sem.acquire()
        count = struct.unpack_from("<Q", buf, 0)[0]
        struct.pack_into("<Q", buf, 0, count + 1)
        sem.release()
    buf.release()
    sem.close()
    blob.close()


def child_mq_put_range(name, start_id, count, ready, start):
    mq = posixipc.NamedMessageQueue.attach(name)
    ready.set()
    start.wait()
    for i in range(count):
        if not mq.put(struct.pack("<I", start_id + i).ljust(32, b"\x00"), timeout=10.0):
            os._exit(2)
    mq.close()
    os._exit(0)
