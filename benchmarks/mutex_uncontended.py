#!/usr/bin/env python3
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


def main():
    n = 200_000
    lock = threading.Lock()

    def lock_cycle():
        lock.acquire()
        lock.release()

    mutex = posixipc.Mutex()

    def posix_cycle():
        mutex.acquire()
        mutex.release()

    empty = posixipc._posixipc._empty_call
    lock_ns = timeit.timeit(lock_cycle, number=n) / n * 1e9
    posix_ns = timeit.timeit(posix_cycle, number=n) / n * 1e9
    empty_ns = timeit.timeit(empty, number=n) / n * 1e9
    mutex.close()
    print(f"threading.Lock acquire+release: {lock_ns:.1f} ns")
    print(f"posixipc.Mutex acquire+release: {posix_ns:.1f} ns")
    print(f"_empty_call: {empty_ns:.1f} ns")
    print(f"ratio vs Lock: {posix_ns / lock_ns:.2f}x")
    print(f"python: {platform.python_version()} ({_gil_status()})")
    print(f"kernel: {platform.release()}")
    print(f"libc: {posixipc.__build_info__.get('libc')}")
    print(f"cpu: {_cpu_model()}")
    print(f"governor: {_governor()}")
    print(f"affinity: {os.sched_getaffinity(0)}")
    print(f"platform: {sysconfig.get_platform()}")


if __name__ == "__main__":
    main()
