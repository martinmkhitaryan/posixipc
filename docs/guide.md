# Client guide

How to use `posixipc` from application code. The public contract is
[`README.md`](../README.md). Queue slot layouts are in
[`queue.md`](queue.md). Recovery and trust rules are in
[`design.md`](design.md). Runnable samples are in
[`examples/`](examples/).

This file is the map: why the pieces exist, what each type is, and
which one to reach for.

---

## Architectural decisions

**The Python object is a handle.** The mutex, condvar, or ring lives in
storage the handle points at. Process-shared primitives live in a named
`mmap` segment. Process-private ones live in the handle. `close()` drops
the handle; it does not destroy a shared primitive.

**One `Layout`, two roles.** The parent calls `create()`. Every other
process builds the same layout and calls `attach()`. Do not do both in
one process. `add()` returns unbound handles; `create` / `attach` /
`open_or_create` bind them and seal the layout.

**The creator inits. Attachers never init.** `pthread_*_init` runs only
on the create path. An attacher that inits would race and corrupt the
segment.

**The digest is a mistake detector.** Same `add()` sequence → same
digest → attach succeeds. A different kind, depth, or `item_size` is
`LayoutMismatchError`. Anyone who can write the segment can forge the
digest. That is why `attach()` also checks `st_uid` and mode.

**Crash recovery is application code.** POSIX can say the owner died. It
cannot repair the bytes that owner was writing. `RobustMutex` requires
`on_owner_died`. `Queue` / `FutexQueue` supply their own recover
(drop a partial put, keep a partial get). A crashed `Semaphore` or
`RWLock` holder does not get a callback; the primitive stays wedged or
short.

**Missing primitives are missing names.** If `mq_open` was not detected,
`NamedMessageQueue` is not in the namespace. Check `features`. There is
no Python fallback.

**Three queues, three types.** `Queue` is a layout ring. `NamedMessageQueue`
is `mq_*`. `posixipc.linux.FutexQueue` is a Linux ring. None is an alias
of another. Digests will not match across `Queue` and `FutexQueue`.

**Linux stays in `posixipc.linux`.** `Futex`, `EventFD`, `MemFD`, and
`FutexQueue` are not in `posixipc.__all__`. Importing `posixipc.linux`
off Linux raises `ImportError`.

**`close()` and `unlink()` are independent.** Close unmaps when the pin
count is zero. Unlink removes the name. POSIX works the same way.

**Omit-vs-`None` on attach is not an acquire.** Omitting `timeout` on
`Layout.attach` / `open_or_create` is 5 seconds. `timeout=None` waits
forever. On `acquire()`, omitted `timeout` means forever.

---

## Entities

### Always present

| Entity | What it is | Typical use |
| --- | --- | --- |
| `Layout` | Factory for a typed shared-memory segment | Parent `create`, children `attach` |
| `SharedMemory` | The mapped region (or a raw untyped segment) | Returned by `create` / `attach`; also raw `create` + `attach_unchecked` |
| `Mutex` | `ERRORCHECK` pthread mutex | Threads, or processes that will not crash holding it |
| `RobustMutex` | Robust pthread mutex | Processes that may die holding the lock |
| `RWLock` | pthread rwlock | Many readers, rare writers; not crash-safe |
| `Condition` | pthread condvar | Wait for a predicate under a mutex |
| `Semaphore` | Unnamed `sem_t` | Counting permits in a layout |
| `Queue` | Bounded byte ring in a layout | Process-shared work items of fixed size |
| `MutexArray` | N mutex slots, one handle | `add_array`; `arr[i]` is `MutexArrayItem` |
| `MutexArrayItem` | One index of an array | What `on_owner_died` receives for arrays |
| `SharedBytes` | One `add_bytes` slot | Buffer protocol into the payload |
| `features` | Immutable capability inventory | `if features.barrier:` |
| `__build_info__` | Raw C probe dict | Prefer `features` |
| `TimeoutError` | The **builtin** | Attach / open deadline expired |

### Present when detected

| Entity | What it is | Typical use |
| --- | --- | --- |
| `Barrier` | pthread barrier | N processes rendezvous |
| `NamedSemaphore` | `sem_open` | A named gate, not a layout slot |
| `NamedMessageQueue` | `mq_*` | Kernel-backed messages, persist after close |
| `SpinLock` | pthread spinlock | `from posixipc.spinlock import SpinLock`. No timeout. |

