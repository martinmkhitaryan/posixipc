#!/usr/bin/env python3
import multiprocessing
import os
import platform
import sys
import sysconfig
import threading
import timeit

import posixipc


def _cpu_model():
    path = "/proc/cpuinfo"
    try:
        for line in open(path, encoding="utf-8"):
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        return platform.processor() or "unknown"
    return platform.processor() or "unknown"


def _governor():
    path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    try:
        return open(path, encoding="utf-8").read().strip()
    except OSError:
        return "unknown"


def _gil_status():
    enabled = getattr(sys, "_is_gil_enabled", None)
    if enabled is None:
        return "gil"
    return "gil" if enabled() else "free-threaded"


def _median(values):
    s = sorted(values)
    mid = len(s) // 2
    if len(s) % 2:
        return s[mid]
    return (s[mid - 1] + s[mid]) / 2


def _ns_per_call(fn, n, repeats=7, warmup=50_000):
    timeit.timeit(fn, number=warmup)
    samples = [timeit.timeit(fn, number=n) / n * 1e9 for _ in range(repeats)]
    return _median(samples), samples


def main():
    n = 200_000
    lock = threading.Lock()
    mutex = posixipc.Mutex()
    mp_lock = multiprocessing.Lock()
    empty = posixipc._posixipc._empty_call  # pyright: ignore[reportAttributeAccessIssue]

    def lock_cycle():
        lock.acquire()
        lock.release()

    def posix_cycle():
        mutex.acquire()
        mutex.release()

    def mp_cycle():
        mp_lock.acquire()
        mp_lock.release()

    lock_ns, lock_samples = _ns_per_call(lock_cycle, n)
    posix_ns, posix_samples = _ns_per_call(posix_cycle, n)
    mp_ns, mp_samples = _ns_per_call(mp_cycle, n)
    empty_ns, empty_samples = _ns_per_call(empty, n)
    mutex.close()
    print(f"threading.Lock acquire+release: {lock_ns:.1f} ns  (samples {[round(x, 1) for x in lock_samples]})")
    print(f"posixipc.Mutex acquire+release: {posix_ns:.1f} ns  (samples {[round(x, 1) for x in posix_samples]})")
    print(f"multiprocessing.Lock acquire+release: {mp_ns:.1f} ns  (samples {[round(x, 1) for x in mp_samples]})")
    print(f"_empty_call: {empty_ns:.1f} ns  (samples {[round(x, 1) for x in empty_samples]})")
    print(f"ratio vs threading.Lock: {posix_ns / lock_ns:.2f}x")
    print(f"ratio vs multiprocessing.Lock: {posix_ns / mp_ns:.2f}x")
    print(f"python: {platform.python_version()} ({_gil_status()})")
    print(f"kernel: {platform.release()}")
    print(f"libc: {posixipc.__build_info__.get('libc')}")
    print(f"cpu: {_cpu_model()}")
    print(f"governor: {_governor()}")
    print(f"affinity: {os.sched_getaffinity(0)}")
    print(f"platform: {sysconfig.get_platform()}")


if __name__ == "__main__":
    main()
