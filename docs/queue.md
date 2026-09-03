# Queues

Three types, three names. None is an alias of another. Do not hide
`mq_*` or futex behind `posixipc.Queue`.

| Type | Backing | Namespace |
| --- | --- | --- |
| `posixipc.Queue` | Shared-memory ring, pthread mutex + cond | portable `posixipc` |
| `posixipc.NamedMessageQueue` | `mq_open` / `mq_send` / `mq_receive` | portable `posixipc`, absent if undetected |
| `posixipc.linux.FutexQueue` | Shared-memory ring, futex wait | `posixipc.linux` only |

A ring larger than 4 GiB − 1 is multiple segments, not a wider
`uint32_t total_size`.

They are not `multiprocessing.Queue` (pipe + feeder thread + pickle)
and not `queue.Queue`. `put` / `get` / timeout are the familiarity
target, not the storage model.

---

## `posixipc.Queue`

Bounded circular buffer of fixed-size **byte** slots in a typed
`Layout` segment. Producers reserve, write, publish. Consumers claim,
copy out, release.

Why this is the default `Queue`:

- Same create / attach / digest path as the mutexes.
- Crash recovery is mechanical (see below).
- Depth and `item_size` are layout constants; a mismatch refuses attach.
- glibc and musl; no `mq_*`, no futex.

Cost: bytes only (pickle is the caller’s problem); bounded; first cut
holds the lock across the memcpy.

### Layout expansion

`Queue` is a **factory** that appends existing kinds. Digest stays a
function of the slot sequence. `layout.add(Queue, …)` expands, in this
order:

1. Two `KIND_ROBUST_MUTEX` slots. The critical section is mutex 0
   (put lock). Mutex 1 is in the layout so the digest stays distinct
   from a single-mutex ring; put and get do not take it.
2. `Condition` whose directory names mutex 0 — not full.
3. `Condition` whose directory names mutex 1 — not empty.
   Both `pthread_cond_wait` calls pass the put lock. Mutex 1 is never
   the wait mutex.
4. `add_bytes` — control word + per-slot state + payload.

Control word (start of the bytes slot, cache-line aligned):

```text
uint32_t head        /* next slot to get; modulo depth */
uint32_t tail        /* next slot to put; modulo depth */
uint32_t count       /* occupied slots; 0..depth */
uint32_t flags
uint32_t depth
uint32_t item_size
```

Then `depth` slot states (`EMPTY` / `RESERVED_PUT` / `READY` /
`RESERVED_GET`) and `depth * item_size` payload bytes, each item on a
cache line.

`count` must match the headers after recovery.

### Wait

`posixipc_blocking_wait` slices, same as `Condition`. `put` waits
not-full; `get` waits not-empty. No spinning. No futex.

### Recovery

The library supplies `on_owner_died`. The user must not pass it to
`add(Queue)` (`TypeError`).

Hold the mutex across the copy.

On owner death:

- `RESERVED_PUT` → `EMPTY` (drop the partial write).
- `RESERVED_GET` → `READY` (item stays).
- Recompute `count` from `READY` and `RESERVED_GET`.

A crashed waiter that did not hold a lock is a no-op.

### API

```python
q = layout.add(posixipc.Queue, depth=32, item_size=256)

q.put(data, timeout=None, *, blocking=True, interruptible=True) -> bool
q.get(timeout=None, *, blocking=True, interruptible=True) -> bytes | None
q.put_nowait(data)
q.get_nowait()
q.qsize() -> int
q.close()
```

- `data` is `bytes` | `bytearray` | `memoryview`, length exactly
  `item_size`.
- `put` returns `False` on timeout; `get` returns `None` on timeout.
- No pickle, `task_done`, `join`, unbounded mode, `empty()`, or
  `full()`.
- No process-private `Queue()`.
- Pickle of a bound handle is `(name, slot, depth, item_size, digest)`.
  Recover is library-supplied on `EOWNERDEAD`; it is not a user
  `on_owner_died` restored from the pickle.
- `features.queue` is always `True`. It is not a build probe.

`Queue` must not attach a `FutexQueue` segment. The slot sequence
differs (condvars vs futex words); digests will not match.

---

## `posixipc.NamedMessageQueue`

POSIX message queue. Same naming and lifetime split as
`NamedSemaphore`: `create` / `attach` / `unlink`. Not a `Layout`
slot. No digest, no capsule, no `on_owner_died`.

