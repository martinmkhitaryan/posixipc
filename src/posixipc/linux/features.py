from posixipc._posixipc import __build_info__


class Features:
    __slots__ = ("futex", "eventfd", "memfd", "futex_queue")

    def __init__(self, info: dict) -> None:
        # __setattr__ rejects writes; object.__setattr__ still fills the slots.
        object.__setattr__(self, "futex", bool(info.get("have_futex", False)))
        object.__setattr__(self, "eventfd", bool(info.get("have_eventfd", False)))
        object.__setattr__(self, "memfd", bool(info.get("have_memfd", False)))
        object.__setattr__(
            self,
            "futex_queue",
            bool(info.get("have_futex", False)) and bool(info.get("have_robust_mutex", False)),
        )

    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError("features is immutable")


features = Features(__build_info__)
