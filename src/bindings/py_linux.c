#include "py_internal.h"

#include "posixipc_config.h"
#include "posixipc_eventfd.h"
#include "posixipc_futex.h"
#include "posixipc_memfd.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#if POSIXIPC_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

typedef struct
{
    PyObject_HEAD uint32_t word;
    _Atomic uint32_t flags;
} PosixIPCFutexObject;

typedef struct
{
    PyObject_HEAD int fd;
    _Atomic uint32_t flags;
} PosixIPCEventFDObject;

typedef struct
{
    PyObject_HEAD posixipc_memfd core;
    _Atomic uint32_t pins;
    _Atomic uint32_t flags;
} PosixIPCMemFDObject;

typedef struct
{
    uint32_t *word;
    uint32_t expected;
    int process_shared;
} futex_wait_arg;

static int futex_wait_cb(void *arg, const posixipc_deadline *slice)
{
    futex_wait_arg *a = (futex_wait_arg *)arg;
    int rc = posixipc_futex_wait(a->word, a->expected, slice, a->process_shared);

    if (rc == EINTR) {
        return ETIMEDOUT;
    }
    return rc;
}

static int futex_is_closed(PosixIPCFutexObject *o)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static PyObject *futex_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    PosixIPCFutexObject *o;
    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kw, ":Futex", kwlist)) {
        return NULL;
    }
    o = (PosixIPCFutexObject *)type->tp_alloc(type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->word = 0;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

static PyObject *futex_wait(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCFutexObject *o = (PosixIPCFutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    futex_wait_arg arg;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    unsigned long expected;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (futex_is_closed(o)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "wait() missing expected");
        return NULL;
    }
    expected = PyLong_AsUnsignedLong(args[0]);
    if (PyErr_Occurred()) {
        return NULL;
    }
    if (expected > 0xffffffffu) {
        PyErr_SetString(PyExc_OverflowError, "expected must fit in uint32");
        return NULL;
    }
    if (posixipc_parse_acquire(args + 1, nargs - 1, kwnames, &opts) < 0) {
        return NULL;
    }
    if (!opts.blocking || (!opts.timeout_none && opts.timeout == 0.0)) {
        if (posixipc_futex_load(&o->word) != (uint32_t)expected) {
            Py_RETURN_TRUE;
        }
        Py_RETURN_FALSE;
    }
    if (!opts.timeout_none) {
        rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, opts.timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    arg.word = &o->word;
    arg.expected = (uint32_t)expected;
    arg.process_shared = 0;
    rc = posixipc_blocking_wait(futex_wait_cb, &arg, userp, opts.interruptible, CLOCK_MONOTONIC);
    if (rc == ETIMEDOUT) {
        Py_RETURN_FALSE;
    }
    if (rc != 0) {
        if (rc != POSIXIPC_ERROR_INTERRUPTED) {
            posixipc_err(st, rc);
        }
        return NULL;
    }
    Py_RETURN_TRUE;
}

static PyObject *futex_wake(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PosixIPCFutexObject *o = (PosixIPCFutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    long n = 1;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (futex_is_closed(o)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "wake() takes at most 1 argument");
        return NULL;
    }
    if (nargs == 1) {
        n = PyLong_AsLong(args[0]);
        if (PyErr_Occurred()) {
            return NULL;
        }
        if (n < 0) {
            PyErr_SetString(PyExc_ValueError, "n must be >= 0");
            return NULL;
        }
        if (n > INT_MAX) {
            n = INT_MAX;
        }
    }
    rc = posixipc_futex_wake(&o->word, (int)n, 0);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *futex_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCFutexObject *o = (PosixIPCFutexObject *)self;

    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *futex_get_value(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCFutexObject *o = (PosixIPCFutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL) {
        return NULL;
    }
    if (futex_is_closed(o)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    return PyLong_FromUnsignedLong(posixipc_futex_load(&o->word));
}

static int futex_set_value(PyObject *self, PyObject *value, void *Py_UNUSED(c))
{
    PosixIPCFutexObject *o = (PosixIPCFutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    unsigned long n;

    if (st == NULL) {
        return -1;
    }
    if (futex_is_closed(o)) {
        return posixipc_raise_closed(st);
    }
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot delete value");
        return -1;
    }
    n = PyLong_AsUnsignedLong(value);
    if (PyErr_Occurred()) {
        return -1;
    }
    if (n > 0xffffffffu) {
        PyErr_SetString(PyExc_OverflowError, "value must fit in uint32");
        return -1;
    }
    posixipc_futex_store(&o->word, (uint32_t)n);
    return 0;
}

static PyObject *futex_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(futex_is_closed((PosixIPCFutexObject *)self));
}

static void futex_dealloc(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);

    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef futex_methods[] = {
    {"wait", POSIXIPC_METH(futex_wait), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"wake", POSIXIPC_METH(futex_wake), METH_FASTCALL, NULL},
    {"close", futex_close, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef futex_getset[] = {
    {"value", futex_get_value, futex_set_value, NULL, NULL},
    {"closed", futex_get_closed, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot futex_slots[] = {
    {Py_tp_dealloc, futex_dealloc},
    {Py_tp_new, futex_new},
    {Py_tp_methods, futex_methods},
    {Py_tp_getset, futex_getset},
    {0, NULL},
};

static PyType_Spec futex_spec = {
    .name = "posixipc.linux.Futex",
    .basicsize = sizeof(PosixIPCFutexObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = futex_slots,
};

static int efd_is_closed(PosixIPCEventFDObject *o)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0 || o->fd < 0;
}

static int efd_check_open(PosixIPCEventFDObject *o, posixipc_state *st)
{
    if (efd_is_closed(o)) {
        return posixipc_raise_closed(st);
    }
    return 0;
}

static PyObject *efd_from_fd(posixipc_state *st, int fd)
{
    PosixIPCEventFDObject *o;

    if (st->EventFD_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "EventFD type is missing");
        (void)posixipc_eventfd_close(fd);
        return NULL;
    }
    o = (PosixIPCEventFDObject *)st->EventFD_Type->tp_alloc(st->EventFD_Type, 0);
    if (o == NULL) {
        (void)posixipc_eventfd_close(fd);
        return NULL;
    }
    o->fd = fd;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_BOUND, memory_order_release);
    return (PyObject *)o;
}

static PyObject *efd_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type(type);
    static char *kwlist[] = {"initval", "semaphore", "nonblock", NULL};
    unsigned long initval = 0;
    int semaphore = 0;
    int nonblock = 0;
    int flags = 0;
    int fd = -1;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|kpp:EventFD", kwlist, &initval, &semaphore, &nonblock)) {
        return NULL;
    }
    if (initval > 0xffffffffu) {
        PyErr_SetString(PyExc_OverflowError, "initval must fit in uint32");
        return NULL;
    }
#if POSIXIPC_HAVE_EVENTFD
    flags = EFD_CLOEXEC;
    if (semaphore) {
        flags |= EFD_SEMAPHORE;
    }
    if (nonblock) {
        flags |= EFD_NONBLOCK;
    }
#else
    (void)semaphore;
    (void)nonblock;
    (void)flags;
#endif
    rc = posixipc_eventfd_create((unsigned)initval, flags, &fd);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    return efd_from_fd(st, fd);
}

static PyObject *efd_write(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    unsigned long long n = 1;
    int rc;

    if (st == NULL || efd_check_open(o, st) < 0) {
        return NULL;
    }
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "write() takes at most 1 argument");
        return NULL;
    }
    if (nargs == 1) {
        n = PyLong_AsUnsignedLongLong(args[0]);
        if (PyErr_Occurred()) {
            return NULL;
        }
        if (n == 0) {
            PyErr_SetString(PyExc_ValueError, "write() value must be >= 1");
            return NULL;
        }
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_eventfd_write(o->fd, (uint64_t)n);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *efd_read(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint64_t n = 0;
    int rc;

    if (st == NULL || efd_check_open(o, st) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_eventfd_read(o->fd, &n);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(n);
}

static PyObject *efd_fileno(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL || efd_check_open(o, st) < 0) {
        return NULL;
    }
    return PyLong_FromLong(o->fd);
}

static PyObject *efd_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    int fd;

    if (efd_is_closed(o)) {
        Py_RETURN_NONE;
    }
    fd = o->fd;
    o->fd = -1;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    if (fd >= 0) {
        (void)posixipc_eventfd_close(fd);
    }
    Py_RETURN_NONE;
}

static PyObject *efd_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *mod;
    PyObject *red;
    PyObject *dupfd_type;
    PyObject *dfd;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;

    if (st == NULL || efd_check_open(o, st) < 0) {
        return NULL;
    }
    mod = PyType_GetModule(st->EventFD_Type);
    if (mod == NULL) {
        return NULL;
    }
    red = PyImport_ImportModule("multiprocessing.reduction");
    if (red == NULL) {
        return NULL;
    }
    dupfd_type = PyObject_GetAttrString(red, "DupFd");
    Py_DECREF(red);
    if (dupfd_type == NULL) {
        return NULL;
    }
    dfd = PyObject_CallFunction(dupfd_type, "i", o->fd);
    Py_DECREF(dupfd_type);
    if (dfd == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_rebuild_eventfd");
    if (fn == NULL) {
        Py_DECREF(dfd);
        return NULL;
    }
    args = PyTuple_Pack(1, dfd);
    Py_DECREF(dfd);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *efd_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(efd_is_closed((PosixIPCEventFDObject *)self));
}

static void efd_finalize(PyObject *self)
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;

    if (!efd_is_closed(o)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.linux.EventFD", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void efd_dealloc(PyObject *self)
{
    PosixIPCEventFDObject *o = (PosixIPCEventFDObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    if (!efd_is_closed(o)) {
        PyObject *r = efd_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef efd_methods[] = {
    {"write", POSIXIPC_METH(efd_write), METH_FASTCALL, NULL},
    {"read", efd_read, METH_NOARGS, NULL},
    {"fileno", efd_fileno, METH_NOARGS, NULL},
    {"close", efd_close, METH_NOARGS, NULL},
    {"__reduce__", efd_reduce, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef efd_getset[] = {
    {"closed", efd_get_closed, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot efd_slots[] = {
    {Py_tp_dealloc, efd_dealloc}, {Py_tp_finalize, efd_finalize}, {Py_tp_new, efd_new},
    {Py_tp_methods, efd_methods}, {Py_tp_getset, efd_getset},     {0, NULL},
};

static PyType_Spec efd_spec = {
    .name = "posixipc.linux.EventFD",
    .basicsize = sizeof(PosixIPCEventFDObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = efd_slots,
};

static int memfd_is_closed(PosixIPCMemFDObject *o)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0 || o->core.map == NULL;
}

static int memfd_pin(PosixIPCMemFDObject *o)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)o);

    atomic_fetch_add_explicit(&o->pins, 1u, memory_order_acq_rel);
    if (memfd_is_closed(o)) {
        atomic_fetch_sub_explicit(&o->pins, 1u, memory_order_acq_rel);
        return posixipc_raise_closed(st);
    }
    return 0;
}

static void memfd_unpin(PosixIPCMemFDObject *o)
{
    atomic_fetch_sub_explicit(&o->pins, 1u, memory_order_acq_rel);
}

static PyObject *memfd_from_core(posixipc_state *st, posixipc_memfd *core)
{
    PosixIPCMemFDObject *o;

    if (st->MemFD_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "MemFD type is missing");
        (void)posixipc_memfd_close(core);
        return NULL;
    }
    o = (PosixIPCMemFDObject *)st->MemFD_Type->tp_alloc(st->MemFD_Type, 0);
    if (o == NULL) {
        (void)posixipc_memfd_close(core);
        return NULL;
    }
    o->core = *core;
    memset(core, 0, sizeof(*core));
    core->fd = -1;
    atomic_store_explicit(&o->pins, 0u, memory_order_relaxed);
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_BOUND, memory_order_release);
    return (PyObject *)o;
}

static PyObject *memfd_create(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    static char *kwlist[] = {"size", "name", NULL};
    Py_ssize_t size;
    const char *name = "posixipc";
    posixipc_memfd core;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "n|s:create", kwlist, &size, &name)) {
        return NULL;
    }
    if (size < 1) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 1");
        return NULL;
    }
    memset(&core, 0, sizeof(core));
    core.fd = -1;
    rc = posixipc_memfd_create(name, (size_t)size, &core);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    return memfd_from_core(st, &core);
}

static PyObject *memfd_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (memfd_is_closed(o)) {
        Py_RETURN_NONE;
    }
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    if (atomic_load_explicit(&o->pins, memory_order_acquire) > 0u) {
        atomic_fetch_and_explicit(&o->flags, ~POSIXIPC_FLAG_CLOSED, memory_order_release);
        PyErr_SetString(PyExc_BufferError, "cannot close MemFD while views exist");
        return NULL;
    }
    rc = posixipc_memfd_close(&o->core);
    if (rc != 0) {
        atomic_fetch_and_explicit(&o->flags, ~POSIXIPC_FLAG_CLOSED, memory_order_release);
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *memfd_fileno(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL) {
        return NULL;
    }
    if (memfd_is_closed(o)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    return PyLong_FromLong(o->core.fd);
}

static PyObject *memfd_get_size(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;

    return PyLong_FromSize_t(o->core.size);
}

static PyObject *memfd_get_name(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;

    if (o->core.name == NULL) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(o->core.name);
}

static PyObject *memfd_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(memfd_is_closed((PosixIPCMemFDObject *)self));
}

static PyObject *memfd_get_payload(PyObject *self, void *Py_UNUSED(c))
{
    return PyMemoryView_FromObject(self);
}

static int memfd_getbuffer(PyObject *exporter, Py_buffer *view, int flags)
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)exporter;
    posixipc_state *st = posixipc_state_from_obj(exporter);

    if (st == NULL) {
        return -1;
    }
    if (memfd_is_closed(o)) {
        posixipc_raise_closed(st);
        return -1;
    }
    if (memfd_pin(o) < 0) {
        return -1;
    }
    if (PyBuffer_FillInfo(view, exporter, o->core.map, (Py_ssize_t)o->core.size, 0, flags) < 0) {
        memfd_unpin(o);
        return -1;
    }
    return 0;
}

