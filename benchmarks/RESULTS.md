# Uncontended mutex benchmark

Measured with `taskset -c 0 python benchmarks/mutex_uncontended.py`
(`timeit`, 200000 loops). Governor was `powersave`; treat numbers as a
committed local record, not a cross-machine claim.

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
| `threading.Lock.acquire`+`release` | 79.8 |
| `posixipc.Mutex.acquire`+`release` | 58.6 |
| `_empty_call` | 14.3 |

Ratio vs `threading.Lock`: **0.74×** (faster).
