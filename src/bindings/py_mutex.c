#include "py_internal.h"

#include <string.h>

#define POSIXIPC_MUTEX_CAPSULE_NAME "posixipc.mutex.v1"
#define POSIXIPC_MUTEX_CAPSULE_VERSION 1u

typedef struct posixipc_mutex_capsule
{
    uint32_t version;
    pthread_mutex_t *mutex;
    int (*retain)(struct posixipc_mutex_capsule *);
    int (*release)(struct posixipc_mutex_capsule *);
} posixipc_mutex_capsule;

typedef struct
{
    posixipc_mutex_capsule pub;
    PosixIPCSharedMemoryObject *region;
} posixipc_mutex_capsule_impl;

static int capsule_retain(posixipc_mutex_capsule *ctx)
{
    posixipc_mutex_capsule_impl *impl = (posixipc_mutex_capsule_impl *)ctx;

    if (impl->region == NULL) {
        PyErr_SetString(PyExc_ValueError, "mutex capsule has no region");
        return -1;
    }
    return posixipc_shmobj_pin(impl->region);
}

static int capsule_release(posixipc_mutex_capsule *ctx)
{
    posixipc_mutex_capsule_impl *impl = (posixipc_mutex_capsule_impl *)ctx;

    if (impl->region == NULL) {
        PyErr_SetString(PyExc_ValueError, "mutex capsule has no region");
        return -1;
    }
    posixipc_shmobj_unpin(impl->region);
    return 0;
}

static void capsule_destructor(PyObject *capsule)
{
    posixipc_mutex_capsule_impl *impl;

    impl = (posixipc_mutex_capsule_impl *)PyCapsule_GetPointer(capsule, POSIXIPC_MUTEX_CAPSULE_NAME);
    if (impl == NULL) {
        PyErr_Clear();
        return;
    }
    if (impl->region != NULL) {
        posixipc_shmobj_unpin(impl->region);
        Py_DECREF(impl->region);
        impl->region = NULL;
    }
    PyMem_Free(impl);
}

PyObject *posixipc_mutex_capsule_new(PosixIPCSharedMemoryObject *r, pthread_mutex_t *lock)
{
    posixipc_mutex_capsule_impl *impl;
    PyObject *cap;

    if (r == NULL || lock == NULL) {
        return posixipc_raise_unbound(), NULL;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return NULL;
    }
    impl = (posixipc_mutex_capsule_impl *)PyMem_Calloc(1, sizeof(*impl));
    if (impl == NULL) {
        posixipc_shmobj_unpin(r);
        return PyErr_NoMemory();
    }
    impl->pub.version = POSIXIPC_MUTEX_CAPSULE_VERSION;
    impl->pub.mutex = lock;
    impl->pub.retain = capsule_retain;
    impl->pub.release = capsule_release;
    impl->region = r;
    Py_INCREF(r);
    cap = PyCapsule_New(impl, POSIXIPC_MUTEX_CAPSULE_NAME, capsule_destructor);
    if (cap == NULL) {
        posixipc_shmobj_unpin(r);
        Py_DECREF(r);
        PyMem_Free(impl);
        return NULL;
    }
    return cap;
}

