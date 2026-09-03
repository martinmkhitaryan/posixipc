#include "py_internal.h"

#include <string.h>

typedef struct
{
    PyObject_HEAD PyObject *array;
    uint32_t index;
    _Atomic int locked;
} PosixIPCMutexArrayItemObject;

static int array_is_closed(PosixIPCMutexArrayObject *a)
{
    uint32_t flags = atomic_load_explicit(&a->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static int array_check_open(PosixIPCMutexArrayObject *a, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&a->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_CLOSED) != 0) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || a->locks == NULL || a->region == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static PyObject *array_item_new(PosixIPCMutexArrayObject *a, uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)a);
    PosixIPCMutexArrayItemObject *item;

    if (st == NULL || st->MutexArrayItem_Type == NULL) {
        return NULL;
    }
    item = (PosixIPCMutexArrayItemObject *)st->MutexArrayItem_Type->tp_alloc(st->MutexArrayItem_Type, 0);
    if (item == NULL) {
        return NULL;
    }
    item->array = (PyObject *)a;
    Py_INCREF(a);
    item->index = index;
    atomic_store_explicit(&item->locked, 0, memory_order_relaxed);
    return (PyObject *)item;
}

static PyObject *array_after_lock(PosixIPCMutexArrayObject *a, uint32_t index, int rc,
                                  PosixIPCMutexArrayItemObject *item)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)a);
    PyObject *fn;
    PyObject *res;
    PyObject *cb;
    int urc;
    pthread_mutex_t *lock;

    if (st == NULL) {
        return NULL;
    }
    lock = a->locks[index];
    if (rc == 0) {
        if (item != NULL) {
            atomic_store_explicit(&item->locked, 1, memory_order_release);
        }
        Py_RETURN_TRUE;
    }
    if (rc == EOWNERDEAD) {
        fn = a->on_owner_died;
        if (fn == NULL) {
            posixipc_mutex_unlock(lock);
            PyErr_SetString(PyExc_TypeError, "RobustMutex has no on_owner_died; "
                                             "assign it or rebuild the Layout");
            return NULL;
        }
        cb = (PyObject *)item;
        if (cb == NULL) {
            cb = array_item_new(a, index);
            if (cb == NULL) {
                posixipc_mutex_unlock(lock);
                return NULL;
            }
        } else {
            Py_INCREF(cb);
        }
        Py_INCREF(fn);
        res = PyObject_CallOneArg(fn, cb);
        Py_DECREF(fn);
        Py_DECREF(cb);
        if (res == NULL) {
            posixipc_mutex_unlock(lock);
            return NULL;
        }
        Py_DECREF(res);
        urc = posixipc_mutex_consistent(lock);
        if (urc != 0) {
            posixipc_mutex_unlock(lock);
            posixipc_err(st, urc);
            return NULL;
        }
        if (item != NULL) {
            atomic_store_explicit(&item->locked, 1, memory_order_release);
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

static int array_lock_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_mutex_lock_until((pthread_mutex_t *)arg, slice);
}

static PyObject *array_lock_index(PosixIPCMutexArrayObject *a, uint32_t index, const posixipc_acquire_opts *opts,
                                  PosixIPCMutexArrayItemObject *item)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)a);
    pthread_mutex_t *lock;
    int rc;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;

    if (st == NULL || array_check_open(a, st) < 0) {
        return NULL;
    }
    if (index >= a->count) {
        PyErr_SetString(PyExc_IndexError, "MutexArray index out of range");
        return NULL;
    }
    lock = a->locks[index];
    rc = posixipc_mutex_trylock(lock);
    if (rc != EBUSY) {
        return array_after_lock(a, index, rc, item);
    }
    if (!opts->blocking || (!opts->timeout_none && opts->timeout == 0.0)) {
        Py_RETURN_FALSE;
    }
    if (!opts->timeout_none) {
        rc = posixipc_deadline_from_seconds(posixipc_mutex_clock(), opts->timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts->interruptible && opts->timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mutex_lock(lock);
        Py_END_ALLOW_THREADS return array_after_lock(a, index, rc, item);
    }
    rc = posixipc_blocking_wait(array_lock_until_cb, lock, userp, opts->interruptible, posixipc_mutex_clock());
    return array_after_lock(a, index, rc, item);
}

