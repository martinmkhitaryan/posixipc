#include "py_internal.h"

#include <string.h>

static int shm_is_closed(PosixIPCSharedMemoryObject *r)
{
    uint32_t flags = atomic_load_explicit(&r->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0 || r->core.map == NULL;
}

int posixipc_shmobj_pin(PosixIPCSharedMemoryObject *r)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)r);

    atomic_fetch_add_explicit(&r->pins, 1u, memory_order_acq_rel);
    if (shm_is_closed(r)) {
        atomic_fetch_sub_explicit(&r->pins, 1u, memory_order_acq_rel);
        return posixipc_raise_closed(st);
    }
    return 0;
}

void posixipc_shmobj_unpin(PosixIPCSharedMemoryObject *r)
{
    atomic_fetch_sub_explicit(&r->pins, 1u, memory_order_acq_rel);
}

int posixipc_shmobj_close(PosixIPCSharedMemoryObject *r, int force)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)r);
    uint32_t flags;
    int rc;

    flags = atomic_load_explicit(&r->flags, memory_order_acquire);
    if ((flags & POSIXIPC_FLAG_CLOSED) != 0 || r->core.map == NULL) {
        atomic_fetch_or_explicit(&r->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
        return 0;
    }
    atomic_fetch_or_explicit(&r->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    if (!force && atomic_load_explicit(&r->pins, memory_order_acquire) > 0u) {
        atomic_fetch_and_explicit(&r->flags, ~POSIXIPC_FLAG_CLOSED, memory_order_release);
        PyErr_SetString(PyExc_BufferError, "cannot close SharedMemory while views or handles exist");
        return -1;
    }
    rc = posixipc_shm_close(&r->core);
    if (rc != 0) {
        atomic_fetch_and_explicit(&r->flags, ~POSIXIPC_FLAG_CLOSED, memory_order_release);
        return posixipc_err(st, rc);
    }
    return 0;
}

PyObject *posixipc_shmobj_from_core(posixipc_state *st, posixipc_shm *core, int created)
{
    PosixIPCSharedMemoryObject *r;

    r = (PosixIPCSharedMemoryObject *)st->SharedMemory_Type->tp_alloc(st->SharedMemory_Type, 0);
    if (r == NULL) {
        posixipc_shm_close(core);
        return NULL;
    }
    r->core = *core;
    memset(core, 0, sizeof(*core));
    atomic_store_explicit(&r->pins, 0u, memory_order_relaxed);
    atomic_store_explicit(&r->flags, 0u, memory_order_relaxed);
    r->created = created;
    return (PyObject *)r;
}

static int parse_timeout_seconds(PyObject *obj, int *is_none, double *seconds)
{
    if (obj == NULL || obj == Py_None) {
        *is_none = 1;
        *seconds = 0.0;
        return 0;
    }
    *is_none = 0;
    *seconds = PyFloat_AsDouble(obj);
    if (PyErr_Occurred()) {
        return -1;
    }
    if (*seconds != *seconds || *seconds < 0.0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be non-negative");
        return -1;
    }
    return 0;
}

static int deadline_from_timeout(PyObject *obj, posixipc_deadline *out, const posixipc_deadline **ptr)
{
    int is_none;
    double seconds;
    int rc;

    if (parse_timeout_seconds(obj, &is_none, &seconds) < 0) {
        return -1;
    }
    if (is_none) {
        *ptr = NULL;
        return 0;
    }
    rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, seconds, out);
    if (rc != 0) {
        errno = rc;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    *ptr = out;
    return 0;
}

static posixipc_shm_expect raw_expect(uint32_t payload)
{
    posixipc_shm_expect expect;

    memset(&expect, 0, sizeof(expect));
    expect.layout_version = POSIXIPC_LAYOUT_VERSION;
    expect.abi_tag = posixipc_abi_tag();
    expect.total_size = POSIXIPC_HEADER_BYTES + payload;
    expect.layout_digest = 0;
    return expect;
}

static PyObject *shm_create(PyObject *cls, PyObject *args)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    Py_ssize_t size;
    posixipc_shm_expect expect;
    posixipc_shm core;
    int rc;
    PyObject *obj;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "sn:create", &name, &size)) {
        return NULL;
    }
    if (size < 0 || (size_t)size > (size_t)UINT32_MAX - POSIXIPC_HEADER_BYTES) {
        PyErr_SetString(PyExc_ValueError, "size out of range");
        return NULL;
    }
    expect = raw_expect((uint32_t)size);
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_create(name, &expect, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    rc = posixipc_shm_publish(&core);
    if (rc != 0) {
        posixipc_shm_mark_broken(&core);
        posixipc_shm_close(&core);
        posixipc_shm_unlink(name);
        posixipc_err(st, rc);
        return NULL;
    }
    obj = posixipc_shmobj_from_core(st, &core, 1);
    return obj;
}

