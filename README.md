# posixipc

[![PyPI version](https://img.shields.io/pypi/v/posixipc)](https://pypi.org/project/posixipc/)
[![Python 3.12+](https://img.shields.io/badge/python-3.12+-blue.svg)](https://www.python.org/downloads/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

POSIX IPC and synchronization for Python. CPython C extension over `pthread_*`,
`sem_*`, and POSIX shared memory.

> [!WARNING]
> This code was written with Cursor (AI, multi-agent). It has not had a
> human review. I will review it later, then publish packages.

`RobustMutex` survives the death of the process that held it. POSIX reports
`EOWNERDEAD`; it does not repair the bytes that owner was writing. Register
`on_owner_died`. The next `acquire()` calls it while you hold the lock, marks
the mutex consistent, and returns.

See [`docs/guide.md`](docs/guide.md) for types and usage,
[`docs/queue.md`](docs/queue.md) for queue layouts,
[`docs/examples/`](docs/examples/) for samples,
[`docs/design.md`](docs/design.md) for recovery and trust notes, and
[`CHANGELOG.md`](CHANGELOG.md) for releases.

## Contents

- [Install](#install)
- [Quick start](#quick-start)
- [When to use](#when-to-use)
- [API](#api)
- [Design](#design)
  - [Object model](#object-model)
  - [Shared memory](#shared-memory)
  - [Layout and digest](#layout-and-digest)
  - [Robust mutex](#robust-mutex)
  - [Timeouts and clocks](#timeouts-and-clocks)
  - [Signals](#signals)
  - [GIL and free-threaded CPython](#gil-and-free-threaded-cpython)
  - [Errors](#errors)
  - [Lifetime](#lifetime)
  - [Pickle](#pickle)
  - [Fork](#fork)
  - [Security](#security)
  - [C API](#c-api)
- [Primitives](#primitives)
- [Performance](#performance)
- [Platform](#platform)
- [Building](#building)
- [Testing](#testing)
- [Changelog](CHANGELOG.md)
- [License](#license)

---

## Install

```bash
pip install posixipc
```

Requires CPython 3.12+ on Linux. Build from source needs a C11 compiler,
CMake ≥ 3.20, and CPython development headers.

---

## Quick start

Do not `create()` and `attach()` in the same process. Put the layout and
`on_owner_died` in a module both processes import.

```python
import posixipc

NAME = "/myapp.state"


class App:
    def __init__(self):
        self.layout = posixipc.Layout()
        self.mutex = self.layout.add(
            posixipc.RobustMutex,
            on_owner_died=self.recover,
        )
        self.cond = self.layout.add(posixipc.Condition, mutex=self.mutex)
        self.state = self.layout.add_bytes(4096)

    def recover(self, mutex):
        # Previous owner died. You hold the lock. Repair self.state.
        # Return → consistent, acquire succeeds.
        # Raise → unlock (poison), exception propagates.
        ...


app = App()
app.layout.create(NAME)
with app.mutex:
    while not ready():
        app.cond.wait()
    use(app.state)

# other process
app = App()
app.layout.attach(NAME)
with app.mutex:
    use(app.state)

ok = app.mutex.acquire(timeout=1.0)  # False on timeout
if ok:
    try:
        use(app.state)
    finally:
        app.mutex.release()
```

`add()` returns unbound handles. `create` / `attach` / `open_or_create` bind
them and seal the layout; further `add()` raises `RuntimeError`.

Process-private lock, no layout:

```python
m = posixipc.Mutex()
with m:
    ...
```

---

## When to use

| Need | Use |
| --- | --- |
| Threads in one process | `threading.Lock` |
| Processes, holder will not crash | `posixipc.Mutex` or `multiprocessing.Lock` |
| Processes, holder may crash | `RobustMutex` + `on_owner_died` |
| Primitives in your own shared-memory layout | `posixipc` |

`posixipc.Mutex` is not faster than `threading.Lock` in-process. See
[Performance](#performance).

---

## API

`RobustMutex` requires `on_owner_died(mutex)`. Rejected at `add()` /
construction without it. No `acquire_recoverable()`, `LockState`, or
`OwnerDiedError`.

```python
from posixipc import (
    SharedMemory,
    Layout,
    Mutex,
    RobustMutex,
    RWLock,
    Condition,
    Semaphore,
    Queue,
    features,
    PosixIPCError,
    NotRecoverableError,
    ClosedError,
    LayoutMismatchError,
    TimeoutError,
)
```

`Barrier`, `NamedSemaphore`, and `NamedMessageQueue` are in the same
namespace only when detected. Otherwise `from posixipc import Barrier` is
`ImportError`. Check `features`.

`SpinLock` is imported from `posixipc.spinlock` (or `posixipc._posixipc`)
when `features.spinlock` is set. `acquire(timeout=…)` is `ValueError`.

`MutexArray` comes from `layout.add_array`. `arr[i]` is a `MutexArrayItem`
(same object type `on_owner_died` receives). `layout.add_bytes` returns
`SharedBytes` (buffer protocol into that slot).

`__build_info__` is the raw C probe dict. Prefer `features`.

Linux-only types live in `posixipc.linux`: `Futex`, `EventFD`, `MemFD`,
`memfd`, `FutexQueue`, and `linux.features` (`futex`, `eventfd`, `memfd`,
`futex_queue`).

`SharedMemory.create` / `attach_unchecked` / `open_or_create` are untyped
regions (digest `0`). Application code should use `Layout`.

---

## Design

### Object model

The Python object is a handle. The primitive lives in storage the handle
points at. A process-shared `pthread_mutex_t` must sit in `mmap(MAP_SHARED)`;
a `PyObject` sits on the interpreter heap. Shared handles therefore point
into the mapping. Private handles keep the primitive inline.

```c
typedef struct {
    PyObject_HEAD
    pthread_mutex_t   *lock;     /* shm slot, or inline_storage */
    PyObject          *region;   /* SharedMemory, or NULL if private */
    uint32_t           slot;
    _Atomic uint32_t   flags;
    pthread_mutex_t    inline_storage;
    PyObject          *on_owner_died;
} PosixIPCMutexObject;
```

`OWNS_STORAGE` is set only for private inline storage. Shared handles never
own the pthread object. `region` keeps the Python object alive; a pin keeps
the mapping alive. `flags` is `_Atomic` on every build.

### Shared memory

Implemented with `shm_open` / `ftruncate` / `mmap` / `munmap` / `shm_unlink`,
not `multiprocessing.shared_memory`.

Typed segments start with a 64-byte header:

```c
typedef struct {
    uint32_t magic;              /* 0x50495043u ('PIPC') */
    uint16_t layout_version;
    uint16_t slot_count;
    uint32_t abi_tag;
    uint32_t flags;
    uint32_t total_size;
    uint32_t directory_bytes;
    _Atomic uint32_t state;      /* UNINIT=0 | INITIALIZING | READY | BROKEN */
    uint32_t layout_digest;
    uint32_t reserved[8];
} posixipc_shm_header;

_Static_assert(sizeof(posixipc_shm_header) == 64, "header must be one cache line");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "header state must be lock-free");
```

`total_size` and slot `offset` are `uint32_t`. Segments larger than
4 GiB − 1 raise `OverflowError` at `create()`. Size checks are
`size <= total_size - offset`.

`UNINIT` is 0 (`ftruncate` zero-fills). `open_or_create` CASes
`INITIALIZING`. If the creator fails after the claim, `BROKEN` is stored
with release ordering and attachers raise.

Slot directory after the header, one record per `add()`:

```c
typedef struct {
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
} posixipc_slot;
```

`Mutex` and `RobustMutex` have different `kind` values. `abi_tag` includes
arch, libc family, sizeof/alignof, and cache-line stride. glibc and musl
`pthread_mutex_t` are both 40 bytes on x86-64 and are not interchangeable.

`Layout.create()`:

1. `shm_open(O_CREAT|O_EXCL|O_RDWR, 0600)`, `fstat` `st_uid == geteuid()`
   and `(st_mode & 077) == 0`.
2. `ftruncate` to header + directory + slots.
3. `mmap`, write header (except `state`), write directory, `*_init` every slot.
4. Store `READY` with release ordering.
5. Bind handles.

`attach()`:

1. `shm_open` existing, same `fstat`.
2. Wait until `st_size >= 64`, map the header, wait for `READY` or `BROKEN`.
3. Check `total_size` against `st_size` and the caller's expected size
   before mapping the rest.
4. Verify magic, versions, `abi_tag`, digest, directory.
5. Bind handles. Never `*_init`.

`open_or_create()` is create, then attach on `EEXIST`, with a bounded retry
if the name disappears. If the creator dies after `O_EXCL` and before
`READY`, attachers time out (`TimeoutError`) unless a supervisor unlinks
and recreates.

`close()` unmaps when the pin count is zero. `unlink()` removes the name.

### Layout and digest

```python
layout = posixipc.Layout()
mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover, prio_inherit=False)
cond = layout.add(posixipc.Condition, mutex=mutex)
sem = layout.add(posixipc.Semaphore, value=1)
bar = layout.add(posixipc.Barrier, parties=4)  # if features.barrier
blob = layout.add_bytes(4096)
locks = layout.add_array(posixipc.Mutex, 32)
q = layout.add(posixipc.Queue, depth=32, item_size=256)

region = layout.create("/myapp.state")
region = layout.attach("/myapp.state", timeout=5.0)
region = layout.open_or_create("/myapp.state", timeout=5.0)
```

Omitting `timeout` on `attach` / `open_or_create` is 5 seconds.
`timeout=None` waits forever. On `acquire()`, omitted `timeout` waits forever.

Offsets come from the slot sequence and the build ABI. The same `add()`
order in two processes produces the same directory.

`layout.digest` is FNV-1a 32 in C (`posixipc_layout_digest`), not
`hash()`. Encoding:

- Basis `0x811C9DC5`, prime `0x01000193`.
- `uint16 layout_version` LE, `uint32 abi_tag` LE,
  `uint32 POSIXIPC_CACHELINE_BYTES` LE.
- Per slot, add-order: `uint16 kind`, `uint16 align`, `uint32 size`,
  `uint32 init_flags`, all LE.
- `add(X); add(X)` is two slots. `add_array(X, 2)` is the same encoding.
- Digest `0` is stored as `1`. Header digest `0` means untyped (no layout).

The digest catches mismatched layouts. It is not a tamper check. Anyone
who can write the segment can forge it.

`SharedMemory.create(name, size)` writes digest `0` and no directory.
There is no `SharedMemory.attach()`; the unchecked path is
`attach_unchecked()`.

### Robust mutex

`pthread_mutexattr_setrobust(..., PTHREAD_MUTEX_ROBUST)`.

On `EOWNERDEAD` you hold the lock. On `ENOTRECOVERABLE` you do not.

`on_owner_died` is required on `RobustMutex`.

- Called as `recover(mutex)` from the next `acquire()` / `with` /
  `Condition.wait()` that sees `EOWNERDEAD`.
- A zero-argument callable fails at recover time (`TypeError`).
- Handles are already bound. Recovery never runs before `create`/`attach`.
- Return: `pthread_mutex_consistent`, then acquire succeeds.
- Raise: `pthread_mutex_unlock` (poisons the mutex), exception propagates.
  Later acquires raise `NotRecoverableError`.
- Not pickled. Rebuild the layout with the same function, or assign
  `mutex.on_owner_died = recover` before the first acquire.

There is no Python `consistent()`. Other extensions use the capsule in
[`posixipc.h`](include/posixipc.h).

Linux delivers `EOWNERDEAD` on `SIGKILL` and `_exit` via `robust_list`.
Not delivered if the mapping is gone before exit, or across panic / power
loss. `close()` / unmap while this process holds a robust lock is
unsupported; the pin count makes `close()` fail instead.

### Timeouts and clocks

On acquire:

| `timeout` | Meaning |
| --- | --- |
| omitted / `None` | block, interruptible |
| `-1` or negative / NaN | `ValueError` |
| `0` | same as `blocking=False` |
| `> 0` | absolute deadline, computed once, on that primitive's clock |
| `blocking=False` and `timeout>0` | `ValueError` |

Acquire timeout returns `False`. Attach / open timeout raises
`TimeoutError`. `Condition.wait(blocking=False)` is `ValueError`.

Clocks are per primitive:

- **Condition:** `pthread_condattr_setclock(CLOCK_MONOTONIC)` at init.
- **Mutex / RWLock / Semaphore:** `*_clocklock` / `sem_clockwait` with
  `CLOCK_MONOTONIC` if `dlsym` finds them at import. musl does not; those
  primitives use `CLOCK_REALTIME` and
  `features.monotonic_timeouts["mutex"]` is `False`.
- Priority inheritance + monotonic needs `FUTEX_LOCK_PI2` (Linux ≥ 5.14
  and a glibc that uses it). `EINVAL` from `clocklock(MONOTONIC)` falls
  back to realtime and is reported on `features`.

`features` flags: `robust_mutex`, `prio_inherit`, `process_shared`,
`barrier`, `spinlock`, `named_semaphore`, `cond_monotonic`,
`monotonic_timeouts` (`mutex` / `rwlock` / `semaphore` / `condition` →
bool), `memfd`, `mq`, `named_message_queue` (same bit as `mq`), `queue`
(always `True`).

Deadlines are nanosecond-resolution, limited by the scheduler.

### Signals

Blocking acquires that have a timed POSIX variant wait in 50 ms slices
and call `PyErr_CheckSignals()` between slices. Handlers run on the main
thread only. `interruptible=False` opts out (main thread).

If `PyErr_CheckSignals()` returns `< 0`, the exception is already set;
the wait helper propagates it.

Ctrl-C latency is a slice plus up to `sys.getswitchinterval()` (5 ms
default) when reacquiring the GIL.

`Barrier.wait()` has no timed POSIX variant and is not interruptible.
`SpinLock` is `trylock` + yield with the GIL released, and checks
signals between yields.

`sem_wait` returns `EINTR` even with `SA_RESTART`. Semaphores use the
same wait helper.

### GIL and free-threaded CPython

`acquire()` calls `pthread_mutex_trylock` with the GIL held. On `EBUSY`
it releases the GIL and waits. `EOWNERDEAD` from trylock runs recovery
with the GIL held. Blocking paths never touch a `PyObject` while the GIL
is released.

Free-threaded builds: multi-phase init, heap types, per-module state,
`Py_mod_gil = Py_MOD_GIL_NOT_USED` (3.13+). Without that slot, import
re-enables the GIL and emits `RuntimeWarning`.

`flags` is `_Atomic`. `region` and `on_owner_died` change under a
per-object lock. `close()` vs in-flight waits is a pin count. Closing a
handle another thread is using is a caller bug; the result is
`ClosedError` or a failed `close()`, not use-after-`munmap`.

Atomics are unconditional, not `#ifdef Py_GIL_DISABLED`. Loads that
decide whether to dereference `lock` use `memory_order_acquire`.
`close()` stores `CLOSED` with release after the pin count hits zero.

### Errors

`pthread_*` returns errno and does not set `errno`. `sem_*`, `shm_open`,
`mmap`, `ftruncate` set `errno`. The C core returns a positive errno.
`posixipc_raise` does `errno = rc` then `PyErr_SetFromErrno`. Codes
`>= 5000` do not go through that path.

| Condition | Result |
| --- | --- |
| success | normal return |
| `ETIMEDOUT` / `EBUSY` on timed or non-blocking acquire | `False` |
| attach / open deadline | `TimeoutError` |
| `EOWNERDEAD` | `on_owner_died(mutex)`, then success |
| `ENOTRECOVERABLE` | `NotRecoverableError` |
| bad argument | `ValueError` |
| header / ABI / digest / slot / `BROKEN` | `LayoutMismatchError` |
| use after `close()`, or `close()` with pins | `ClosedError` or `BufferError` |
| double `release()`, unlock you do not own | `RuntimeError` |
| other errno | `OSError` |

```python
class PosixIPCError(Exception): ...

class NotRecoverableError(PosixIPCError):
    """Permanently unusable. You do not hold the lock."""

class ClosedError(PosixIPCError, ValueError): ...

class LayoutMismatchError(PosixIPCError, ValueError): ...

# TimeoutError is the builtin.
```

Default mutex type is `PTHREAD_MUTEX_ERRORCHECK`.

### Lifetime

1. Python wrapper — ends at deallocation.
2. Mapping — `munmap` when pins == 0.
3. Name — `shm_unlink` / `sem_unlink`.

Bound handles, `memoryview`s, and capsules increment the region pin.
`close()` with `pins > 0` raises `BufferError`. `tp_dealloc` of a shared
handle drops a pin; it does not `pthread_mutex_destroy` shared storage.

Private primitives: destroy is unlock-if-held, then
`pthread_mutex_destroy`. Shared slots are not destroyed. Teardown is
unlink plus the last mapping drop.

`close()` is idempotent. An owned private object collected without
`close()` emits `ResourceWarning`.

### Pickle

`copy.copy` / `copy.deepcopy` raise `TypeError`. Process-private handles
raise `TypeError` on pickle.

Bound shared handles pickle as a capability:
`_attach_slot(name, slot, kind, digest)`. `MutexArray` also stores
`count`. `Queue` / `FutexQueue` also store `depth` and `item_size`.
Unpickle checks that slot's kind and the digest, not the full directory.
`on_owner_died` is not in the payload. An unpickled `Condition` rebuilds
its mutex with `on_owner_died=None`.

`EventFD` and `MemFD` pickle via `multiprocessing.reduction.DupFd`.

Preferred spawn path: same factory in both processes so recovery is
registered again.

```python
def make_app(recover):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.RobustMutex, on_owner_died=recover)
    state = layout.add_bytes(4096)
    return layout, mutex, state

layout, mutex, state = make_app(recover)
layout.create(name)

# child
layout, mutex, state = make_app(recover)
layout.attach(name)
```

If you pickle the handle, assign `mutex.on_owner_died = recover` before
the first acquire.

`SharedMemory.__reduce__` pickles the name and re-attaches unchecked
after the same uid/mode `fstat`. Pickle layout handles, not the raw
region.

### Fork

No `pthread_atfork` handlers.

| Object | `fork()` |
| --- | --- |
| Shared, unlocked | Supported. Create the segment before forking. |
| Shared robust mutex **held** by the parent | Parent still owns it. Child must not unlock or acquire. Child exit does not produce `EOWNERDEAD` for the parent. |
| Private, unlocked, single-threaded fork | Child gets a copy. Avoid. |
| Private, held | Unsupported. |
| Multithreaded fork | Unsupported except fork-then-exec. |
| Named semaphore | Handle inherited; counts shared. |
| `SharedMemory` mapping | Inherited; re-attach by name under `spawn`. |

Prefer `spawn` or `forkserver`.

### Security

A writable POSIX shm object is trusted to every uid that can open it.
Header checks catch accidents. They do not make a hostile
`pthread_mutex_t` safe.

`create()` uses `0600` and requires `st_uid == geteuid()` and no
group/other write. `attach()` does the same. A same-uid squat is still
possible; use an unpredictable name or create from a supervisor.

`SharedMemory` buffer protocol is read-only on the header and directory.
Writable bytes are `region.payload`, after the directory. Writing a
primitive slot through a `memoryview` is unsupported.

### C API

Python call overhead dominates an uncontended lock. Other extensions can
lock through a capsule.

- `PyCapsule` `posixipc.mutex.v1` → `pthread_mutex_t *`, retains a pin.
- Installed `posixipc.h`: header layout, slot record, capsule accessors.
- `region.payload` as a `memoryview`.
- `MutexArray`: `arr.acquire(i)` / `arr[i]` / `arr.as_capsule(i)`.

```python
locks = layout.add_array(posixipc.Mutex, 32)
locks.acquire(0)
locks.release(0)
with locks[3]:
    pass
cap = locks.as_capsule(0)
```

```c
#include <posixipc.h>

pthread_mutex_t *m = posixipc_mutex_from_capsule(capsule);
posixipc_mutex_capsule_retain(capsule);
/* pthread_mutex_lock(m) */
posixipc_mutex_capsule_release(capsule);
```

---

## Primitives

| Class | Backing | Robust | Notes |
| --- | --- | --- | --- |
| `Mutex` | `pthread_mutex_t` | no | `ERRORCHECK`. `prio_inherit` optional; no effect on `SCHED_OTHER`. |
| `RobustMutex` | `pthread_mutex_t` | yes | Requires `on_owner_died`. |
| `RWLock` | `pthread_rwlock_t` | no | `acquire_read` / `acquire_write` / `read()` / `write()`. Crash holding write wedges it. |
| `Condition` | `pthread_cond_t` | no | Monotonic. Always `while`. `mutex=` required at `add()`. Shared cond + mutex must be in the same layout. `wait()` runs mutex recovery on `EOWNERDEAD`. `blocking=False` is `ValueError`. |
| `Semaphore` | unnamed `sem_t` | no | `release(n=1)`. `sem_post` is async-signal-safe in C, not from `signal.signal`. |
| `NamedSemaphore` | `sem_open` | no | `create` / `attach` / `unlink` (no `open_or_create`). Crashed holder does not release. Absent if undetected. |
| `MutexArray` | N mutex slots | * | `add_array`. `arr[i]` is `MutexArrayItem`. Robust if kind is `RobustMutex`. |
| `SharedBytes` | bytes slot | no | Return of `add_bytes`. Buffer protocol. |
| `Queue` | layout ring | yes | Five slots. CS is the put lock. Library recover. Exact `item_size` bytes. No process-private `Queue()`. |
| `linux.FutexQueue` | layout ring + futex | yes | Linux. Three slots. Digest ≠ `Queue`. |
| `linux.Futex` | `FUTEX_WAIT` / `WAKE` | no | Process-private `wait(expected)` / `wake(n)`. Not a layout kind. |
| `linux.EventFD` | `eventfd(2)` | no | `write` / `read` / `fileno`. `semaphore=` / `nonblock=`. Pickle via `DupFd`. |
| `linux.MemFD` | `memfd_create` | no | `memfd(size)` is `MemFD.create`. `memoryview` / `.payload`. Pickle via fd. |
| `NamedMessageQueue` | `mq_*` | no | `create` / `attach` / `open_or_create` / `unlink`. Existing name keeps `maxmsg`/`msgsize`. Notify callback runs on the notify thread. Not a layout kind. Absent if undetected. |
| `Barrier` | `pthread_barrier_t` | no | `wait()` is `True` for one waiter. Not interruptible. Crash hangs the rest. Absent if undetected. |
| `SpinLock` | `pthread_spinlock_t` | no | `trylock` + yield, GIL released. No timeout. Import `posixipc.spinlock`. |

No `Mutex.locked()`. POSIX cannot query a mutex without racing.

---

## Performance

Python call overhead dominates an uncontended native lock. Target is
parity with `threading.Lock` uncontended, and faster than
`multiprocessing.Lock` across processes.

Numbers: [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md). Re-run with
`python benchmarks/mutex_uncontended.py` (`taskset` if you can).

---

## Platform

| Tier | Platform |
| --- | --- |
| 1 | Linux x86-64, glibc, CPython 3.12–3.14 |
| 1 | Linux x86-64, glibc, CPython 3.13t/3.14t |
| 2 | Linux aarch64, glibc |
| 2 | Linux x86-64, musl |
| 3 | Other POSIX, feature-detected, not in CI |

musl is a different pthread implementation (often the same sizeof) and
lacks `*_clocklock`. It does implement `pthread_barrier_*`.

WSL is Linux. Native Windows is a separate backend (`WAIT_ABANDONED` +
file mapping), not this tree under MSVC. Not in CI.

---

## Building

```bash
pip install .
# or
pip install -e .
```

CMake via scikit-build-core. Links `Development.Module` only, not
`libpython`. Clock functions are `dlsym`'d at import.

C tests are off unless you ask:

```bash
cmake -B build -DBUILD_TESTING=ON -DPOSIXIPC_DEVELOPER_MODE=ON
cmake --build build
ctest --test-dir build
```

`POSIXIPC_DEVELOPER_MODE` turns on `-Werror`. Formatters: Ruff for
Python, `.clang-format` for C (`src/`, `include/`, `tests_c/`).

`POSIXIPC_SANITIZER` = `address` | `thread` | `undefined`.
`POSIXIPC_CACHELINE_BYTES` defaults to 64; changing it changes `abi_tag`
and the digest.

---

## Testing

`tests_c/` has no Python dependency (usable under TSan). TSan is
single-process; `fork()` TSan runs do not prove cross-process races.
Those need ASan/UBSan plus the stress loops.

Robust tests `SIGKILL` a child after it publishes a hold flag. Owner
death is not simulated with `release()`. Every test has a timeout.

```bash
pytest tests/
```

---

## License

MIT. See [`LICENSE`](LICENSE).