static int array_parse_index(PyObject *obj, uint32_t *out)
{
    Py_ssize_t v = PyNumber_AsSsize_t(obj, PyExc_IndexError);

    if (v == -1 && PyErr_Occurred()) {
        return -1;
    }
    if (v < 0) {
        PyErr_SetString(PyExc_IndexError, "MutexArray index out of range");
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static PyObject *array_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    posixipc_acquire_opts opts;
    uint32_t index;

    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "acquire() missing index");
        return NULL;
    }
    if (array_parse_index(args[0], &index) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args + 1, nargs - 1, kwnames, &opts) < 0) {
        return NULL;
    }
    return array_lock_index(a, index, &opts, NULL);
}

static PyObject *array_release_index(PosixIPCMutexArrayObject *a, uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)a);
    int rc;

    if (st == NULL || array_check_open(a, st) < 0) {
        return NULL;
    }
    if (index >= a->count) {
        PyErr_SetString(PyExc_IndexError, "MutexArray index out of range");
        return NULL;
    }
    rc = posixipc_mutex_unlock(a->locks[index]);
    if (rc == EPERM) {
        PyErr_SetString(PyExc_RuntimeError, "release unlocked lock");
        return NULL;
    }
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *array_release(PyObject *self, PyObject *args)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    PyObject *idx;
    uint32_t index;

    if (!PyArg_ParseTuple(args, "O:release", &idx)) {
        return NULL;
    }
    if (array_parse_index(idx, &index) < 0) {
        return NULL;
    }
    return array_release_index(a, index);
}

static PyObject *array_as_capsule(PyObject *self, PyObject *args)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *idx;
    uint32_t index;

    if (st == NULL || array_check_open(a, st) < 0) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "O:as_capsule", &idx)) {
        return NULL;
    }
    if (array_parse_index(idx, &index) < 0) {
        return NULL;
    }
    if (index >= a->count) {
        PyErr_SetString(PyExc_IndexError, "MutexArray index out of range");
        return NULL;
    }
    return posixipc_mutex_capsule_new((PosixIPCSharedMemoryObject *)a->region, a->locks[index]);
}

static PyObject *array_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    if (array_is_closed(a)) {
        Py_RETURN_NONE;
    }
    if (a->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)a->region);
        Py_CLEAR(a->region);
    }
    PyMem_Free(a->locks);
    a->locks = NULL;
    atomic_fetch_or_explicit(&a->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *array_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc handles cannot be copied");
    return NULL;
}

static PyObject *array_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t flags = atomic_load_explicit(&a->flags, memory_order_acquire);
    PosixIPCSharedMemoryObject *r;
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;
    uint32_t digest;

    if (st == NULL) {
        return NULL;
    }
    if (array_is_closed(a)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if ((flags & POSIXIPC_FLAG_PROCESS_SHARED) == 0 || (flags & POSIXIPC_FLAG_BOUND) == 0 || a->region == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot pickle a process-private or unbound handle");
        return NULL;
    }
    r = (PosixIPCSharedMemoryObject *)a->region;
    if (r->core.hdr == NULL || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    digest = r->core.hdr->layout_digest;
    mod = PyType_GetModule(st->MutexArray_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_array");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(sIIII)", r->core.name, a->first_slot, a->count, (unsigned)a->kind, digest);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static Py_ssize_t array_len(PyObject *self)
{
    return (Py_ssize_t)((PosixIPCMutexArrayObject *)self)->count;
}

static PyObject *array_getitem(PyObject *self, Py_ssize_t i)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (st == NULL || array_check_open(a, st) < 0) {
        return NULL;
    }
    if (i < 0 || (uint32_t)i >= a->count) {
        PyErr_SetString(PyExc_IndexError, "MutexArray index out of range");
        return NULL;
    }
    return array_item_new(a, (uint32_t)i);
}

static PyObject *array_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    uint32_t flags = atomic_load_explicit(&a->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_PROCESS_SHARED) != 0);
}

static PyObject *array_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    uint32_t flags = atomic_load_explicit(&a->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_BOUND) != 0);
}

static PyObject *array_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(array_is_closed((PosixIPCMutexArrayObject *)self));
}

static PyObject *array_get_region(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    if (a->region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(a->region);
}

static PyObject *array_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexArrayObject *)self)->first_slot);
}

static PyObject *array_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexArrayObject *)self)->kind);
}

static PyObject *array_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    PosixIPCSharedMemoryObject *r;

    if (a->region == NULL) {
        Py_RETURN_NONE;
    }
    r = (PosixIPCSharedMemoryObject *)a->region;
    if (r->core.hdr == NULL) {
        Py_RETURN_NONE;
    }
    return PyLong_FromUnsignedLong(r->core.hdr->layout_digest);
}

