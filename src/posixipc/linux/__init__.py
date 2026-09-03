import sys

if sys.platform != "linux":
    raise ImportError("posixipc.linux is only available on Linux")

from posixipc._posixipc import EventFD, Futex, FutexQueue, MemFD
from posixipc.linux.features import features

memfd = MemFD.create

__all__ = [
    "EventFD",
    "Futex",
    "FutexQueue",
    "MemFD",
    "features",
    "memfd",
]
