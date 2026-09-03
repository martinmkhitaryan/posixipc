from types import MappingProxyType

from posixipc._posixipc import __build_info__, _monotonic_timeouts


class Features:
    __slots__ = (
        "robust_mutex",
        "prio_inherit",
        "process_shared",
        "barrier",
        "spinlock",
        "named_semaphore",
        "cond_monotonic",
        "monotonic_timeouts",
        "memfd",
        "mq",
        "queue",
        "named_message_queue",
    )

    def __init__(self, info: dict) -> None:
        # __setattr__ rejects writes; object.__setattr__ still fills the slots.
        object.__setattr__(self, "robust_mutex", bool(info["have_robust_mutex"]))
        object.__setattr__(self, "prio_inherit", bool(info["have_prio_inherit"]))
        object.__setattr__(self, "process_shared", bool(info["have_process_shared"]))
        object.__setattr__(self, "barrier", bool(info["have_barrier"]))
        object.__setattr__(self, "spinlock", bool(info["have_spinlock"]))
        object.__setattr__(self, "named_semaphore", bool(info["have_named_semaphore"]))
        object.__setattr__(self, "cond_monotonic", bool(info["have_cond_monotonic"]))
        object.__setattr__(self, "memfd", bool(info["have_memfd"]))
        object.__setattr__(self, "mq", bool(info["have_mq"]))
        object.__setattr__(self, "queue", True)  # always shipped; not a build probe
        object.__setattr__(self, "named_message_queue", bool(info["have_mq"]))  # same bit as mq
        object.__setattr__(
            self,
            "monotonic_timeouts",
            MappingProxyType(dict(_monotonic_timeouts)),
        )

    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError("features is immutable")


features = Features(__build_info__)