### `posixipc.linux` only

| Entity | What it is | Typical use |
| --- | --- | --- |
| `FutexQueue` | Ring + futex wait | Same `put`/`get` as `Queue`, Linux wait |
| `Futex` | Process-private futex word | In-process wait/wake |
| `EventFD` | `eventfd(2)` | Integrate with `select` / `epoll` |
| `MemFD` | `memfd_create` | Anonymous mapping; `memfd()` is `MemFD.create` |
| `linux.features` | Linux capability flags | `futex`, `eventfd`, `memfd`, `futex_queue` |

### Errors

| Exception | Meaning |
| --- | --- |
| `PosixIPCError` | Base |
| `NotRecoverableError` | Robust mutex is poisoned; you do **not** hold it |
| `ClosedError` | Handle was closed |
| `LayoutMismatchError` | Digest / ABI / `BROKEN` segment |
| `TimeoutError` | Builtin; attach or open timed out |

`acquire()` timeout is `False`, never an exception.

---

## Usage

### Threads in one process

No layout.

```python
import posixipc

m = posixipc.Mutex()
with m:
    ...
m.close()
```

Prefer `threading.Lock` unless you already need a posixipc type.

### Shared state between processes

Same class in both processes. Parent creates; child attaches.

```python
import posixipc

NAME = "/myapp.state"


class App:
    def __init__(self):
        self.layout = posixipc.Layout()
        self.mutex = self.layout.add(posixipc.Mutex)
        self.state = self.layout.add_bytes(4096)

    def run_parent(self):
        region = self.layout.create(NAME)
        try:
            with self.mutex:
                memoryview(self.state)[:4] = b"init"
        finally:
            self.mutex.close()
            self.state.close()
            region.close()

    def run_child(self):
        region = self.layout.attach(NAME)
        try:
            with self.mutex:
                ...
        finally:
            self.mutex.close()
            self.state.close()
            region.close()
            posixipc.SharedMemory.unlink_name(NAME)
```

### Holder may crash

Use `RobustMutex` and repair **your** bytes in `on_owner_died`.

```python
class App:
    def __init__(self):
        self.layout = posixipc.Layout()
        self.mutex = self.layout.add(
            posixipc.RobustMutex,
            on_owner_died=self.recover,
        )
        self.state = self.layout.add_bytes(8)

    def recover(self, mutex):
        # You hold `mutex`. Fix self.state, then return.
        memoryview(self.state)[:] = b"\x00" * 8
```

If `recover` raises, the library unlocks and poisons the mutex. The next
acquire is `NotRecoverableError`.

See [`examples/crash_recovery.py`](examples/crash_recovery.py).

### Wait for a predicate

`Condition` must name its mutex at `add()`. Always `while`.

```python
self.mutex = self.layout.add(posixipc.RobustMutex, on_owner_died=self.recover)
self.cond = self.layout.add(posixipc.Condition, mutex=self.mutex)

with self.mutex:
    while not ready(self.state):
        self.cond.wait()
    self.cond.notify_all()
```

`wait(blocking=False)` is `ValueError`. Timeout returns `False`.

If the mutex is robust and the owner dies while a waiter is in
`wait()`, recovery runs before `wait()` returns.

### Counting permits

Layout-backed unnamed semaphore:

```python
sem = layout.add(posixipc.Semaphore, value=2)
sem.acquire()
sem.release()
```

Named, not a layout slot — persists after `close()` until `unlink()`:

```python
if posixipc.features.named_semaphore:
    gate = posixipc.NamedSemaphore.create("/myapp.gate", value=1)
    with gate:
        ...
    gate.close()
    posixipc.NamedSemaphore.unlink_name("/myapp.gate")
```

A crashed named-semaphore holder does not give the count back.

### Bounded byte jobs between processes

`Queue`: fixed `item_size`, layout digest, library recover.

```python
q = layout.add(posixipc.Queue, depth=32, item_size=256)
# parent
q.put(payload)           # False on timeout
item = q.get(timeout=1)  # None on timeout
```

`data` must be exactly `item_size` bytes. No pickle inside `put`/`get`.

Linux-only, same methods, different wait:

