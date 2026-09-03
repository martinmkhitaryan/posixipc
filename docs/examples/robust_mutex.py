"""Recovery function: you already hold the mutex on EOWNERDEAD."""

from __future__ import annotations

import posixipc


def recover(mutex: posixipc.RobustMutex) -> None:
    del mutex


def main() -> None:
    mutex = posixipc.RobustMutex(on_owner_died=recover)
    try:
        with mutex:
            pass
        mutex.acquire()
        mutex.release()
    finally:
        mutex.close()


if __name__ == "__main__":
    main()
