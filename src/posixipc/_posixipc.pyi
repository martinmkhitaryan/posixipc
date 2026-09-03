from collections.abc import Callable
from typing import Any, Self

__build_info__: dict[str, Any]
_monotonic_timeouts: dict[str, bool]
LAYOUT_VERSION: int
CACHELINE_BYTES: int
KIND_BYTES: int
KIND_MUTEX: int
KIND_ROBUST_MUTEX: int
KIND_RWLOCK: int
KIND_COND: int
KIND_SEM: int
KIND_BARRIER: int
KIND_SPIN: int
abi_tag: int
MUTEX_CAPSULE_NAME: str

def _empty_call() -> None: ...
def _attach_unchecked(name: str, timeout: float | None = ...) -> SharedMemory: ...
def _attach_slot(
    name: str, slot: int, kind: int, digest: int
) -> Mutex | SharedBytes | RWLock | Condition | Semaphore: ...
def _attach_array(name: str, slot: int, count: int, kind: int, digest: int) -> MutexArray: ...
def _attach_queue(name: str, slot: int, depth: int, item_size: int, digest: int) -> Queue: ...
def _attach_futex_queue(name: str, slot: int, depth: int, item_size: int, digest: int) -> FutexQueue: ...
def _rebuild_eventfd(dfd: Any) -> EventFD: ...
def _rebuild_memfd(dfd: Any, size: int, name: str) -> MemFD: ...
def _layout_digest(
    layout_version: int,
    abi_tag: int,
    slots: list[tuple[int, int, int, int]],
) -> int: ...

class SharedMemory:
    name: str
    size: int
    digest: int
    closed: bool
    payload: memoryview

    @classmethod
    def create(cls, name: str, size: int) -> Self: ...
    @classmethod
    def attach_unchecked(cls, name: str, timeout: float | None = ...) -> Self: ...
    @classmethod
    def open_or_create(cls, name: str, size: int, timeout: float | None = ...) -> Self: ...
    @classmethod
    def unlink_name(cls, name: str) -> None: ...
    def close(self) -> None: ...
    def unlink(self) -> None: ...

class SharedBytes:
    region: SharedMemory | None
    slot: int
    kind: int
    offset: int
    size: int
    closed: bool
    def close(self) -> None: ...
    def __buffer__(self, flags: int) -> memoryview: ...

class Mutex:
    on_owner_died: Callable[[Mutex], Any] | None
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(
        self,
        *,
        process_shared: bool = False,
        prio_inherit: bool = False,
        on_owner_died: Callable[[Mutex], Any] | None = None,
    ) -> None: ...
    def acquire(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self) -> None: ...
    def close(self) -> None: ...
    def as_capsule(self) -> object: ...
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class MutexArrayItem:
    index: int
    array: MutexArray | None
    def acquire(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self) -> None: ...
    def as_capsule(self) -> object: ...
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class MutexArray:
    on_owner_died: Callable[[MutexArrayItem], Any] | None
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    def acquire(
        self,
        index: int,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self, index: int) -> None: ...
    def close(self) -> None: ...
    def as_capsule(self, index: int) -> object: ...
    def __len__(self) -> int: ...
    def __getitem__(self, index: int) -> MutexArrayItem: ...

class RobustMutex(Mutex):
    on_owner_died: Callable[[RobustMutex], Any]
    def __init__(
        self,
        *,
        process_shared: bool = False,
        prio_inherit: bool = False,
        on_owner_died: Callable[[RobustMutex], Any],
    ) -> None: ...

