import posixipc


def test_raw_create_attach_payload(shm_name):
    region = posixipc.SharedMemory.create(shm_name, 4096)
    try:
        assert region.name == shm_name
        assert region.digest == 0
        assert region.size >= 4096
        payload = region.payload
        assert len(payload) >= 4096
        payload[0] = 42
        other = posixipc.SharedMemory.attach_unchecked(shm_name, timeout=2.0)
        try:
            other_payload = other.payload
            assert other_payload[0] == 42
            assert other.digest == 0
            other_payload.release()
        finally:
            other.close()
    finally:
        payload.release()
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_open_or_create_idempotent(shm_name):
    a = posixipc.SharedMemory.open_or_create(shm_name, 1024, timeout=2.0)
    try:
        b = posixipc.SharedMemory.open_or_create(shm_name, 1024, timeout=2.0)
        try:
            assert a.digest == b.digest == 0
        finally:
            b.close()
    finally:
        a.close()
        posixipc.SharedMemory.unlink_name(shm_name)


def test_header_buffer_is_readonly(shm_name):
    region = posixipc.SharedMemory.create(shm_name, 64)
    try:
        view = memoryview(region)
        assert view.readonly
        raised = False
        try:
            view[0] = 1
        except TypeError:
            raised = True
        assert raised
        view.release()
    finally:
        region.close()
        posixipc.SharedMemory.unlink_name(shm_name)
