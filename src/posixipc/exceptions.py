class PosixIPCError(Exception):
    pass


class NotRecoverableError(PosixIPCError):
    """Permanently unusable. You do not hold the lock."""


class ClosedError(PosixIPCError, ValueError):
    pass


class LayoutMismatchError(PosixIPCError, ValueError):
    pass
