import pickle

import pytest

import posixipc
from posixipc.linux import MemFD, features, memfd
from tests.helpers import procs

pytestmark = pytest.mark.skipif(not features.memfd, reason="memfd missing")


def test_memfd_create_and_buffer():
    m = memfd(64, name="posixipc-test")
    try:
        assert m.size == 64
        assert m.name == "posixipc-test"
        assert m.fileno() >= 0
        mv = memoryview(m)
        mv[:5] = b"hello"
        assert bytes(m.payload[:5]) == b"hello"
        mv.release()
    finally:
        m.close()
    with pytest.raises(posixipc.ClosedError):
        m.fileno()


def test_memfd_pickle_spawn():
    m = MemFD.create(64)
    ctx = procs.spawn_ctx()
    parent, child = ctx.Pipe(duplex=False)
    proc = ctx.Process(target=procs.child_memfd_read, args=(m, child))
    try:
        memoryview(m)[:5] = b"hello"
        dumped = pickle.dumps(m)
        clone = pickle.loads(dumped)
        clone.close()
        proc.start()
        assert parent.recv() == b"hello"
        assert procs.join_or_kill(proc) == 0
    finally:
        parent.close()
        child.close()
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        m.close()