static int mutex_flags_closed(uint32_t flags)
{
    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static int mutex_check_open(PosixIPCMutexObject *m, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);

    if (mutex_flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || m->lock == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

PyObject *posixipc_mutex_after_lock(PosixIPCMutexObject *m, int rc)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)m);
    PyObject *fn;
    PyObject *res;
    int urc;

    if (st == NULL) {
        return NULL;
    }
    if (rc == 0) {
        atomic_store_explicit(&m->locked, 1, memory_order_release);
        Py_RETURN_TRUE;
    }
    if (rc == EOWNERDEAD) {
        atomic_store_explicit(&m->locked, 1, memory_order_release);
        fn = m->on_owner_died;
        if (fn == NULL) {
            posixipc_mutex_unlock(m->lock);
            atomic_store_explicit(&m->locked, 0, memory_order_release);
            PyErr_SetString(PyExc_TypeError, "RobustMutex has no on_owner_died; "
                                             "assign it or rebuild the Layout");
            return NULL;
        }
        Py_INCREF(fn);
        res = PyObject_CallOneArg(fn, (PyObject *)m);
        Py_DECREF(fn);
        if (res == NULL) {
            posixipc_mutex_unlock(m->lock);
            atomic_store_explicit(&m->locked, 0, memory_order_release);
            return NULL;
        }
        Py_DECREF(res);
        urc = posixipc_mutex_consistent(m->lock);
        if (urc != 0) {
            posixipc_mutex_unlock(m->lock);
            atomic_store_explicit(&m->locked, 0, memory_order_release);
            posixipc_err(st, urc);
            return NULL;
        }
        Py_RETURN_TRUE;
    }
    if (rc == ENOTRECOVERABLE) {
        PyErr_SetString(st->exc_NotRecoverableError, "mutex is permanently unusable");
        return NULL;
    }
    if (rc == ETIMEDOUT || rc == EBUSY) {
        Py_RETURN_FALSE;
    }
    if (rc == EDEADLK) {
        PyErr_SetString(PyExc_RuntimeError, "lock already held by this thread");
        return NULL;
    }
    posixipc_err(st, rc);
    return NULL;
}

static int mutex_lock_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_mutex_lock_until((pthread_mutex_t *)arg, slice);
}

static PyObject *mutex_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *timeout_obj = Py_None;
    int blocking = 1;
    int interruptible = 1;
    Py_ssize_t nkw = kwnames != NULL ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t i;
    int timeout_none;
    double timeout;
    int rc;
    pthread_mutex_t *lock;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;

    if (st == NULL) {
        return NULL;
    }
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "acquire() takes at most 1 positional argument");
        return NULL;
    }
    if (nargs == 1) {
        timeout_obj = args[0];
    }
    for (i = 0; i < nkw; i++) {
        const char *key = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));
        PyObject *val = args[nargs + i];

        if (key == NULL) {
            return NULL;
        }
        if (strcmp(key, "timeout") == 0) {
            if (nargs >= 1) {
                PyErr_SetString(PyExc_TypeError, "acquire() got multiple values for argument "
                                                 "'timeout'");
                return NULL;
            }
            timeout_obj = val;
        } else if (strcmp(key, "blocking") == 0) {
            blocking = PyObject_IsTrue(val);
            if (blocking < 0) {
                return NULL;
            }
        } else if (strcmp(key, "interruptible") == 0) {
            interruptible = PyObject_IsTrue(val);
            if (interruptible < 0) {
                return NULL;
            }
        } else {
            PyErr_Format(PyExc_TypeError, "acquire() got an unexpected keyword argument '%s'", key);
            return NULL;
        }
    }
    if (mutex_check_open(m, st) < 0) {
        return NULL;
    }
    if (timeout_obj == Py_None) {
        timeout_none = 1;
        timeout = 0.0;
    } else {
        timeout_none = 0;
        timeout = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred()) {
            return NULL;
        }
        if (timeout != timeout || timeout < 0.0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be non-negative");
            return NULL;
        }
    }
    if (!blocking && !timeout_none && timeout > 0.0) {
        PyErr_SetString(PyExc_ValueError, "can't specify a timeout for a non-blocking call");
        return NULL;
    }
    lock = m->lock;
    rc = posixipc_mutex_trylock(lock);
    if (rc != EBUSY) {
        return posixipc_mutex_after_lock(m, rc);
    }
    if (!blocking || (!timeout_none && timeout == 0.0)) {
        Py_RETURN_FALSE;
    }
    if (!timeout_none) {
        rc = posixipc_deadline_from_seconds(posixipc_mutex_clock(), timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!interruptible && timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mutex_lock(lock);
        Py_END_ALLOW_THREADS return posixipc_mutex_after_lock(m, rc);
    }
    rc = posixipc_blocking_wait(mutex_lock_until_cb, lock, userp, interruptible, posixipc_mutex_clock());
    return posixipc_mutex_after_lock(m, rc);
}

