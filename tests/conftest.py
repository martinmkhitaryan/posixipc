import os
import uuid
from pathlib import Path

import pytest


@pytest.fixture
def shm_name():
    name = f"/posixipc-test-{uuid.uuid4()}"
    yield name
    path = Path("/dev/shm") / name.lstrip("/")
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    except OSError:
        try:
            import posixipc  # noqa: F401
        except ImportError:
            pass


@pytest.fixture
def worker_shm_prefix():
    worker = os.environ.get("PYTEST_XDIST_WORKER", "gw0")
    return f"/posixipc-test-{worker}-"