static void memfd_releasebuffer(PyObject *exporter, Py_buffer *Py_UNUSED(view))
{
    memfd_unpin((PosixIPCMemFDObject *)exporter);
}

static PyObject *memfd_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *mod;
    PyObject *red;
    PyObject *dupfd_type;
    PyObject *dfd;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;
    const char *name;

    if (st == NULL) {
        return NULL;
    }
    if (memfd_is_closed(o)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    mod = PyType_GetModule(st->MemFD_Type);
    if (mod == NULL) {
        return NULL;
    }
    red = PyImport_ImportModule("multiprocessing.reduction");
    if (red == NULL) {
        return NULL;
    }
    dupfd_type = PyObject_GetAttrString(red, "DupFd");
    Py_DECREF(red);
    if (dupfd_type == NULL) {
        return NULL;
    }
    dfd = PyObject_CallFunction(dupfd_type, "i", o->core.fd);
    Py_DECREF(dupfd_type);
    if (dfd == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_rebuild_memfd");
    if (fn == NULL) {
        Py_DECREF(dfd);
        return NULL;
    }
    name = o->core.name != NULL ? o->core.name : "posixipc";
    args = Py_BuildValue("(Ons)", dfd, (Py_ssize_t)o->core.size, name);
    Py_DECREF(dfd);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *memfd_tp_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    (void)type;
    (void)args;
    (void)kw;
    PyErr_SetString(PyExc_TypeError, "MemFD() cannot be called; use MemFD.create()");
    return NULL;
}

static void memfd_finalize(PyObject *self)
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;

    if (!memfd_is_closed(o)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.linux.MemFD", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void memfd_dealloc(PyObject *self)
{
    PosixIPCMemFDObject *o = (PosixIPCMemFDObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    if (!memfd_is_closed(o)) {
        atomic_store_explicit(&o->pins, 0u, memory_order_relaxed);
        (void)posixipc_memfd_close(&o->core);
    }
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef memfd_methods[] = {
    {"create", POSIXIPC_METH(memfd_create), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"close", memfd_close, METH_NOARGS, NULL},
    {"fileno", memfd_fileno, METH_NOARGS, NULL},
    {"__reduce__", memfd_reduce, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef memfd_getset[] = {
    {"size", memfd_get_size, NULL, NULL, NULL},
    {"name", memfd_get_name, NULL, NULL, NULL},
    {"closed", memfd_get_closed, NULL, NULL, NULL},
    {"payload", memfd_get_payload, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot memfd_slots[] = {
    {Py_tp_dealloc, memfd_dealloc},     {Py_tp_finalize, memfd_finalize},
    {Py_tp_methods, memfd_methods},     {Py_tp_getset, memfd_getset},
    {Py_bf_getbuffer, memfd_getbuffer}, {Py_bf_releasebuffer, memfd_releasebuffer},
    {Py_tp_new, memfd_tp_new},          {0, NULL},
};

static PyType_Spec memfd_spec = {
    .name = "posixipc.linux.MemFD",
    .basicsize = sizeof(PosixIPCMemFDObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = memfd_slots,
};

static int fq_is_closed(PosixIPCFutexQueueObject *q)
{
    uint32_t flags = atomic_load_explicit(&q->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static int fq_check_open(PosixIPCFutexQueueObject *q, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&q->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_CLOSED) != 0) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || q->put_lock == NULL || q->region == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static int fq_lock(PosixIPCFutexQueueObject *q, pthread_mutex_t *lock)
{
    int rc = posixipc_mutex_lock(lock);

    if (rc == EOWNERDEAD) {
        rc = posixipc_queue_recover(&q->view.view);
        if (rc != 0) {
            (void)posixipc_mutex_unlock(lock);
            return rc;
        }
        rc = posixipc_mutex_consistent(lock);
        if (rc != 0) {
            (void)posixipc_mutex_unlock(lock);
            return rc;
        }
        posixipc_futex_add(q->view.not_full, 1u);
        posixipc_futex_add(q->view.not_empty, 1u);
        (void)posixipc_futex_wake(q->view.not_full, INT_MAX, 1);
        (void)posixipc_futex_wake(q->view.not_empty, INT_MAX, 1);
        return 0;
    }
    return rc;
}

static int fq_wait_word(PosixIPCFutexQueueObject *q, uint32_t *word, const posixipc_acquire_opts *opts,
                        posixipc_state *st)
{
    futex_wait_arg arg;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    int rc;
    int lrc;

    arg.word = word;
    arg.expected = posixipc_futex_load(word);
    arg.process_shared = 1;
    rc = posixipc_mutex_unlock(q->put_lock);
    if (rc != 0) {
        return rc;
    }
    if (!opts->timeout_none) {
        rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, opts->timeout, &user);
        if (rc != 0) {
            lrc = fq_lock(q, q->put_lock);
            return lrc != 0 ? lrc : rc;
        }
        userp = &user;
    }
    rc = posixipc_blocking_wait(futex_wait_cb, &arg, userp, opts->interruptible, CLOCK_MONOTONIC);
    lrc = fq_lock(q, q->put_lock);
    if (lrc != 0) {
        return lrc;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        posixipc_err(st, rc);
    }
    return rc;
}

PyObject *posixipc_futexq_new_unbound(posixipc_state *st, uint32_t depth, uint32_t item_size, uint32_t first_slot)
{
    PosixIPCFutexQueueObject *q;

    if (st->FutexQueue_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "FutexQueue type is missing");
        return NULL;
    }
    q = (PosixIPCFutexQueueObject *)st->FutexQueue_Type->tp_alloc(st->FutexQueue_Type, 0);
    if (q == NULL) {
        return NULL;
    }
    q->put_lock = NULL;
    q->get_lock = NULL;
    q->region = NULL;
    q->first_slot = first_slot;
    q->depth = depth;
    q->item_size = item_size;
    memset(&q->view, 0, sizeof(q->view));
    atomic_store_explicit(&q->flags, POSIXIPC_FLAG_PROCESS_SHARED, memory_order_relaxed);
    return (PyObject *)q;
}

int posixipc_futexq_init_on_shm(posixipc_shm *core, const posixipc_slot *bytes_slot, uint32_t depth, uint32_t item_size)
{
    void *ptr;
    posixipc_futexq_view view;
    int rc;

    rc = posixipc_shm_offset_ptr(core, bytes_slot->offset, bytes_slot->size, bytes_slot->align, &ptr);
    if (rc != 0) {
        return rc;
    }
    rc = posixipc_futexq_map(ptr, bytes_slot->size, depth, item_size, &view);
    if (rc != 0) {
        return rc;
    }
    return posixipc_futexq_init(&view);
}

int posixipc_futexq_bind(PosixIPCFutexQueueObject *q, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                         uint32_t first)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)q);
    void *ptr;
    int rc;

    if (st == NULL) {
        return -1;
    }
    if (slots[first].kind != POSIXIPC_KIND_ROBUST_MUTEX || slots[first + 1u].kind != POSIXIPC_KIND_ROBUST_MUTEX ||
        slots[first + 2u].kind != POSIXIPC_KIND_BYTES) {
        PyErr_SetString(st->exc_LayoutMismatchError, "FutexQueue slot sequence does not match");
        return -1;
    }
    rc = posixipc_shm_offset_ptr(&r->core, slots[first].offset, slots[first].size, slots[first].align, &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    q->put_lock = (pthread_mutex_t *)ptr;
    rc = posixipc_shm_offset_ptr(&r->core, slots[first + 1u].offset, slots[first + 1u].size, slots[first + 1u].align,
                                 &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    q->get_lock = (pthread_mutex_t *)ptr;
    rc = posixipc_shm_offset_ptr(&r->core, slots[first + 2u].offset, slots[first + 2u].size, slots[first + 2u].align,
                                 &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    rc = posixipc_futexq_map(ptr, slots[first + 2u].size, q->depth, q->item_size, &q->view);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (q->view.view.ctrl->depth != q->depth || q->view.view.ctrl->item_size != q->item_size) {
        PyErr_SetString(st->exc_LayoutMismatchError, "FutexQueue control word does not match layout");
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(q->region, (PyObject *)r);
    q->first_slot = first;
    atomic_fetch_or_explicit(&q->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

static PyObject *fq_put(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    Py_buffer view;
    int rc;
    uint32_t n;

    if (st == NULL || fq_check_open(q, st) < 0) {
        return NULL;
    }
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "put() missing data");
        return NULL;
    }
    if (posixipc_parse_acquire(args + 1, nargs - 1, kwnames, &opts) < 0) {
        return NULL;
    }
    if (PyObject_GetBuffer(args[0], &view, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    if (view.len < 0 || (size_t)view.len != (size_t)q->item_size) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "put() data length must equal item_size");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = fq_lock(q, q->put_lock);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        PyBuffer_Release(&view);
        if (rc == ENOTRECOVERABLE) {
            PyErr_SetString(st->exc_NotRecoverableError, "mutex is permanently unusable");
            return NULL;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    for (;;) {
        if (posixipc_queue_qsize(&q->view.view, &n) != 0) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyBuffer_Release(&view);
            posixipc_err(st, EINVAL);
            return NULL;
        }
        if (n < q->depth) {
            break;
        }
        if (!opts.blocking || (!opts.timeout_none && opts.timeout == 0.0)) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyBuffer_Release(&view);
            Py_RETURN_FALSE;
        }
        rc = fq_wait_word(q, q->view.not_full, &opts, st);
        if (rc == ETIMEDOUT) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyBuffer_Release(&view);
            Py_RETURN_FALSE;
        }
        if (rc != 0) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyBuffer_Release(&view);
            if (rc != POSIXIPC_ERROR_INTERRUPTED) {
                posixipc_err(st, rc);
            }
            return NULL;
        }
    }
    rc = posixipc_queue_put(&q->view.view, view.buf);
    posixipc_futex_add(q->view.not_empty, 1u);
    (void)posixipc_mutex_unlock(q->put_lock);
    PyBuffer_Release(&view);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    (void)posixipc_futex_wake(q->view.not_empty, 1, 1);
    Py_RETURN_TRUE;
}

static PyObject *fq_get(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    PyObject *out;
    char *buf;
    int rc;
    uint32_t n;

    if (st == NULL || fq_check_open(q, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    buf = (char *)PyMem_Malloc(q->item_size);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    Py_BEGIN_ALLOW_THREADS rc = fq_lock(q, q->put_lock);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        PyMem_Free(buf);
        if (rc == ENOTRECOVERABLE) {
            PyErr_SetString(st->exc_NotRecoverableError, "mutex is permanently unusable");
            return NULL;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    for (;;) {
        if (posixipc_queue_qsize(&q->view.view, &n) != 0) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyMem_Free(buf);
            posixipc_err(st, EINVAL);
            return NULL;
        }
        if (n > 0u) {
            break;
        }
        if (!opts.blocking || (!opts.timeout_none && opts.timeout == 0.0)) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyMem_Free(buf);
            Py_RETURN_NONE;
        }
        rc = fq_wait_word(q, q->view.not_empty, &opts, st);
        if (rc == ETIMEDOUT) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyMem_Free(buf);
            Py_RETURN_NONE;
        }
        if (rc != 0) {
            (void)posixipc_mutex_unlock(q->put_lock);
            PyMem_Free(buf);
            if (rc != POSIXIPC_ERROR_INTERRUPTED) {
                posixipc_err(st, rc);
            }
            return NULL;
        }
    }
    rc = posixipc_queue_get(&q->view.view, buf);
    posixipc_futex_add(q->view.not_full, 1u);
    (void)posixipc_mutex_unlock(q->put_lock);
    if (rc != 0) {
        PyMem_Free(buf);
        posixipc_err(st, rc);
        return NULL;
    }
    (void)posixipc_futex_wake(q->view.not_full, 1, 1);
    out = PyBytes_FromStringAndSize(buf, (Py_ssize_t)q->item_size);
    PyMem_Free(buf);
    return out;
}

static PyObject *fq_put_nowait(PyObject *self, PyObject *data)
{
    PyObject *args[2];
    PyObject *names;
    PyObject *res;

    args[0] = data;
    args[1] = Py_False;
    names = PyUnicode_FromString("blocking");
    if (names == NULL) {
        return NULL;
    }
    {
        PyObject *tup = PyTuple_Pack(1, names);

        Py_DECREF(names);
        if (tup == NULL) {
            return NULL;
        }
        res = fq_put(self, args, 1, tup);
        Py_DECREF(tup);
        return res;
    }
}

static PyObject *fq_get_nowait(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *false_obj = Py_False;
    PyObject *names = PyUnicode_FromString("blocking");
    PyObject *tup;
    PyObject *res;

    if (names == NULL) {
        return NULL;
    }
    tup = PyTuple_Pack(1, names);
    Py_DECREF(names);
    if (tup == NULL) {
        return NULL;
    }
    res = fq_get(self, &false_obj, 0, tup);
    Py_DECREF(tup);
    return res;
}

static PyObject *fq_qsize(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t n;
    int rc;

    if (st == NULL || fq_check_open(q, st) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = fq_lock(q, q->put_lock);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        if (rc == ENOTRECOVERABLE) {
            PyErr_SetString(st->exc_NotRecoverableError, "mutex is permanently unusable");
            return NULL;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    rc = posixipc_queue_qsize(&q->view.view, &n);
    (void)posixipc_mutex_unlock(q->put_lock);
    if (rc != 0) {
        posixipc_err(st, EINVAL);
        return NULL;
    }
    return PyLong_FromUnsignedLong(n);
}

static PyObject *fq_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;

    if (fq_is_closed(q)) {
        Py_RETURN_NONE;
    }
    if (q->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)q->region);
        Py_CLEAR(q->region);
    }
    q->put_lock = NULL;
    q->get_lock = NULL;
    atomic_fetch_or_explicit(&q->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *fq_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc handles cannot be copied");
    return NULL;
}

static PyObject *fq_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t flags = atomic_load_explicit(&q->flags, memory_order_acquire);
    PosixIPCSharedMemoryObject *r;
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;

    if (st == NULL) {
        return NULL;
    }
    if (fq_is_closed(q)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || q->region == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot pickle a process-private or unbound handle");
        return NULL;
    }
    r = (PosixIPCSharedMemoryObject *)q->region;
    if (r->core.hdr == NULL || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    mod = PyType_GetModule(st->FutexQueue_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_futex_queue");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(sIIII)", r->core.name, q->first_slot, q->depth, q->item_size, r->core.hdr->layout_digest);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *fq_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    uint32_t flags = atomic_load_explicit(&((PosixIPCFutexQueueObject *)self)->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_BOUND) != 0);
}

static PyObject *fq_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(fq_is_closed((PosixIPCFutexQueueObject *)self));
}

static PyObject *fq_get_region(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;

    if (q->region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(q->region);
}

static PyObject *fq_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCFutexQueueObject *)self)->first_slot);
}

static PyObject *fq_get_depth(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCFutexQueueObject *)self)->depth);
}