static PyObject *shm_attach_unchecked(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    PyObject *timeout_obj = NULL;
    static char *kwlist[] = {"name", "timeout", NULL};
    posixipc_deadline dl;
    const posixipc_deadline *dlp;
    posixipc_shm core;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|O:attach_unchecked", kwlist, &name, &timeout_obj)) {
        return NULL;
    }
    if (timeout_obj == NULL) {
        timeout_obj = PyFloat_FromDouble(5.0);
        if (timeout_obj == NULL) {
            return NULL;
        }
    } else {
        Py_INCREF(timeout_obj);
    }
    if (deadline_from_timeout(timeout_obj, &dl, &dlp) < 0) {
        Py_DECREF(timeout_obj);
        return NULL;
    }
    Py_DECREF(timeout_obj);
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, NULL, dlp, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return posixipc_shmobj_from_core(st, &core, 0);
}

static PyObject *shm_open_or_create(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    Py_ssize_t size;
    PyObject *timeout_obj = NULL;
    static char *kwlist[] = {"name", "size", "timeout", NULL};
    posixipc_deadline dl;
    const posixipc_deadline *dlp;
    posixipc_shm_expect expect;
    posixipc_shm core;
    int rc;
    int created = 0;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "sn|O:open_or_create", kwlist, &name, &size, &timeout_obj)) {
        return NULL;
    }
    if (size < 0 || (size_t)size > (size_t)UINT32_MAX - POSIXIPC_HEADER_BYTES) {
        PyErr_SetString(PyExc_ValueError, "size out of range");
        return NULL;
    }
    if (timeout_obj == NULL) {
        timeout_obj = PyFloat_FromDouble(5.0);
        if (timeout_obj == NULL) {
            return NULL;
        }
    } else {
        Py_INCREF(timeout_obj);
    }
    if (deadline_from_timeout(timeout_obj, &dl, &dlp) < 0) {
        Py_DECREF(timeout_obj);
        return NULL;
    }
    Py_DECREF(timeout_obj);
    expect = raw_expect((uint32_t)size);
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_open_or_create(name, &expect, dlp, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    if (atomic_load_explicit(&core.hdr->state, memory_order_acquire) == POSIXIPC_STATE_INITIALIZING) {
        created = 1;
        rc = posixipc_shm_publish(&core);
        if (rc != 0) {
            posixipc_shm_mark_broken(&core);
            posixipc_shm_close(&core);
            posixipc_shm_unlink(name);
            posixipc_err(st, rc);
            return NULL;
        }
    }
    return posixipc_shmobj_from_core(st, &core, created);
}