```python
mq = posixipc.NamedMessageQueue.create("/myapp.jobs", maxmsg=8, msgsize=256)
mq = posixipc.NamedMessageQueue.attach("/myapp.jobs")
mq = posixipc.NamedMessageQueue.open_or_create("/myapp.jobs", maxmsg=8, msgsize=256)
mq.put(data, timeout=None, *, blocking=True, interruptible=True, priority=0) -> bool
mq.get(timeout=None, *, blocking=True, interruptible=True) -> bytes | None
mq.request_notification(None | signo | callback | (callback, arg))
mq.close()
mq.unlink()
NamedMessageQueue.unlink_name(name)
```

Rules:

- `data` length is `1..msgsize`. The kernel may zero-pad; `get`
  returns the sent length (not `msgsize`) if the implementation can
  see it.
- `priority` is the POSIX message priority (`mq_send`).
- A crashed sender that had not completed `mq_send` leaves the queue
  unchanged. A crashed receiver that already returned from
  `mq_receive` has taken the message; the library cannot put it back.
- Name rules follow the platform. glibc prefixes `mq.`; document the
  component limit like `NamedSemaphore`.
- Absent from the namespace if `mq_open` was not detected;
  `features.named_message_queue` (or `features.mq`) is `False`.
- Not in a `Layout`. `layout.add(NamedMessageQueue)` is `TypeError`.
- `open_or_create` is `mq_open(O_CREAT)` without `O_EXCL`. If the name
  already exists, `maxmsg` / `msgsize` are ignored.
- `request_notification` is one-shot `mq_notify`. `None` cancels. An
  `int` is `SIGEV_SIGNAL`. A callable or `(callable, arg)` is
  `SIGEV_THREAD`: the callback runs on the notify pthread (GIL
  acquired there). Exceptions are written unraisable, not propagated
  to the caller of `request_notification`. Only one registrant at a
  time; re-arm after delivery.

Do not call this `Queue` or `NamedQueue`.

---

## `posixipc.linux.FutexQueue`

Layout-backed ring whose **wait** is `FUTEX_WAIT` / `FUTEX_WAKE`, not
`pthread_cond`. Linux only. Not in `posixipc.__all__`. Importing
`posixipc.linux` off Linux fails; the name is absent.

This is a **different type** and a **different slot sequence** from
`Queue`. A process using `Queue` must not attach a `FutexQueue`
segment (and the reverse). Do not share a digest by omitting
condvars and hoping.

Frozen expansion of `layout.add(FutexQueue, …)` (only valid when
`posixipc.linux` is imported and the kind is that type):

1. Two `KIND_ROBUST_MUTEX` slots (put lock, get lock). The critical
   section uses the put lock, same as `Queue`. Library-supplied
   recover. `on_owner_died=` on `add(FutexQueue)` is `TypeError`.
2. One `KIND_BYTES` slot: cacheline-aligned `not_full` / `not_empty`
   futex words, then the same head/tail/count/slot-state machine as
   `Queue`. Wait is `FUTEX_WAIT` / `FUTEX_WAKE` on those words.
   No `Condition` slots.

That is three slots (`RM`, `RM`, `BYTES`) versus `Queue`'s five
(`RM`, `RM`, `COND`, `COND`, `BYTES`). Digests cannot match.

Recovery uses the same `RESERVED_*` rules as `Queue`. A crashed
holder of the robust mutex is `EOWNERDEAD`. A crashed futex waiter
is not; it has already dropped the mutex. `Queue` stays the portable
default because it does not need Linux futex.

API surface matches `Queue` (`put` / `get` / `qsize` / `close`) so
callers can switch types at `add()` time, not method names.

`features` on `posixipc.linux` reports presence. No leak into
`posixipc.features` beyond “linux extra is importable.”

`posixipc.linux.Futex` is a process-private word (`wait` / `wake` /
`value`). `EventFD` wraps `eventfd(2)`. `memfd(size)` is
`MemFD.create`: an anonymous mapping whose `__reduce__` uses
`multiprocessing.reduction.DupFd`, not the named-shm pickle path.

---

## What none of them do

- Pickle inside `put` / `get`.
- Unbounded / disk-backed overflow.
- Widening `total_size` past `uint32_t`.
- One class that `dlsym`s futex and falls back to `mq_send`.

---

## Tests

Hard timeout on every test.

`Queue`: in-process full/empty/timeout/wrong `item_size`; two
processes `spawn`; `SIGKILL` during `RESERVED_PUT` (partial never
appears) and `RESERVED_GET` (item still gettable); pin/`close`;
sanitizers; fuzzed control words never hang.

`NamedMessageQueue`: persist after close (name still attachable);
close vs unlink; priority order if the OS honors it; crashed holder
does not return a kernel count (document actual POSIX behavior);
absence when `features.mq` is false.

`FutexQueue`: same functional tests as `Queue`, plus “does not import
off Linux” and “cannot attach a `Queue` digest.”
