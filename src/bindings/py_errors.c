#include "py_internal.h"

#include <assert.h>
#include <errno.h>

posixipc_state *posixipc_get_state(PyObject *mod)
{
    return (posixipc_state *)PyModule_GetState(mod);
}

posixipc_state *posixipc_state_from_type(PyTypeObject *tp)
{
    PyObject *mod = PyType_GetModule(tp);

    if (mod == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError, "type is not attached to posixipc");
        }
        return NULL;
    }
    return posixipc_get_state(mod);
}

posixipc_state *posixipc_state_from_obj(PyObject *obj)
{
    return posixipc_state_from_type(Py_TYPE(obj));
}

int posixipc_raise_closed(posixipc_state *st)
{
    PyErr_SetString(st->exc_ClosedError, "I/O operation on closed posixipc object");
    return -1;
}

int posixipc_raise_unbound(void)
{
    PyErr_SetString(PyExc_RuntimeError, "handle is not bound; call Layout.create() or attach() first");
    return -1;
}

int posixipc_err(posixipc_state *st, int rc)
{
    if (rc == 0) {
        return 0;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        return -1;
    }
    if (rc == POSIXIPC_ERROR_NOT_READY) {
        PyErr_SetString(PyExc_TimeoutError, "shared memory segment not ready");
        return -1;
    }
    if (rc == POSIXIPC_ERROR_BROKEN || rc == POSIXIPC_ERROR_LAYOUT_MISMATCH) {
        PyErr_SetString(st->exc_LayoutMismatchError, "shared memory layout or ABI mismatch");
        return -1;
    }
    assert(rc != ETIMEDOUT && rc != EBUSY);
    assert(rc < 5000);
    if (rc == ETIMEDOUT || rc == EBUSY || rc >= 5000) {
        PyErr_SetString(st->exc_PosixIPCError, "internal posixipc error code");
        return -1;
    }
    errno = rc;
    PyErr_SetFromErrno(PyExc_OSError);
    return -1;
}
