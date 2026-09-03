import mmap
import os
import random
import struct
from pathlib import Path

import posixipc


def _shm_path(name):
    return Path("/dev/shm") / name.lstrip("/")


def _mutate_and_attach(shm_name, rng, field):
    layout = posixipc.Layout()
    mutex = layout.add(posixipc.Mutex)
    blob = layout.add_bytes(64)
    region = layout.create(shm_name)
    path = _shm_path(shm_name)
    size = path.stat().st_size
    fd = os.open(path, os.O_RDWR)
    try:
        mm = mmap.mmap(fd, size)
        try:
            if field == "magic":
                mm[0:4] = rng.randbytes(4)
            elif field == "layout_version":
                mm[4:6] = struct.pack("<H", rng.randrange(0, 65536))
            elif field == "slot_count":
                mm[6:8] = struct.pack("<H", rng.randrange(0, 65536))
            elif field == "abi_tag":
                mm[8:12] = struct.pack("<I", rng.randrange(0, 2**32))
            elif field == "total_size":
                mm[16:20] = struct.pack("<I", rng.choice([0, 1, 8, 10**9]))
            elif field == "directory_bytes":
                mm[20:24] = struct.pack("<I", rng.randrange(0, 2**32))
            elif field == "state":
                mm[24:28] = struct.pack("<I", rng.choice([0, 1, 3, 99]))
            elif field == "digest":
                mm[28:32] = struct.pack("<I", rng.randrange(1, 2**32))
            elif field == "slot_kind":
                mm[64:66] = struct.pack("<H", rng.randrange(0, 65536))
            elif field == "slot_align":
                mm[66:68] = struct.pack("<H", rng.randrange(0, 65536))
            elif field == "slot_offset":
                mm[68:72] = struct.pack("<I", rng.randrange(0, 2**32))
            elif field == "slot_size":
                mm[72:76] = struct.pack("<I", rng.randrange(0, 2**32))
            elif field == "slot_flags":
                mm[76:80] = struct.pack("<I", rng.randrange(0, 2**32))
            else:
                raise AssertionError(field)
            mm.flush()
        finally:
            mm.close()
    finally:
        os.close(fd)

    child = posixipc.Layout()
    child.add(posixipc.Mutex)
    child.add_bytes(64)
    raised = False
    try:
        attached = child.attach(shm_name, timeout=0.2)
    except (
        posixipc.LayoutMismatchError,
        TimeoutError,
        OSError,
        posixipc.PosixIPCError,
    ):
        raised = True
    else:
        attached.close()
    mutex.close()
    blob.close()
    region.close()
    posixipc.SharedMemory.unlink_name(shm_name)
    assert raised, f"attach succeeded after mutating {field}"


def test_fuzz_attach_header_and_directory(shm_name):
    rng = random.Random(20260903)
    fields = (
        "magic",
        "layout_version",
        "slot_count",
        "abi_tag",
        "total_size",
        "directory_bytes",
        "state",
        "digest",
        "slot_kind",
        "slot_align",
        "slot_offset",
        "slot_size",
        "slot_flags",
    )
    for i, field in enumerate(fields):
        _mutate_and_attach(f"{shm_name}-{i}", rng, field)
