# Changelog

## 1.0.0 — 2026-09-04

First release. POSIX IPC and synchronization for CPython 3.12+ on Linux,
as a C extension over `pthread_*`, `sem_*`, and POSIX shared memory.

This release was written in Cursor and reviewed by Cursor multi-agent
runs. It still needs a human review.

### Added

**Layout and memory**

- `Layout`, `SharedMemory`, `SharedBytes`
- `features`, `__build_info__`
- Capsule API in `posixipc.h` (`posixipc.mutex.v1`)

**Always present**

- `Mutex`, `RobustMutex`, `RWLock`, `Condition`, `Semaphore`
- `MutexArray`, `MutexArrayItem`
- `Queue`

**When detected**

- `Barrier`, `SpinLock` (`posixipc.spinlock`)
- `NamedSemaphore`, `NamedMessageQueue`

**`posixipc.linux`**

- `Futex`, `EventFD`, `MemFD`, `memfd`, `FutexQueue`
- `linux.features` (`futex`, `eventfd`, `memfd`, `futex_queue`)

**Errors**

- `PosixIPCError`, `NotRecoverableError`, `ClosedError`,
  `LayoutMismatchError`
- `TimeoutError` (builtin)

**Docs and tests**

- `README`, `docs/guide.md`, `docs/queue.md`, `docs/design.md`, examples
- C tests and pytest, including spawn / `SIGKILL` stress

Windows is out of scope for this backend.
