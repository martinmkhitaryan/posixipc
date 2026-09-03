# Design notes

This expands the README contract. The public Python API is still defined in
`README.md`. For a client map of types and usage see [`guide.md`](guide.md).
This file is the longer form of the recovery and trust rules.

## Owner death

On `EOWNERDEAD` the waiter **holds** the robust mutex. The library calls
`on_owner_died(mutex)` while that is true, then `pthread_mutex_consistent()`.
If the callback raises, the library unlocks (poisoning the mutex) and
propagates the exception.

`on_owner_died` repairs **application** state in the segment, not the mutex
object. A crashed holder does not run Python `__del__` or `close()` on the
other side of a `SIGKILL`.

On `ENOTRECOVERABLE` you do **not** hold the lock. That is
`posixipc.NotRecoverableError`.

## Writable segment is a trust boundary

Anyone who can `mmap` the object writable can:

- forge the layout digest
- overwrite `pthread_mutex_t` storage
- make `on_owner_died` see whatever bytes they planted

`attach()` checks ownership (`st_uid == geteuid()`) and mode (`077` must be
clear). That is not a confidentiality mechanism. Use an unpredictable name or
a supervisor that creates the object first.

The capsule API yields a `pthread_mutex_t *` into that same mapping. A C
extension that locks through a capsule is trusting the same segment.

## Capsule pins

`Mutex.as_capsule()` (and `MutexArray.as_capsule(i)`) returns a versioned
`PyCapsule` named `posixipc.mutex.v1`. Creating it increments the region's pin
count. Destroying it decrements. `SharedMemory.close()` raises `BufferError`
while any capsule pin remains.

Other extensions include the installed `posixipc.h` and call
`posixipc_mutex_from_capsule`. `retain` / `release` on the capsule context
add extra pins that must be balanced while the capsule object is alive.

## Queues

Three types, three names ([`queue.md`](queue.md)): `posixipc.Queue` is
the Layout ring; `NamedMessageQueue` is `mq_*`; `posixipc.linux.FutexQueue`
is a Linux ring that must not attach a `Queue` segment.

## MutexArray

`layout.add_array(posixipc.Mutex, N)` is N directory slots with the same
encoding as N calls to `add(posixipc.Mutex)`. The digest matches. The Python
object is one handle and one pin. Index with `arr[i]` or `arr.acquire(i)`.
