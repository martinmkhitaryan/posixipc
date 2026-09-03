import pickle

import pytest

import posixipc
from posixipc.linux import EventFD, features
from tests.helpers import procs

pytestmark = pytest.mark.skipif(not features.eventfd, reason="eventfd missing")


def test_eventfd_write_read():
    e = EventFD()
    try:
        e.write(4)
        assert e.read() == 4
        assert e.fileno() >= 0
    finally:
        e.close()
    with pytest.raises(posixipc.ClosedError):
        e.read()


def test_eventfd_pickle_spawn():
    e = EventFD()
    ctx = procs.spawn_ctx()
    parent, child = ctx.Pipe(duplex=False)
    proc = ctx.Process(target=procs.child_eventfd_read, args=(e, child))
    try:
        e.write(9)
        dumped = pickle.dumps(e)
        clone = pickle.loads(dumped)
        clone.close()
        proc.start()
        assert parent.recv() == 9
        assert procs.join_or_kill(proc) == 0
    finally:
        parent.close()
        child.close()
        if proc.is_alive():
            proc.kill()
            proc.join(2)
        e.close()
