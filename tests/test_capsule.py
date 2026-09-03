import ctypes
from ctypes import c_char_p, c_void_p, py_object
from pathlib import Path

import pytest

import posixipc


def _capsule_name(capsule):
    pythonapi = ctypes.pythonapi
    pythonapi.PyCapsule_GetName.restype = c_char_p
    pythonapi.PyCapsule_GetName.argtypes = [py_object]
    return pythonapi.PyCapsule_GetName(capsule)


def _capsule_pointer(capsule, name):
    pythonapi = ctypes.pythonapi
    pythonapi.PyCapsule_GetPointer.restype = c_void_p
    pythonapi.PyCapsule_GetPointer.argtypes = [py_object, c_char_p]
    return pythonapi.PyCapsule_GetPointer(capsule, name)


def test_as_capsule_pins_region(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    capsule = mutex.as_capsule()
    assert _capsule_name(capsule) == posixipc._posixipc.MUTEX_CAPSULE_NAME.encode()
    assert _capsule_pointer(capsule, posixipc._posixipc.MUTEX_CAPSULE_NAME.encode())
    with pytest.raises(BufferError):
        region.close()
    del capsule
    mutex.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_capsule_release_allows_close(shm_name):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    region = layout.create(shm_name)
    capsule = mutex.as_capsule()
    mutex.close()
    with pytest.raises(BufferError):
        region.close()
    del capsule
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)


def test_private_mutex_as_capsule_rejected():
    mutex = posixipc.Mutex()
    try:
        with pytest.raises(TypeError):
            mutex.as_capsule()
    finally:
        mutex.close()


def test_unbound_mutex_as_capsule_rejected():
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    with pytest.raises(RuntimeError):
        mutex.as_capsule()


def test_public_header_documents_capsule():
    pkg = Path(posixipc.__file__).resolve().parent
    header = pkg / "posixipc.h"
    if not header.is_file():
        header = Path(__file__).resolve().parents[1] / "include" / "posixipc.h"
    text = header.read_text(encoding="utf-8")
    assert "posixipc_mutex_from_capsule" in text
    assert "posixipc_atomic_u32" in text
    assert "posixipc_shm_header" in text
    assert "posixipc_slot" in text
    assert "#ifdef __cplusplus" in text