static PyObject *fq_get_item_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCFutexQueueObject *)self)->item_size);
}

static int fq_init(PyObject *self, PyObject *args, PyObject *kw)
{
    (void)self;
    (void)args;
    (void)kw;
    PyErr_SetString(PyExc_TypeError, "FutexQueue() cannot be called; use Layout.add(FutexQueue, ...)");
    return -1;
}

static int fq_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(q->region);
    return 0;
}

static int fq_clear(PyObject *self)
{
    (void)self;
    return 0;
}

static void fq_dealloc(PyObject *self)
{
    PosixIPCFutexQueueObject *q = (PosixIPCFutexQueueObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_GC_UnTrack(self);
    if (!fq_is_closed(q)) {
        PyObject *r = fq_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(q->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef fq_methods[] = {
    {"put", POSIXIPC_METH(fq_put), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"get", POSIXIPC_METH(fq_get), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"put_nowait", fq_put_nowait, METH_O, NULL},
    {"get_nowait", fq_get_nowait, METH_NOARGS, NULL},
    {"qsize", fq_qsize, METH_NOARGS, NULL},
    {"close", fq_close, METH_NOARGS, NULL},
    {"__reduce__", fq_reduce, METH_NOARGS, NULL},
    {"__copy__", fq_copy, METH_NOARGS, NULL},
    {"__deepcopy__", fq_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef fq_getset[] = {
    {"bound", fq_get_bound, NULL, NULL, NULL},
    {"closed", fq_get_closed, NULL, NULL, NULL},
    {"region", fq_get_region, NULL, NULL, NULL},
    {"slot", fq_get_slot, NULL, NULL, NULL},
    {"depth", fq_get_depth, NULL, NULL, NULL},
    {"item_size", fq_get_item_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot fq_slots[] = {
    {Py_tp_dealloc, fq_dealloc}, {Py_tp_traverse, fq_traverse}, {Py_tp_clear, fq_clear},        {Py_tp_init, fq_init},
    {Py_tp_methods, fq_methods}, {Py_tp_getset, fq_getset},     {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec fq_spec = {
    .name = "posixipc.linux.FutexQueue",
    .basicsize = sizeof(PosixIPCFutexQueueObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = fq_slots,
};

static PyObject *mod_rebuild_eventfd(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    PyObject *dfd;
    PyObject *fdobj;
    long fd;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "O:_rebuild_eventfd", &dfd)) {
        return NULL;
    }
    fdobj = PyObject_CallMethod(dfd, "detach", NULL);
    if (fdobj == NULL) {
        return NULL;
    }
    fd = PyLong_AsLong(fdobj);
    Py_DECREF(fdobj);
    if (PyErr_Occurred()) {
        return NULL;
    }
    if (fd < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid eventfd");
        return NULL;
    }
    return efd_from_fd(st, (int)fd);
}

static PyObject *mod_rebuild_memfd(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    PyObject *dfd;
    PyObject *fdobj;
    Py_ssize_t size;
    const char *name;
    long fd;
    posixipc_memfd core;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "Ons:_rebuild_memfd", &dfd, &size, &name)) {
        return NULL;
    }
    if (size < 1) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 1");
        return NULL;
    }
    fdobj = PyObject_CallMethod(dfd, "detach", NULL);
    if (fdobj == NULL) {
        return NULL;
    }
    fd = PyLong_AsLong(fdobj);
    Py_DECREF(fdobj);
    if (PyErr_Occurred()) {
        return NULL;
    }
    if (fd < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid memfd");
        return NULL;
    }
    memset(&core, 0, sizeof(core));
    core.fd = -1;
    rc = posixipc_memfd_from_fd((int)fd, (size_t)size, name, &core);
    if (rc != 0) {
        (void)posixipc_eventfd_close((int)fd);
        posixipc_err(st, rc);
        return NULL;
    }
    return memfd_from_core(st, &core);
}

int posixipc_linux_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;
    static PyMethodDef extra[] = {
        {"_rebuild_eventfd", mod_rebuild_eventfd, METH_VARARGS, NULL},
        {"_rebuild_memfd", mod_rebuild_memfd, METH_VARARGS, NULL},
        {NULL, NULL, 0, NULL},
    };
    PyMethodDef *m;

    type = PyType_FromModuleAndSpec(mod, &futex_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Futex_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Futex", type) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &efd_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->EventFD_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "EventFD", type) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &memfd_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->MemFD_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "MemFD", type) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &fq_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->FutexQueue_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "FutexQueue", type) < 0) {
        return -1;
    }
    for (m = extra; m->ml_name != NULL; m++) {
        PyObject *fn = PyCFunction_New(m, mod);

        if (fn == NULL) {
            return -1;
        }
        if (PyModule_AddObject(mod, m->ml_name, fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }
    return 0;
}