static PyObject *shm_unlink_name(PyObject *cls, PyObject *args)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "s:unlink_name", &name)) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_unlink(name);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *shm_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    if (posixipc_shmobj_close((PosixIPCSharedMemoryObject *)self, 0) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *shm_unlink(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;
    char *name;

    if (st == NULL) {
        return NULL;
    }
    if (shm_is_closed(r) || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    name = r->core.name;
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_unlink(name);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *shm_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;

    if (st == NULL) {
        return NULL;
    }
    if (shm_is_closed(r) || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    mod = PyType_GetModule(st->SharedMemory_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_unchecked");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(s)", r->core.name);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *shm_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "SharedMemory cannot be copied");
    return NULL;
}

static PyObject *shm_get_name(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (shm_is_closed(r) || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    return PyUnicode_FromString(r->core.name);
}

static PyObject *shm_get_size(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (shm_is_closed(r)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    return PyLong_FromSize_t(r->core.map_len);
}

static PyObject *shm_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (shm_is_closed(r) || r->core.hdr == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    return PyLong_FromUnsignedLong(r->core.hdr->layout_digest);
}

static PyObject *shm_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;

    return PyBool_FromLong(shm_is_closed(r));
}

static PyObject *shm_get_payload(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t dir_end;
    uint32_t size;
    PyObject *view;
    PyObject *mv;

    if (st == NULL) {
        return NULL;
    }
    if (shm_is_closed(r) || r->core.hdr == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    dir_end = POSIXIPC_HEADER_BYTES + r->core.hdr->directory_bytes;
    if (r->core.hdr->total_size < dir_end) {
        PyErr_SetString(st->exc_LayoutMismatchError, "invalid payload window");
        return NULL;
    }
    size = r->core.hdr->total_size - dir_end;
    view = posixipc_bytes_view(r, dir_end, size);
    if (view == NULL) {
        return NULL;
    }
    mv = PyMemoryView_FromObject(view);
    Py_DECREF(view);
    return mv;
}

static int shm_getbuffer(PyObject *exporter, Py_buffer *view, int flags)
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)exporter;
    posixipc_state *st = posixipc_state_from_obj(exporter);

    if (shm_is_closed(r)) {
        posixipc_raise_closed(st);
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    if (PyBuffer_FillInfo(view, exporter, r->core.map, (Py_ssize_t)r->core.map_len, 1, flags) < 0) {
        posixipc_shmobj_unpin(r);
        return -1;
    }
    return 0;
}

static void shm_releasebuffer(PyObject *exporter, Py_buffer *Py_UNUSED(view))
{
    posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)exporter);
}

static int shm_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static int shm_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void shm_finalize(PyObject *self)
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;

    if (r->created && !shm_is_closed(r)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.SharedMemory", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void shm_dealloc(PyObject *self)
{
    PosixIPCSharedMemoryObject *r = (PosixIPCSharedMemoryObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    PyObject_GC_UnTrack(self);
    if (!shm_is_closed(r)) {
        (void)posixipc_shmobj_close(r, 1);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef shm_methods[] = {
    {"create", shm_create, METH_VARARGS | METH_CLASS, NULL},
    {"attach_unchecked", POSIXIPC_METH(shm_attach_unchecked), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"open_or_create", POSIXIPC_METH(shm_open_or_create), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"unlink_name", shm_unlink_name, METH_VARARGS | METH_CLASS, NULL},
    {"close", shm_close, METH_NOARGS, NULL},
    {"unlink", shm_unlink, METH_NOARGS, NULL},
    {"__reduce__", shm_reduce, METH_NOARGS, NULL},
    {"__copy__", shm_copy, METH_NOARGS, NULL},
    {"__deepcopy__", shm_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef shm_getset[] = {
    {"name", shm_get_name, NULL, NULL, NULL},       {"size", shm_get_size, NULL, NULL, NULL},
    {"digest", shm_get_digest, NULL, NULL, NULL},   {"closed", shm_get_closed, NULL, NULL, NULL},
    {"payload", shm_get_payload, NULL, NULL, NULL}, {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot shm_slots[] = {
    {Py_tp_dealloc, shm_dealloc},     {Py_tp_traverse, shm_traverse},
    {Py_tp_clear, shm_clear},         {Py_tp_finalize, shm_finalize},
    {Py_tp_methods, shm_methods},     {Py_tp_getset, shm_getset},
    {Py_bf_getbuffer, shm_getbuffer}, {Py_bf_releasebuffer, shm_releasebuffer},
    {Py_tp_new, PyType_GenericNew},   {0, NULL},
};

static PyType_Spec shm_spec = {
    .name = "posixipc.SharedMemory",
    .basicsize = sizeof(PosixIPCSharedMemoryObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = shm_slots,
};

static PyObject *mod_attach_unchecked(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    PyObject *cls;

    if (st == NULL) {
        return NULL;
    }
    cls = (PyObject *)st->SharedMemory_Type;
    return shm_attach_unchecked(cls, args, NULL);
}

int posixipc_shm_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;
    static PyMethodDef extra[] = {
        {"_attach_unchecked", mod_attach_unchecked, METH_VARARGS, NULL},
        {NULL, NULL, 0, NULL},
    };
    PyMethodDef *m;

    type = PyType_FromModuleAndSpec(mod, &shm_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->SharedMemory_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "SharedMemory", type) < 0) {
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