static PyObject *array_get_on_owner_died(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    if (a->on_owner_died == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(a->on_owner_died);
}

static int array_set_on_owner_died(PyObject *self, PyObject *value, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    if (value == NULL || value == Py_None) {
        Py_CLEAR(a->on_owner_died);
        return 0;
    }
    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "on_owner_died must be callable");
        return -1;
    }
    Py_INCREF(value);
    Py_XSETREF(a->on_owner_died, value);
    return 0;
}

static int array_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(a->region);
    Py_VISIT(a->on_owner_died);
    return 0;
}

static int array_clear(PyObject *self)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;

    Py_CLEAR(a->on_owner_died);
    return 0;
}

static void array_dealloc(PyObject *self)
{
    PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_GC_UnTrack(self);
    if (!array_is_closed(a)) {
        PyObject *r = array_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(a->on_owner_died);
    Py_CLEAR(a->region);
    PyMem_Free(a->locks);
    a->locks = NULL;
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyObject *item_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;
    posixipc_acquire_opts opts;

    if (item->array == NULL) {
        return posixipc_raise_unbound(), NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    return array_lock_index((PosixIPCMutexArrayObject *)item->array, item->index, &opts, item);
}

static PyObject *item_release(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;
    PyObject *res;

    if (item->array == NULL) {
        return posixipc_raise_unbound(), NULL;
    }
    res = array_release_index((PosixIPCMutexArrayObject *)item->array, item->index);
    if (res != NULL) {
        atomic_store_explicit(&item->locked, 0, memory_order_release);
    }
    return res;
}

static PyObject *item_as_capsule(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;
    PosixIPCMutexArrayObject *a;
    posixipc_state *st = posixipc_state_from_obj(self);

    if (item->array == NULL) {
        return posixipc_raise_unbound(), NULL;
    }
    a = (PosixIPCMutexArrayObject *)item->array;
    if (st == NULL || array_check_open(a, st) < 0) {
        return NULL;
    }
    return posixipc_mutex_capsule_new((PosixIPCSharedMemoryObject *)a->region, a->locks[item->index]);
}

static PyObject *item_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *ok = item_acquire(self, NULL, 0, NULL);

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

static PyObject *item_exit(PyObject *self, PyObject *Py_UNUSED(args))
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;

    if (atomic_load_explicit(&item->locked, memory_order_acquire)) {
        return item_release(self, NULL);
    }
    Py_RETURN_NONE;
}

static PyObject *item_get_index(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCMutexArrayItemObject *)self)->index);
}

static PyObject *item_get_array(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;

    if (item->array == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(item->array);
}

static int item_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(item->array);
    return 0;
}

static int item_clear(PyObject *self)
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;

    Py_CLEAR(item->array);
    return 0;
}

