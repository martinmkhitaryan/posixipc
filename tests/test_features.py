import pytest

import posixipc


def test_monotonic_timeouts_shape():
    mt = posixipc.features.monotonic_timeouts
    assert set(mt) == {"mutex", "rwlock", "semaphore", "condition"}
    assert all(isinstance(v, bool) for v in mt.values())


def test_robust_mutex_present_on_linux_glibc():
    info = posixipc.__build_info__
    if info.get("libc") == "glibc":
        assert posixipc.features.robust_mutex is True


def test_features_are_immutable():
    try:
        posixipc.features.robust_mutex = False
    except AttributeError:
        return
    raise AssertionError("features should be immutable")


def test_features_flags_are_bool():
    for name in (
        "robust_mutex",
        "prio_inherit",
        "process_shared",
        "barrier",
        "spinlock",
        "named_semaphore",
        "cond_monotonic",
        "memfd",
        "mq",
        "queue",
        "named_message_queue",
    ):
        assert isinstance(getattr(posixipc.features, name), bool)


def test_barrier_namespace_matches_feature():
    if posixipc.features.barrier:
        assert hasattr(posixipc, "Barrier")
        from posixipc import Barrier

        assert Barrier is posixipc.Barrier
    else:
        assert not hasattr(posixipc, "Barrier")


def test_spinlock_is_opt_in():
    import importlib

    assert "SpinLock" not in posixipc.__all__
    assert not hasattr(posixipc, "SpinLock")
    if posixipc.features.spinlock:
        spin = importlib.import_module("posixipc.spinlock")
        assert spin.SpinLock is not None
    else:
        with pytest.raises(ImportError):
            importlib.import_module("posixipc.spinlock")


def test_named_semaphore_namespace_matches_feature():
    if posixipc.features.named_semaphore:
        assert hasattr(posixipc, "NamedSemaphore")
    else:
        assert not hasattr(posixipc, "NamedSemaphore")


def test_monotonic_timeouts_are_runtime_not_build_info():
    info = posixipc.__build_info__
    runtime = dict(posixipc._posixipc._monotonic_timeouts)
    assert posixipc.features.monotonic_timeouts == runtime
    assert "have_mutex_clocklock" in info
    assert "have_rwlock_clocklock" in info
    assert "have_sem_clockwait" in info
    assert runtime["condition"] == bool(info["have_cond_monotonic"])


def test_every_feature_flag_is_present():
    names = (
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
    for name in names:
        assert hasattr(posixipc.features, name)


def test_queue_is_always_present():
    assert posixipc.features.queue is True
    assert hasattr(posixipc, "Queue")


def test_linux_extra_is_opt_in():
    import importlib
    import sys

    assert "FutexQueue" not in posixipc.__all__
    assert not hasattr(posixipc, "FutexQueue")
    if sys.platform == "linux":
        linux = importlib.import_module("posixipc.linux")
        assert linux.FutexQueue is not None
        assert hasattr(linux.features, "futex")
        assert hasattr(linux.features, "eventfd")
        assert hasattr(linux.features, "memfd")
        assert hasattr(linux.features, "futex_queue")
    else:
        with pytest.raises(ImportError):
            importlib.import_module("posixipc.linux")


def test_named_message_queue_namespace_matches_feature():
    if posixipc.features.named_message_queue:
        assert hasattr(posixipc, "NamedMessageQueue")
        assert posixipc.features.mq is True
    else:
        assert not hasattr(posixipc, "NamedMessageQueue")
        assert posixipc.features.mq is False
