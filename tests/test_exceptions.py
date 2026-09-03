from posixipc.exceptions import (
    ClosedError,
    LayoutMismatchError,
    NotRecoverableError,
    PosixIPCError,
)


def test_exception_hierarchy():
    assert issubclass(NotRecoverableError, PosixIPCError)
    assert issubclass(ClosedError, PosixIPCError)
    assert issubclass(ClosedError, ValueError)
    assert issubclass(LayoutMismatchError, PosixIPCError)
    assert issubclass(LayoutMismatchError, ValueError)
