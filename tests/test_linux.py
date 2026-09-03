import importlib
import sys

import pytest

import posixipc


def test_linux_names_absent_from_portable():
    assert "Futex" not in posixipc.__all__
    assert "EventFD" not in posixipc.__all__
    assert "MemFD" not in posixipc.__all__
    assert "FutexQueue" not in posixipc.__all__
    assert "memfd" not in posixipc.__all__
    assert not hasattr(posixipc, "Futex")
    assert not hasattr(posixipc, "EventFD")
    assert not hasattr(posixipc, "MemFD")
    assert not hasattr(posixipc, "FutexQueue")
    assert not hasattr(posixipc, "memfd")


def test_linux_import_on_this_platform():
    if sys.platform != "linux":
        with pytest.raises(ImportError):
            importlib.import_module("posixipc.linux")
        return
    linux = importlib.import_module("posixipc.linux")
    assert linux.Futex is not None
    assert linux.EventFD is not None
    assert linux.MemFD is not None
    assert linux.FutexQueue is not None
    assert linux.features.futex is True
    assert linux.features.futex_queue is True


def test_linux_rejects_non_linux(monkeypatch):
    monkeypatch.setattr(sys, "platform", "darwin")
    sys.modules.pop("posixipc.linux", None)
    sys.modules.pop("posixipc.linux.features", None)
    with pytest.raises(ImportError, match="only available on Linux"):
        importlib.import_module("posixipc.linux")