static void item_dealloc(PyObject *self)
{
    PosixIPCMutexArrayItemObject *item = (PosixIPCMutexArrayItemObject *)self;
    PyTypeObject *tp = Py_TYPE(self);

    PyObject_GC_UnTrack(self);
    Py_CLEAR(item->array);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef array_methods[] = {
    {"acquire", POSIXIPC_METH(array_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", array_release, METH_VARARGS, NULL},
    {"close", array_close, METH_NOARGS, NULL},
    {"as_capsule", array_as_capsule, METH_VARARGS, NULL},
    {"__reduce__", array_reduce, METH_NOARGS, NULL},
    {"__copy__", array_copy, METH_NOARGS, NULL},
    {"__deepcopy__", array_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef array_getset[] = {
    {"on_owner_died", array_get_on_owner_died, array_set_on_owner_died, NULL, NULL},
    {"process_shared", array_get_process_shared, NULL, NULL, NULL},
    {"bound", array_get_bound, NULL, NULL, NULL},
    {"closed", array_get_closed, NULL, NULL, NULL},
    {"region", array_get_region, NULL, NULL, NULL},
    {"slot", array_get_slot, NULL, NULL, NULL},
    {"kind", array_get_kind, NULL, NULL, NULL},
    {"digest", array_get_digest, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot array_slots[] = {
    {Py_tp_dealloc, array_dealloc}, {Py_tp_traverse, array_traverse}, {Py_tp_clear, array_clear},
    {Py_tp_methods, array_methods}, {Py_tp_getset, array_getset},     {Py_sq_length, array_len},
    {Py_sq_item, array_getitem},    {Py_tp_new, PyType_GenericNew},   {0, NULL},
};

static PyType_Spec array_spec = {
    .name = "posixipc.MutexArray",
    .basicsize = sizeof(PosixIPCMutexArrayObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = array_slots,
};

static PyMethodDef item_methods[] = {
    {"acquire", POSIXIPC_METH(item_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", item_release, METH_NOARGS, NULL},
    {"as_capsule", item_as_capsule, METH_NOARGS, NULL},
    {"__enter__", item_enter, METH_NOARGS, NULL},
    {"__exit__", item_exit, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef item_getset[] = {
    {"index", item_get_index, NULL, NULL, NULL},
    {"array", item_get_array, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot item_slots[] = {
    {Py_tp_dealloc, item_dealloc},
    {Py_tp_traverse, item_traverse},
    {Py_tp_clear, item_clear},
    {Py_tp_methods, item_methods},
    {Py_tp_getset, item_getset},
    {Py_tp_new, PyType_GenericNew},
    {0, NULL},
};

static PyType_Spec item_spec = {
    .name = "posixipc.MutexArrayItem",
    .basicsize = sizeof(PosixIPCMutexArrayItemObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = item_slots,
};

PyObject *posixipc_mutexarray_new_unbound(posixipc_state *st, uint16_t kind, uint32_t init_flags,
                                          PyObject *on_owner_died, uint32_t first_slot, uint32_t count)
{
    PosixIPCMutexArrayObject *a;

    if (st->MutexArray_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "MutexArray type is missing");
        return NULL;
    }
    a = (PosixIPCMutexArrayObject *)st->MutexArray_Type->tp_alloc(st->MutexArray_Type, 0);
    if (a == NULL) {
        return NULL;
    }
    a->locks = NULL;
    a->count = count;
    a->first_slot = first_slot;
    a->kind = kind;
    a->align = (uint16_t)posixipc_kind_align(kind);
    a->offset = 0;
    a->size = posixipc_kind_size(kind, 0);
    a->init_flags = init_flags;
    a->region = NULL;
    a->on_owner_died = on_owner_died;
    Py_XINCREF(on_owner_died);
    atomic_store_explicit(&a->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)a;
}

int posixipc_mutexarray_bind(PosixIPCMutexArrayObject *a, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                             uint32_t first, uint32_t count)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)a);
    pthread_mutex_t **locks;
    uint32_t i;
    int rc;
    void *ptr;

    if (st == NULL) {
        return -1;
    }
    if (count == 0 || a->count != count) {
        PyErr_SetString(PyExc_ValueError, "MutexArray slot count mismatch");
        return -1;
    }
    locks = (pthread_mutex_t **)PyMem_Calloc(count, sizeof(*locks));
    if (locks == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (i = 0; i < count; i++) {
        const posixipc_slot *slot = &slots[first + i];

        if (slot->kind != a->kind) {
            PyMem_Free(locks);
            PyErr_SetString(st->exc_LayoutMismatchError, "MutexArray slot kind does not match");
            return -1;
        }
        rc = posixipc_shm_offset_ptr(&r->core, slot->offset, slot->size, slot->align, &ptr);
        if (rc != 0) {
            PyMem_Free(locks);
            posixipc_err(st, rc);
            return -1;
        }
        locks[i] = (pthread_mutex_t *)ptr;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        PyMem_Free(locks);
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(a->region, (PyObject *)r);
    PyMem_Free(a->locks);
    a->locks = locks;
    a->first_slot = first;
    a->count = count;
    a->align = slots[first].align;
    a->offset = slots[first].offset;
    a->size = slots[first].size;
    a->init_flags = slots[first].init_flags;
    atomic_fetch_or_explicit(&a->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

int posixipc_array_register(PyObject *mod, posixipc_state *st)
{
    PyObject *array_type;
    PyObject *item_type;

    array_type = PyType_FromModuleAndSpec(mod, &array_spec, NULL);
    if (array_type == NULL) {
        return -1;
    }
    st->MutexArray_Type = (PyTypeObject *)array_type;
    if (PyModule_AddObjectRef(mod, "MutexArray", array_type) < 0) {
        return -1;
    }
    item_type = PyType_FromModuleAndSpec(mod, &item_spec, NULL);
    if (item_type == NULL) {
        return -1;
    }
    st->MutexArrayItem_Type = (PyTypeObject *)item_type;
    if (PyModule_AddObjectRef(mod, "MutexArrayItem", item_type) < 0) {
        return -1;
    }
    return 0;
}
