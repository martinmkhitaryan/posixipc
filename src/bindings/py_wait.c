#include "py_internal.h"

#include "posixipc_clocksym.h"

#include <string.h>

#define SLICE_NS 50000000L

clockid_t posixipc_mutex_clock(void)
{
    return posixipc_have_mutex_clocklock() ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

static void add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int posixipc_parse_acquire(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, posixipc_acquire_opts *out)
{
    PyObject *timeout_obj = Py_None;
    Py_ssize_t nkw = kwnames != NULL ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t i;

    out->timeout_none = 1;
    out->timeout = 0.0;
    out->blocking = 1;
    out->interruptible = 1;
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "takes at most 1 positional argument");
        return -1;
    }
    if (nargs == 1) {
        timeout_obj = args[0];
    }
    for (i = 0; i < nkw; i++) {
        const char *key = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));
        PyObject *val = args[nargs + i];

        if (key == NULL) {
            return -1;
        }
        if (strcmp(key, "timeout") == 0) {
            if (nargs >= 1) {
                PyErr_SetString(PyExc_TypeError, "got multiple values for argument 'timeout'");
                return -1;
            }
            timeout_obj = val;
        } else if (strcmp(key, "blocking") == 0) {
            out->blocking = PyObject_IsTrue(val);
            if (out->blocking < 0) {
                return -1;
            }
        } else if (strcmp(key, "interruptible") == 0) {
            out->interruptible = PyObject_IsTrue(val);
            if (out->interruptible < 0) {
                return -1;
            }
        } else {
            PyErr_Format(PyExc_TypeError, "unexpected keyword argument '%s'", key);
            return -1;
        }
    }
    if (timeout_obj == Py_None) {
        out->timeout_none = 1;
        out->timeout = 0.0;
    } else {
        out->timeout_none = 0;
        out->timeout = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred()) {
            return -1;
        }
        if (out->timeout != out->timeout || out->timeout < 0.0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be non-negative");
            return -1;
        }
    }
    if (!out->blocking && !out->timeout_none && out->timeout > 0.0) {
        PyErr_SetString(PyExc_ValueError, "can't specify a timeout for a non-blocking call");
        return -1;
    }
    return 0;
}

int posixipc_blocking_wait(int (*fn)(void *, const posixipc_deadline *), void *arg, const posixipc_deadline *user,
                           int interruptible, clockid_t clk)
{
    posixipc_deadline slice;
    posixipc_deadline nowd;
    int rc;

    if (!interruptible) {
        if (user == NULL) {
            posixipc_deadline inf;

            inf.clk = clk;
            inf.ts.tv_sec = 0x7fffffff;
            inf.ts.tv_nsec = 0;
            Py_BEGIN_ALLOW_THREADS rc = fn(arg, &inf);
            Py_END_ALLOW_THREADS return rc;
        }
        Py_BEGIN_ALLOW_THREADS rc = fn(arg, user);
        Py_END_ALLOW_THREADS return rc;
    }

    for (;;) {
        nowd.clk = user != NULL ? user->clk : clk;
        rc = posixipc_now(nowd.clk, &nowd.ts);
        if (rc != 0) {
            return rc;
        }
        slice = nowd;
        add_ns(&slice.ts, SLICE_NS);
        if (user != NULL) {
            posixipc_deadline_min(user, &slice, &slice);
        }
        Py_BEGIN_ALLOW_THREADS rc = fn(arg, &slice);
        Py_END_ALLOW_THREADS if (rc != ETIMEDOUT)
        {
            return rc;
        }
        if (PyErr_CheckSignals() < 0) {
            return POSIXIPC_ERROR_INTERRUPTED;
        }
        if (user != NULL && posixipc_deadline_expired(user)) {
            return ETIMEDOUT;
        }
    }
}
