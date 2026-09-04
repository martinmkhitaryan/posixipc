# Uncontended mutex benchmark

Measured with `taskset -c 0 python benchmarks/mutex_uncontended.py`
(`timeit`, 200000 loops, median of 7 after warmup). Governor was
`powersave`; treat numbers as a committed local record, not a
cross-machine claim.

All three locks are acquire+release in **one process**.
`multiprocessing.Lock` is still a SemLock (the same object you would
share across processes); this is not a two-process run.

| Field | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 155H |
| Kernel | Linux 7.0.0-30-generic |
| libc | glibc (`sizeof(pthread_mutex_t)` = 40) |
| Python | CPython 3.12.3 (GIL build) |
| Governor | powersave |
| Affinity | CPU 0 (`taskset -c 0`) |

| Operation | ns / call |
| --- | --- |
| `threading.Lock.acquire`+`release` | 119.7 |
| `posixipc.Mutex.acquire`+`release` | 82.7 |
| `multiprocessing.Lock.acquire`+`release` | 80.9 |
| `_empty_call` | 16.4 |

Ratio vs `threading.Lock`: **0.69×** (faster).
Ratio vs `multiprocessing.Lock`: **1.02×** (same).

`threading.Lock` is slower here because it is not a `pthread_mutex`.
CPython 3.12 implements it as a POSIX semaphore (`sem_trywait` /
`sem_post`) behind `_thread.lock`, with METH_VARARGS parsing and a
timed-acquire helper that still builds a timeout on the non-blocking
try. `posixipc.Mutex` is `pthread_mutex_trylock` then
`pthread_mutex_unlock`, GIL held on that uncontended path.

`taskset -c 0` pins the process to one core so the run does not migrate
(cache / frequency noise). It does not make one lock faster than
another.

The governor is Linux CPU frequency scaling. `powersave` keeps clocks
lower and more variable than `performance`. Absolute ns move with it;
the ranking does not.