static PyObject *mutex_release(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (mutex_check_open(m, st) < 0) {
        return NULL;
    }
    rc = posixipc_mutex_unlock(m->lock);
    if (rc == EPERM) {
        PyErr_SetString(PyExc_RuntimeError, "release unlocked lock");
        return NULL;
    }
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    atomic_store_explicit(&m->locked, 0, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *mutex_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *ok = mutex_acquire(self, NULL, 0, NULL);

    if (ok == NULL) {
        return NULL;
    }
    if (ok == Py_False) {
        Py_DECREF(ok);
        PyErr_SetString(PyExc_RuntimeError, "failed to acquire mutex");
        return NULL;
    }
    Py_DECREF(ok);
    return Py_NewRef(self);
}

static PyObject *mutex_exit(PyObject *self, PyObject *Py_UNUSED(args))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    if (atomic_load_explicit(&m->locked, memory_order_acquire)) {
        return mutex_release(self, NULL);
    }
    Py_RETURN_NONE;
}

static PyObject *mutex_as_capsule(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL || mutex_check_open(m, st) < 0) {
        return NULL;
    }
    if (m->region == NULL) {
        PyErr_SetString(PyExc_TypeError, "as_capsule() requires a process-shared bound mutex");
        return NULL;
    }
    return posixipc_mutex_capsule_new((PosixIPCSharedMemoryObject *)m->region, m->lock);
}

static PyObject *mutex_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL) {
        return NULL;
    }
    if (mutex_flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && m->lock != NULL) {
        if (atomic_load_explicit(&m->locked, memory_order_acquire)) {
            (void)posixipc_mutex_unlock(m->lock);
            atomic_store_explicit(&m->locked, 0, memory_order_release);
        }
        (void)posixipc_mutex_destroy(m->lock);
    }
    if (m->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)m->region);
        Py_CLEAR(m->region);
    }
    m->lock = NULL;
    atomic_fetch_or_explicit(&m->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *mutex_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc mutex handles cannot be copied");
    return NULL;
}

static PyObject *mutex_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);
    PosixIPCSharedMemoryObject *r;
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;
    uint32_t digest;

    if (st == NULL) {
        return NULL;
    }
    if (mutex_flags_closed(flags)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if ((flags & POSIXIPC_FLAG_PROCESS_SHARED) == 0 || (flags & POSIXIPC_FLAG_BOUND) == 0 || m->region == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot pickle a process-private or unbound handle");
        return NULL;
    }
    r = (PosixIPCSharedMemoryObject *)m->region;
    if (r->core.hdr == NULL || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    digest = r->core.hdr->layout_digest;
    mod = PyType_GetModule(st->Mutex_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_slot");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(sIII)", r->core.name, m->slot, (unsigned)m->kind, digest);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *mutex_get_on_owner_died(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    if (m->on_owner_died == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(m->on_owner_died);
}

static int mutex_set_on_owner_died(PyObject *self, PyObject *value, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    if (value == NULL || value == Py_None) {
        Py_CLEAR(m->on_owner_died);
        return 0;
    }
    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "on_owner_died must be callable");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(m->on_owner_died, value);
    return 0;
}

static PyObject *mutex_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_PROCESS_SHARED) != 0);
}

static PyObject *mutex_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_BOUND) != 0);
}

static PyObject *mutex_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);

    return PyBool_FromLong(mutex_flags_closed(flags));
}

static PyObject *mutex_get_region(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    if (m->region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(m->region);
}

static PyObject *mutex_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexObject *)self)->slot);
}

static PyObject *mutex_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexObject *)self)->kind);
}

static PyObject *mutex_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    PosixIPCSharedMemoryObject *r;

    if (m->region == NULL) {
        Py_RETURN_NONE;
    }
    r = (PosixIPCSharedMemoryObject *)m->region;
    if (r->core.hdr == NULL) {
        Py_RETURN_NONE;
    }
    return PyLong_FromUnsignedLong(r->core.hdr->layout_digest);
}

static PyObject *mutex_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexObject *)self)->offset);
}

static PyObject *mutex_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexObject *)self)->size);
}