class RWLock:
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(self, *, process_shared: bool = False) -> None: ...
    def acquire_read(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def acquire_write(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self) -> None: ...
    def read(self) -> Any: ...
    def write(self) -> Any: ...
    def close(self) -> None: ...

class Condition:
    mutex: Mutex | None
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(self, mutex: Mutex, *, process_shared: bool = False) -> None: ...
    def wait(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def notify(self) -> None: ...
    def notify_all(self) -> None: ...
    def close(self) -> None: ...

class Semaphore:
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(self, value: int = 1, *, process_shared: bool = False) -> None: ...
    def acquire(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self, n: int = 1) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Barrier:
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(self, parties: int, *, process_shared: bool = False) -> None: ...
    def wait(self) -> bool: ...
    def close(self) -> None: ...

class SpinLock:
    process_shared: bool
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    kind: int
    digest: int | None
    offset: int
    size: int
    def __init__(self, *, process_shared: bool = False) -> None: ...
    def acquire(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self) -> None: ...
    def close(self) -> None: ...
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Queue:
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    depth: int
    item_size: int
    def put(
        self,
        data: bytes | bytearray | memoryview,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def get(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bytes | None: ...
    def put_nowait(self, data: bytes | bytearray | memoryview) -> bool: ...
    def get_nowait(self) -> bytes | None: ...
    def qsize(self) -> int: ...
    def close(self) -> None: ...

class Futex:
    value: int
    closed: bool
    def __init__(self) -> None: ...
    def wait(
        self,
        expected: int,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def wake(self, n: int = 1) -> None: ...
    def close(self) -> None: ...

class EventFD:
    closed: bool
    def __init__(
        self,
        initval: int = 0,
        *,
        semaphore: bool = False,
        nonblock: bool = False,
    ) -> None: ...
    def write(self, n: int = 1) -> None: ...
    def read(self) -> int: ...
    def fileno(self) -> int: ...
    def close(self) -> None: ...

class MemFD:
    name: str | None
    size: int
    closed: bool
    payload: memoryview
    def __init__(self) -> None: ...
    @classmethod
    def create(cls, size: int, name: str = "posixipc") -> Self: ...
    def fileno(self) -> int: ...
    def close(self) -> None: ...
    def __buffer__(self, flags: int) -> memoryview: ...

class FutexQueue:
    bound: bool
    closed: bool
    region: SharedMemory | None
    slot: int
    depth: int
    item_size: int
    def put(
        self,
        data: bytes | bytearray | memoryview,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def get(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bytes | None: ...
    def put_nowait(self, data: bytes | bytearray | memoryview) -> bool: ...
    def get_nowait(self) -> bytes | None: ...
    def qsize(self) -> int: ...
    def close(self) -> None: ...

class NamedMessageQueue:
    name: str | None
    closed: bool
    def __init__(self) -> None: ...
    @classmethod
    def create(cls, name: str, maxmsg: int = 8, msgsize: int = 256) -> Self: ...
    @classmethod
    def attach(cls, name: str) -> Self: ...
    @classmethod
    def open_or_create(cls, name: str, maxmsg: int = 8, msgsize: int = 256) -> Self: ...
    @classmethod
    def unlink_name(cls, name: str) -> None: ...
    def put(
        self,
        data: bytes | bytearray | memoryview,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
        priority: int = 0,
    ) -> bool: ...
    def get(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bytes | None: ...
    def request_notification(
        self,
        notification: int | Callable[..., Any] | tuple[Callable[..., Any], Any] | None = None,
    ) -> None: ...
    def close(self) -> None: ...
    def unlink(self) -> None: ...

class NamedSemaphore:
    name: str | None
    closed: bool
    def __init__(self) -> None: ...
    @classmethod
    def create(cls, name: str, value: int = 1) -> Self: ...
    @classmethod
    def attach(cls, name: str) -> Self: ...
    @classmethod
    def unlink_name(cls, name: str) -> None: ...
    def acquire(
        self,
        timeout: float | None = None,
        *,
        blocking: bool = True,
        interruptible: bool = True,
    ) -> bool: ...
    def release(self, n: int = 1) -> None: ...
    def close(self) -> None: ...
    def unlink(self) -> None: ...
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Layout:
    digest: int
    slots: list[tuple[int, int, int, int, int]]
    def add(
        self,
        kind: type[Mutex]
        | type[RWLock]
        | type[Condition]
        | type[Semaphore]
        | type[Barrier]
        | type[SpinLock]
        | type[Queue]
        | type[FutexQueue],
        *,
        on_owner_died: Callable[[RobustMutex], Any] | None = None,
        prio_inherit: bool = False,
        process_shared: bool = True,
        mutex: Mutex | None = None,
        value: int = 1,
        parties: int = ...,
        depth: int = ...,
        item_size: int = ...,
    ) -> Mutex | RWLock | Condition | Semaphore | Barrier | SpinLock | Queue | FutexQueue: ...
    def add_array(
        self,
        kind: type[Mutex],
        count: int,
        *,
        on_owner_died: Callable[[MutexArrayItem], Any] | None = None,
        prio_inherit: bool = False,
        process_shared: bool = True,
    ) -> MutexArray: ...
    def add_bytes(self, size: int) -> SharedBytes: ...
    def create(self, name: str) -> SharedMemory: ...
    def attach(self, name: str, timeout: float | None = ...) -> SharedMemory: ...
    def open_or_create(self, name: str, timeout: float | None = ...) -> SharedMemory: ...