```python
from posixipc.linux import FutexQueue

q = layout.add(FutexQueue, depth=32, item_size=256)
```

Do not attach a `Queue` segment as a `FutexQueue` (or the reverse).

See [`queue.md`](queue.md) and [`examples/queue.py`](examples/queue.py).

### Kernel message queue

When you want the name to outlive the mapping, or priorities:

```python
if posixipc.features.named_message_queue:
    mq = posixipc.NamedMessageQueue.create("/myapp.jobs", maxmsg=8, msgsize=256)
    mq.put(b"job", priority=1)
    msg = mq.get()
    mq.close()
    posixipc.NamedMessageQueue.unlink_name("/myapp.jobs")
```

`open_or_create` does not resize an existing name. A notify callable
runs on the `mq_notify` thread; exceptions there are unraisable.

### Many locks, one pin

```python
locks = layout.add_array(posixipc.Mutex, 32)
locks.acquire(3)
locks.release(3)
with locks[7]:
    ...
```

`add_array(RobustMutex, N, on_owner_died=recover)` — one callback for
every index. `SIGKILL` on one index does not poison the others.

### Rendezvous

```python
if posixipc.features.barrier:
    bar = layout.add(posixipc.Barrier, parties=4)
    serial = bar.wait()  # True for exactly one waiter
```

A crashed participant hangs the rest. That is POSIX.

### Linux extras

```python
from posixipc.linux import EventFD, Futex, memfd

f = Futex()
f.wait(0, timeout=0.1)
f.wake(1)

e = EventFD()
e.write(1)
assert e.read() == 1

region = memfd(4096)
mv = memoryview(region)
mv[0] = 1
mv.release()
```

`EventFD` / `MemFD` pickle with `multiprocessing.reduction.DupFd`, not
the named-shm path.

### Other C extensions

`mutex.as_capsule()` yields `posixipc.mutex.v1`. Include the installed
`posixipc.h` and call `posixipc_mutex_from_capsule`. The capsule pins
the segment; `region.close()` raises `BufferError` until it is dropped.

---

## Which type

| Situation | Type |
| --- | --- |
| Threads only, no crash story | `threading.Lock` |
| Processes, holder will not die | `Layout` + `Mutex` |
| Processes, holder may `SIGKILL` | `RobustMutex` + `on_owner_died` |
| Readers / writers, no crash story | `RWLock` |
| Wait until state changes | `Condition` on that mutex |
| N identical locks | `MutexArray` |
| Fixed-size bytes, crash-safe ring | `Queue` |
| Same ring, Linux wait | `linux.FutexQueue` |
| Named jobs, kernel queue | `NamedMessageQueue` |
| Named permit | `NamedSemaphore` |
| N-way start line | `Barrier` |
| Raw bytes, no directory | `SharedMemory.create` (tools, not apps) |

---

## Lifetime checklist

1. Build the same `Layout` in every process (same `add()` order).
2. Parent `create`; children `attach`. Unlink when the name should go.
3. `close()` every handle, then the region.
4. Do not `unlink` while another process still needs the name.
5. After pickle, assign `on_owner_died` again before the first acquire.
6. `features.queue` is always `True`. `named_message_queue` is the same
   bit as `features.mq`.

---

## Easy to misread

- `Layout.attach` / `open_or_create`: omitted `timeout` is 5s; `None` is
  forever. `acquire()` is the opposite.
- Pickle checks one slot + digest, not the full directory.
  Unpickled `Condition` drops `on_owner_died` on its mutex.
- `Queue` waits always use the put lock. Mutex 1 is in the layout for
  the digest only.
- `NamedMessageQueue.open_or_create` ignores `maxmsg` / `msgsize` if
  the name exists. `NamedSemaphore` has no `open_or_create`.
- `request_notification` callbacks run on the `mq_notify` thread.
  Exceptions are unraisable.
- `Condition.wait(blocking=False)` and `SpinLock.acquire(timeout=…)`
  are `ValueError`.
- `add()` after `create` / `attach` / `open_or_create` raises (sealed).
- `SharedMemory.open_or_create` is untyped (digest 0), same 5s default.
- `linux.features.futex_queue` is futex **and** robust mutex, not a
  separate probe.
- `__all__` lists `Barrier` / `NamedSemaphore` / `NamedMessageQueue`
  even when those imports were skipped. Use `features`.