static int mutex_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"process_shared", "prio_inherit", "on_owner_died", NULL};
    int process_shared = 0;
    int prio_inherit = 0;
    PyObject *on_owner_died = NULL;
    uint32_t flags;
    posixipc_mutex_config cfg;
    int rc;
    int is_robust;

    if (st == NULL) {
        return -1;
    }
    flags = atomic_load_explicit(&m->flags, memory_order_acquire);
    if ((flags & POSIXIPC_FLAG_BOUND) != 0 || (flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "mutex handle is already initialized");
        return -1;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|$ppO:Mutex", kwlist, &process_shared, &prio_inherit, &on_owner_died)) {
        return -1;
    }
    is_robust = PyObject_TypeCheck(self, st->RobustMutex_Type);
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared mutex requires Layout.add()");
        return -1;
    }
    if (is_robust) {
        if (on_owner_died == NULL || on_owner_died == Py_None) {
            PyErr_SetString(PyExc_TypeError, "RobustMutex requires on_owner_died");
            return -1;
        }
        if (!PyCallable_Check(on_owner_died)) {
            PyErr_SetString(PyExc_TypeError, "on_owner_died must be callable");
            return -1;
        }
    } else if (on_owner_died != NULL && on_owner_died != Py_None) {
        PyErr_SetString(PyExc_TypeError, "Mutex() does not take on_owner_died");
        return -1;
    }
    cfg.flags = 0;
    if (prio_inherit) {
        cfg.flags |= POSIXIPC_FLAG_PRIORITY_INHERIT;
    }
    if (is_robust) {
        cfg.flags |= POSIXIPC_FLAG_ROBUST;
    }
    rc = posixipc_mutex_init(&m->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    m->lock = &m->inline_storage;
    m->kind = is_robust ? POSIXIPC_KIND_ROBUST_MUTEX : POSIXIPC_KIND_MUTEX;
    m->align = (uint16_t)posixipc_kind_align(m->kind);
    m->offset = 0;
    m->size = posixipc_kind_size(m->kind, 0);
    m->init_flags = cfg.flags | POSIXIPC_FLAG_OWNS_STORAGE;
    m->slot = 0;
    if (is_robust) {
        Py_INCREF(on_owner_died);
        m->on_owner_died = on_owner_died;
    }
    atomic_store_explicit(&m->locked, 0, memory_order_relaxed);
    atomic_store_explicit(&m->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND | cfg.flags,
                          memory_order_release);
    return 0;
}

static int mutex_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(m->region);
    Py_VISIT(m->on_owner_died);
    return 0;
}

static int mutex_clear(PyObject *self)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;

    Py_CLEAR(m->on_owner_died);
    return 0;
}

