import os
import subprocess
import sys

import posixipc
from tests.helpers import procs


def _fnv1a_32(data: bytes) -> int:
    h = 0x811C9DC5
    for byte in data:
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return 1 if h == 0 else h


def _le16(value: int) -> bytes:
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def _le32(value: int) -> bytes:
    return bytes(
        (
            value & 0xFF,
            (value >> 8) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 24) & 0xFF,
        )
    )


def python_layout_digest(layout_version, abi_tag, slots):
    buf = bytearray()
    buf += _le16(layout_version)
    buf += _le32(abi_tag)
    buf += _le32(posixipc._posixipc.CACHELINE_BYTES)
    for kind, align, size, init_flags in slots:
        buf += _le16(kind)
        buf += _le16(align)
        buf += _le32(size)
        buf += _le32(init_flags)
    return _fnv1a_32(bytes(buf))


def test_offsets_aligned_after_directory():
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    blob = layout.add_bytes(8)
    slots = layout.slots
    assert slots[0][2] >= 64
    assert mutex.offset == slots[0][2]
    assert blob.offset == slots[1][2]
    assert mutex.offset + mutex.size <= blob.offset
    assert mutex.offset % mutex.size == 0 or mutex.offset % 8 == 0
    header_and_dir = 64 + 16 * len(slots)
    assert mutex.offset >= header_and_dir
    assert blob.offset >= mutex.offset + mutex.size


def test_mutex_vs_robust_digest_disagree():
    def recover(mutex):
        del mutex

    a = posixipc.Layout()
    a.add(posixipc.Mutex)
    b = posixipc.Layout()
    b.add(posixipc.RobustMutex, on_owner_died=recover)
    assert a.digest != b.digest


def test_c_and_python_digest_match():
    layout = posixipc.Layout()
    layout.add(posixipc.Mutex)
    layout.add_bytes(64)
    slots = [(kind, align, size, flags) for kind, align, _off, size, flags in layout.slots]
    py = python_layout_digest(posixipc._posixipc.LAYOUT_VERSION, posixipc._posixipc.abi_tag, slots)
    cside = posixipc._posixipc._layout_digest(posixipc._posixipc.LAYOUT_VERSION, posixipc._posixipc.abi_tag, slots)
    assert py == cside == layout.digest


def test_spawn_child_same_offsets_and_digest(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    parent_conn, child_conn = ctx.Pipe()
    proc = ctx.Process(target=procs.child_layout_report, args=(shm_name, child_conn))
    proc.start()
    try:
        assert parent_conn.poll(5)
        report = parent_conn.recv()
        assert report["digest"] == layout.digest
        assert report["mutex_offset"] == mutex.offset
        assert report["blob_offset"] == blob.offset
        assert report["slots"] == layout.slots
        assert procs.join_or_kill(proc) == 0
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_extra_add_attach_mismatch(shm_name):
    layout, mutex, blob = procs.make_plain_layout()
    region = layout.create(shm_name)
    ctx = procs.spawn_ctx()
    parent_conn, child_conn = ctx.Pipe()
    proc = ctx.Process(target=procs.child_extra_add_attach, args=(shm_name, child_conn))
    proc.start()
    try:
        assert parent_conn.poll(5)
        assert parent_conn.recv() == "mismatch"
        assert procs.join_or_kill(proc) == 0
    finally:
        mutex.close()
        blob.close()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_digest_stable_under_hashseed():
    snippet = "import posixipc\nlayout = posixipc.Layout()\nlayout.add(posixipc.Mutex)\nprint(layout.digest)\n"
    env1 = {**os.environ, "PYTHONHASHSEED": "1"}
    env2 = {**os.environ, "PYTHONHASHSEED": "99"}
    d1 = subprocess.check_output([sys.executable, "-c", snippet], env=env1, text=True)
    d2 = subprocess.check_output([sys.executable, "-c", snippet], env=env2, text=True)
    n1 = d1.strip().splitlines()[-1]
    n2 = d2.strip().splitlines()[-1]
    assert n1 == n2
    assert n1.isdigit()
