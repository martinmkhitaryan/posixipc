from posixipc._posixipc import EventFD as EventFD
from posixipc._posixipc import Futex as Futex
from posixipc._posixipc import FutexQueue as FutexQueue
from posixipc._posixipc import MemFD as MemFD
from posixipc.linux.features import Features

features: Features

def memfd(size: int, name: str = "posixipc") -> MemFD: ...