static void mutex_finalize(PyObject *self)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    uint32_t flags = atomic_load_explicit(&m->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !mutex_flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.Mutex", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void mutex_dealloc(PyObject *self)
{
    PosixIPCMutexObject *m = (PosixIPCMutexObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&m->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!mutex_flags_closed(flags)) {
        PyObject *r = mutex_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(m->on_owner_died);
    Py_CLEAR(m->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef mutex_methods[] = {
    {"acquire", POSIXIPC_METH(mutex_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", mutex_release, METH_NOARGS, NULL},
    {"close", mutex_close, METH_NOARGS, NULL},
    {"as_capsule", mutex_as_capsule, METH_NOARGS, NULL},
    {"__enter__", mutex_enter, METH_NOARGS, NULL},
    {"__exit__", mutex_exit, METH_VARARGS, NULL},
    {"__reduce__", mutex_reduce, METH_NOARGS, NULL},
    {"__copy__", mutex_copy, METH_NOARGS, NULL},
    {"__deepcopy__", mutex_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef mutex_getset[] = {
    {"on_owner_died", mutex_get_on_owner_died, mutex_set_on_owner_died, NULL, NULL},
    {"process_shared", mutex_get_process_shared, NULL, NULL, NULL},
    {"bound", mutex_get_bound, NULL, NULL, NULL},
    {"closed", mutex_get_closed, NULL, NULL, NULL},
    {"region", mutex_get_region, NULL, NULL, NULL},
    {"slot", mutex_get_slot, NULL, NULL, NULL},
    {"kind", mutex_get_kind, NULL, NULL, NULL},
    {"digest", mutex_get_digest, NULL, NULL, NULL},
    {"offset", mutex_get_offset, NULL, NULL, NULL},
    {"size", mutex_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot mutex_slots[] = {
    {Py_tp_dealloc, mutex_dealloc},   {Py_tp_traverse, mutex_traverse}, {Py_tp_clear, mutex_clear},
    {Py_tp_finalize, mutex_finalize}, {Py_tp_init, mutex_init},         {Py_tp_methods, mutex_methods},
    {Py_tp_getset, mutex_getset},     {Py_tp_new, PyType_GenericNew},   {0, NULL},
};

static PyType_Spec mutex_spec = {
    .name = "posixipc.Mutex",
    .basicsize = sizeof(PosixIPCMutexObject),
    .flags =
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = mutex_slots,
};

static PyType_Slot robust_slots[] = {
    {Py_tp_dealloc, mutex_dealloc},
    {Py_tp_traverse, mutex_traverse},
    {Py_tp_clear, mutex_clear},
    {Py_tp_finalize, mutex_finalize},
    {Py_tp_init, mutex_init},
    {Py_tp_doc, (void *)"Process-shared or private robust mutex."},
    {0, NULL},
};

static PyType_Spec robust_spec = {
    .name = "posixipc.RobustMutex",
    .basicsize = sizeof(PosixIPCMutexObject),
    .flags =
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = robust_slots,
};

PyObject *posixipc_mutex_new_unbound(posixipc_state *st, PyTypeObject *type, uint16_t kind, uint32_t init_flags,
                                     PyObject *on_owner_died, uint32_t slot)
{
    PosixIPCMutexObject *m;

    (void)st;
    m = (PosixIPCMutexObject *)type->tp_alloc(type, 0);
    if (m == NULL) {
        return NULL;
    }
    m->lock = NULL;
    m->region = NULL;
    m->slot = slot;
    m->kind = kind;
    m->align = (uint16_t)posixipc_kind_align(kind);
    m->offset = 0;
    m->size = posixipc_kind_size(kind, 0);
    m->init_flags = init_flags;
    atomic_store_explicit(&m->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    atomic_store_explicit(&m->locked, 0, memory_order_relaxed);
    m->on_owner_died = on_owner_died;
    Py_XINCREF(on_owner_died);
    return (PyObject *)m;
}

int posixipc_mutex_bind(PosixIPCMutexObject *m, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                        uint32_t index, int do_init)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)m);
    void *ptr;
    int rc;
    posixipc_mutex_config cfg;

    if (st == NULL) {
        return -1;
    }
    rc = posixipc_shm_offset_ptr(&r->core, slot->offset, slot->size, slot->align, &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (do_init) {
        cfg.flags = slot->init_flags;
        rc = posixipc_mutex_init((pthread_mutex_t *)ptr, &cfg);
        if (rc != 0) {
            posixipc_err(st, rc);
            return -1;
        }
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(m->region, (PyObject *)r);
    m->lock = (pthread_mutex_t *)ptr;
    m->slot = index;
    m->kind = slot->kind;
    m->align = slot->align;
    m->offset = slot->offset;
    m->size = slot->size;
    m->init_flags = slot->init_flags;
    atomic_fetch_or_explicit(&m->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

int posixipc_mutex_register(PyObject *mod, posixipc_state *st)
{
    PyObject *mutex_type;
    PyObject *bases;
    PyObject *robust_type;

    mutex_type = PyType_FromModuleAndSpec(mod, &mutex_spec, NULL);
    if (mutex_type == NULL) {
        return -1;
    }
    st->Mutex_Type = (PyTypeObject *)mutex_type;
    if (PyModule_AddObjectRef(mod, "Mutex", mutex_type) < 0) {
        return -1;
    }
    bases = PyTuple_Pack(1, mutex_type);
    if (bases == NULL) {
        return -1;
    }
    robust_type = PyType_FromModuleAndSpec(mod, &robust_spec, bases);
    Py_DECREF(bases);
    if (robust_type == NULL) {
        return -1;
    }
    st->RobustMutex_Type = (PyTypeObject *)robust_type;
    if (PyModule_AddObjectRef(mod, "RobustMutex", robust_type) < 0) {
        return -1;
    }
    if (PyModule_AddStringConstant(mod, "MUTEX_CAPSULE_NAME", POSIXIPC_MUTEX_CAPSULE_NAME) < 0) {
        return -1;
    }
    return 0;
}
