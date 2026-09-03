#include "py_internal.h"

#include "posixipc_config.h"

#include <errno.h>
#include <string.h>

static int flags_closed(uint32_t flags)
{
    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static PyObject *copy_denied(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc handles cannot be copied");
    return NULL;
}

static PyObject *reduce_shared(PyObject *mod_type, const char *name, uint32_t slot, uint16_t kind, uint32_t digest)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)mod_type);
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;

    if (st == NULL) {
        return NULL;
    }
    mod = PyType_GetModule((PyTypeObject *)mod_type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_slot");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(sIII)", name, slot, (unsigned)kind, digest);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static int shared_reduce_check(uint32_t flags, PyObject *region, posixipc_state *st, PosixIPCSharedMemoryObject **out)
{
    PosixIPCSharedMemoryObject *r;

    if (flags_closed(flags)) {
        posixipc_raise_closed(st);
        return -1;
    }
    if ((flags & POSIXIPC_FLAG_PROCESS_SHARED) == 0 || (flags & POSIXIPC_FLAG_BOUND) == 0 || region == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot pickle a process-private or unbound handle");
        return -1;
    }
    r = (PosixIPCSharedMemoryObject *)region;
    if (r->core.hdr == NULL || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return -1;
    }
    *out = r;
    return 0;
}

static PyObject *handle_flag_bool(_Atomic uint32_t *flags, uint32_t bit)
{
    uint32_t f = atomic_load_explicit(flags, memory_order_acquire);

    return PyBool_FromLong((f & bit) != 0);
}

static PyObject *handle_digest(PyObject *region)
{
    PosixIPCSharedMemoryObject *r;

    if (region == NULL) {
        Py_RETURN_NONE;
    }
    r = (PosixIPCSharedMemoryObject *)region;
    if (r->core.hdr == NULL) {
        Py_RETURN_NONE;
    }
    return PyLong_FromUnsignedLong(r->core.hdr->layout_digest);
}

static PyObject *handle_region(PyObject *region)
{
    if (region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(region);
}

/* --- RWLock ----------------------------------------------------------- */

static int rwlock_check_open(PosixIPCRWLockObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || o->lock == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static int rd_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_rwlock_rdlock_until((pthread_rwlock_t *)arg, slice);
}

static int wr_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_rwlock_wrlock_until((pthread_rwlock_t *)arg, slice);
}

static PyObject *rwlock_result(posixipc_state *st, int rc)
{
    if (rc == 0) {
        Py_RETURN_TRUE;
    }
    if (rc == ETIMEDOUT || rc == EBUSY) {
        Py_RETURN_FALSE;
    }
    posixipc_err(st, rc);
    return NULL;
}

static PyObject *rwlock_acquire_mode(PosixIPCRWLockObject *o, posixipc_acquire_opts *opts, int writer)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)o);
    int rc;
    pthread_rwlock_t *lock;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;

    if (st == NULL || rwlock_check_open(o, st) < 0) {
        return NULL;
    }
    lock = o->lock;
    rc = writer ? posixipc_rwlock_trywrlock(lock) : posixipc_rwlock_tryrdlock(lock);
    if (rc != EBUSY) {
        return rwlock_result(st, rc);
    }
    if (!opts->blocking || (!opts->timeout_none && opts->timeout == 0.0)) {
        Py_RETURN_FALSE;
    }
    if (!opts->timeout_none) {
        rc = posixipc_deadline_from_seconds(posixipc_rwlock_clock(), opts->timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts->interruptible && opts->timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = writer ? posixipc_rwlock_wrlock(lock) : posixipc_rwlock_rdlock(lock);
        Py_END_ALLOW_THREADS return rwlock_result(st, rc);
    }
    rc = posixipc_blocking_wait(writer ? wr_until_cb : rd_until_cb, lock, userp, opts->interruptible,
                                posixipc_rwlock_clock());
    return rwlock_result(st, rc);
}

static PyObject *rwlock_acquire_read(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    posixipc_acquire_opts opts;

    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    return rwlock_acquire_mode((PosixIPCRWLockObject *)self, &opts, 0);
}

static PyObject *rwlock_acquire_write(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    posixipc_acquire_opts opts;

    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    return rwlock_acquire_mode((PosixIPCRWLockObject *)self, &opts, 1);
}

static PyObject *rwlock_release(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || rwlock_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_rwlock_unlock(o->lock);
    if (rc == EPERM) {
        PyErr_SetString(PyExc_RuntimeError, "release unlocked rwlock");
        return NULL;
    }
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *cm_new(PyObject *rwlock, int writer);

static PyObject *rwlock_read(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return cm_new(self, 0);
}

static PyObject *rwlock_write(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    return cm_new(self, 1);
}

static PyObject *rwlock_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && o->lock != NULL) {
        (void)posixipc_rwlock_destroy(o->lock);
    }
    if (o->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)o->region);
        Py_CLEAR(o->region);
    }
    o->lock = NULL;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *rwlock_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PosixIPCSharedMemoryObject *r;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (st == NULL || shared_reduce_check(flags, o->region, st, &r) < 0) {
        return NULL;
    }
    return reduce_shared((PyObject *)Py_TYPE(self), r->core.name, o->slot, o->kind, r->core.hdr->layout_digest);
}

static int rwlock_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"process_shared", NULL};
    int process_shared = 0;
    posixipc_rwlock_config cfg;
    int rc;
    uint32_t flags;

    if (st == NULL) {
        return -1;
    }
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    if ((flags & POSIXIPC_FLAG_BOUND) != 0 || (flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "handle is already initialized");
        return -1;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|$p:RWLock", kwlist, &process_shared)) {
        return -1;
    }
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared rwlock requires Layout.add()");
        return -1;
    }
    cfg.flags = 0;
    rc = posixipc_rwlock_init(&o->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    o->lock = &o->inline_storage;
    o->kind = POSIXIPC_KIND_RWLOCK;
    o->align = (uint16_t)posixipc_kind_align(o->kind);
    o->size = posixipc_kind_size(o->kind, 0);
    o->init_flags = POSIXIPC_FLAG_OWNS_STORAGE;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND, memory_order_release);
    return 0;
}

static int rwlock_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->region);
    return 0;
}

static int rwlock_clear(PyObject *self)
{
    (void)self;
    return 0;
}

static void rwlock_finalize(PyObject *self)
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.RWLock", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void rwlock_dealloc(PyObject *self)
{
    PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = rwlock_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(o->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef rwlock_methods[] = {
    {"acquire_read", POSIXIPC_METH(rwlock_acquire_read), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"acquire_write", POSIXIPC_METH(rwlock_acquire_write), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", rwlock_release, METH_NOARGS, NULL},
    {"read", rwlock_read, METH_NOARGS, NULL},
    {"write", rwlock_write, METH_NOARGS, NULL},
    {"close", rwlock_close, METH_NOARGS, NULL},
    {"__reduce__", rwlock_reduce, METH_NOARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyObject *rwlock_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCRWLockObject *)self)->flags, POSIXIPC_FLAG_PROCESS_SHARED);
}

static PyObject *rwlock_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCRWLockObject *)self)->flags, POSIXIPC_FLAG_BOUND);
}

static PyObject *rwlock_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCRWLockObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static PyObject *rwlock_get_region(PyObject *self, void *Py_UNUSED(c))
{
    return handle_region(((PosixIPCRWLockObject *)self)->region);
}

static PyObject *rwlock_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCRWLockObject *)self)->slot);
}

static PyObject *rwlock_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCRWLockObject *)self)->kind);
}

static PyObject *rwlock_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    return handle_digest(((PosixIPCRWLockObject *)self)->region);
}

static PyObject *rwlock_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCRWLockObject *)self)->offset);
}

static PyObject *rwlock_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCRWLockObject *)self)->size);
}

static PyGetSetDef rwlock_getset[] = {
    {"process_shared", rwlock_get_process_shared, NULL, NULL, NULL},
    {"bound", rwlock_get_bound, NULL, NULL, NULL},
    {"closed", rwlock_get_closed, NULL, NULL, NULL},
    {"region", rwlock_get_region, NULL, NULL, NULL},
    {"slot", rwlock_get_slot, NULL, NULL, NULL},
    {"kind", rwlock_get_kind, NULL, NULL, NULL},
    {"digest", rwlock_get_digest, NULL, NULL, NULL},
    {"offset", rwlock_get_offset, NULL, NULL, NULL},
    {"size", rwlock_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot rwlock_slots[] = {
    {Py_tp_dealloc, rwlock_dealloc},   {Py_tp_traverse, rwlock_traverse}, {Py_tp_clear, rwlock_clear},
    {Py_tp_finalize, rwlock_finalize}, {Py_tp_init, rwlock_init},         {Py_tp_methods, rwlock_methods},
    {Py_tp_getset, rwlock_getset},     {Py_tp_new, PyType_GenericNew},    {0, NULL},
};

static PyType_Spec rwlock_spec = {
    .name = "posixipc.RWLock",
    .basicsize = sizeof(PosixIPCRWLockObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = rwlock_slots,
};

typedef struct
{
    PyObject_HEAD PyObject *rwlock;
    int writer;
    int held;
} PosixIPCRWLockCMObject;

static PyObject *cm_new(PyObject *rwlock, int writer)
{
    posixipc_state *st = posixipc_state_from_obj(rwlock);
    PosixIPCRWLockCMObject *cm;

    if (st == NULL) {
        return NULL;
    }
    cm = (PosixIPCRWLockCMObject *)st->RWLockCM_Type->tp_alloc(st->RWLockCM_Type, 0);
    if (cm == NULL) {
        return NULL;
    }
    cm->rwlock = Py_NewRef(rwlock);
    cm->writer = writer;
    cm->held = 0;
    return (PyObject *)cm;
}

static PyObject *cm_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCRWLockCMObject *cm = (PosixIPCRWLockCMObject *)self;
    posixipc_acquire_opts opts = {.timeout_none = 1, .blocking = 1, .interruptible = 1};
    PyObject *ok = rwlock_acquire_mode((PosixIPCRWLockObject *)cm->rwlock, &opts, cm->writer);

    if (ok == NULL) {
        return NULL;
    }
    if (ok == Py_False) {
        Py_DECREF(ok);
        PyErr_SetString(PyExc_RuntimeError, "failed to acquire rwlock");
        return NULL;
    }
    Py_DECREF(ok);
    cm->held = 1;
    return Py_NewRef(cm->rwlock);
}

static PyObject *cm_exit(PyObject *self, PyObject *Py_UNUSED(args))
{
    PosixIPCRWLockCMObject *cm = (PosixIPCRWLockCMObject *)self;

    if (cm->held) {
        cm->held = 0;
        return rwlock_release(cm->rwlock, NULL);
    }
    Py_RETURN_NONE;
}

static int cm_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCRWLockCMObject *cm = (PosixIPCRWLockCMObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(cm->rwlock);
    return 0;
}

static int cm_clear(PyObject *self)
{
    PosixIPCRWLockCMObject *cm = (PosixIPCRWLockCMObject *)self;

    Py_CLEAR(cm->rwlock);
    return 0;
}

static void cm_dealloc(PyObject *self)
{
    PosixIPCRWLockCMObject *cm = (PosixIPCRWLockCMObject *)self;
    PyTypeObject *tp = Py_TYPE(self);

    PyObject_GC_UnTrack(self);
    if (cm->held && cm->rwlock != NULL) {
        PyObject *r = rwlock_release(cm->rwlock, NULL);

        Py_XDECREF(r);
        PyErr_Clear();
        cm->held = 0;
    }
    Py_CLEAR(cm->rwlock);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef cm_methods[] = {
    {"__enter__", cm_enter, METH_NOARGS, NULL},
    {"__exit__", cm_exit, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot cm_slots[] = {
    {Py_tp_dealloc, cm_dealloc}, {Py_tp_traverse, cm_traverse},  {Py_tp_clear, cm_clear},
    {Py_tp_methods, cm_methods}, {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec cm_spec = {
    .name = "posixipc.RWLockCM",
    .basicsize = sizeof(PosixIPCRWLockCMObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = cm_slots,
};

PyObject *posixipc_rwlock_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot)
{
    PosixIPCRWLockObject *o;

    o = (PosixIPCRWLockObject *)st->RWLock_Type->tp_alloc(st->RWLock_Type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->lock = NULL;
    o->region = NULL;
    o->slot = slot;
    o->kind = POSIXIPC_KIND_RWLOCK;
    o->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_RWLOCK);
    o->size = posixipc_kind_size(POSIXIPC_KIND_RWLOCK, 0);
    o->init_flags = init_flags;
    atomic_store_explicit(&o->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

int posixipc_rwlock_bind(PosixIPCRWLockObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                         uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)o);
    void *ptr;
    int rc;

    if (st == NULL) {
        return -1;
    }
    rc = posixipc_shm_offset_ptr(&r->core, slot->offset, slot->size, slot->align, &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(o->region, (PyObject *)r);
    o->lock = (pthread_rwlock_t *)ptr;
    o->slot = index;
    o->kind = slot->kind;
    o->align = slot->align;
    o->offset = slot->offset;
    o->size = slot->size;
    o->init_flags = slot->init_flags;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

/* --- Condition -------------------------------------------------------- */

typedef struct
{
    pthread_cond_t *cond;
    pthread_mutex_t *mutex;
} cond_pair;

static int cond_until_cb(void *arg, const posixipc_deadline *slice)
{
    cond_pair *p = arg;

    return posixipc_cond_wait_until(p->cond, p->mutex, slice);
}

static int cond_check_open(PosixIPCConditionObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || o->cond == NULL) {
        return posixipc_raise_unbound();
    }
    if (o->mutex == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Condition has no associated mutex");
        return -1;
    }
    return 0;
}

static PyObject *cond_wait(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    PosixIPCMutexObject *m;
    cond_pair pair;
    clockid_t clk;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    int rc;
    PyObject *res;

    if (st == NULL || cond_check_open(o, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    if (!opts.blocking) {
        PyErr_SetString(PyExc_ValueError, "Condition.wait() cannot be non-blocking");
        return NULL;
    }
    m = (PosixIPCMutexObject *)o->mutex;
    if (m->lock == NULL || atomic_load_explicit(&m->locked, memory_order_acquire) == 0) {
        PyErr_SetString(PyExc_RuntimeError, "cannot wait on an un-acquired mutex");
        return NULL;
    }
    pair.cond = o->cond;
    pair.mutex = m->lock;
    clk = posixipc_cond_clock(o->init_flags);
    if (!opts.timeout_none) {
        rc = posixipc_deadline_from_seconds(clk, opts.timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts.interruptible && opts.timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_cond_wait(pair.cond, pair.mutex);
        Py_END_ALLOW_THREADS
    } else {
        rc = posixipc_blocking_wait(cond_until_cb, &pair, userp, opts.interruptible, clk);
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        atomic_store_explicit(&m->locked, 1, memory_order_release);
        return NULL;
    }
    if (rc == ETIMEDOUT) {
        atomic_store_explicit(&m->locked, 1, memory_order_release);
        Py_RETURN_FALSE;
    }
    res = posixipc_mutex_after_lock(m, rc);
    return res;
}

static PyObject *cond_notify(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || cond_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_cond_signal(o->cond);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *cond_notify_all(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || cond_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_cond_broadcast(o->cond);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *cond_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && o->cond != NULL) {
        (void)posixipc_cond_destroy(o->cond);
    }
    if (o->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)o->region);
        Py_CLEAR(o->region);
    }
    Py_CLEAR(o->mutex);
    o->cond = NULL;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *cond_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PosixIPCSharedMemoryObject *r;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (st == NULL || shared_reduce_check(flags, o->region, st, &r) < 0) {
        return NULL;
    }
    return reduce_shared((PyObject *)Py_TYPE(self), r->core.name, o->slot, o->kind, r->core.hdr->layout_digest);
}

static int cond_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"mutex", "process_shared", NULL};
    PyObject *mutex = NULL;
    int process_shared = 0;
    posixipc_cond_config cfg;
    int rc;
    uint32_t flags;

    if (st == NULL) {
        return -1;
    }
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    if ((flags & POSIXIPC_FLAG_BOUND) != 0 || (flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "handle is already initialized");
        return -1;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|$p:Condition", kwlist, &mutex, &process_shared)) {
        return -1;
    }
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared condition requires Layout.add()");
        return -1;
    }
    if (!PyObject_TypeCheck(mutex, st->Mutex_Type)) {
        PyErr_SetString(PyExc_TypeError, "mutex must be a posixipc.Mutex");
        return -1;
    }
    cfg.flags = 0;
#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    cfg.flags |= POSIXIPC_FLAG_MONOTONIC;
#endif
    rc = posixipc_cond_init(&o->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    o->cond = &o->inline_storage;
    o->kind = POSIXIPC_KIND_COND;
    o->align = (uint16_t)posixipc_kind_align(o->kind);
    o->size = posixipc_kind_size(o->kind, 0);
    o->init_flags = cfg.flags | POSIXIPC_FLAG_OWNS_STORAGE;
    Py_INCREF(mutex);
    o->mutex = mutex;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND | cfg.flags,
                          memory_order_release);
    return 0;
}

static int cond_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->region);
    Py_VISIT(o->mutex);
    return 0;
}

static int cond_clear(PyObject *self)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;

    Py_CLEAR(o->mutex);
    return 0;
}

static void cond_finalize(PyObject *self)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.Condition", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void cond_dealloc(PyObject *self)
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = cond_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(o->mutex);
    Py_CLEAR(o->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyObject *cond_get_mutex(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCConditionObject *o = (PosixIPCConditionObject *)self;

    if (o->mutex == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(o->mutex);
}

static PyObject *cond_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCConditionObject *)self)->flags, POSIXIPC_FLAG_PROCESS_SHARED);
}

static PyObject *cond_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCConditionObject *)self)->flags, POSIXIPC_FLAG_BOUND);
}

static PyObject *cond_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCConditionObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static PyObject *cond_get_region(PyObject *self, void *Py_UNUSED(c))
{
    return handle_region(((PosixIPCConditionObject *)self)->region);
}

static PyObject *cond_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCConditionObject *)self)->slot);
}

static PyObject *cond_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCConditionObject *)self)->kind);
}

static PyObject *cond_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    return handle_digest(((PosixIPCConditionObject *)self)->region);
}

static PyObject *cond_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCConditionObject *)self)->offset);
}

static PyObject *cond_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCConditionObject *)self)->size);
}

static PyGetSetDef cond_getset[] = {
    {"mutex", cond_get_mutex, NULL, NULL, NULL},
    {"process_shared", cond_get_process_shared, NULL, NULL, NULL},
    {"bound", cond_get_bound, NULL, NULL, NULL},
    {"closed", cond_get_closed, NULL, NULL, NULL},
    {"region", cond_get_region, NULL, NULL, NULL},
    {"slot", cond_get_slot, NULL, NULL, NULL},
    {"kind", cond_get_kind, NULL, NULL, NULL},
    {"digest", cond_get_digest, NULL, NULL, NULL},
    {"offset", cond_get_offset, NULL, NULL, NULL},
    {"size", cond_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef cond_methods[] = {
    {"wait", POSIXIPC_METH(cond_wait), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"notify", cond_notify, METH_NOARGS, NULL},
    {"notify_all", cond_notify_all, METH_NOARGS, NULL},
    {"close", cond_close, METH_NOARGS, NULL},
    {"__reduce__", cond_reduce, METH_NOARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot cond_slots[] = {
    {Py_tp_dealloc, cond_dealloc},   {Py_tp_traverse, cond_traverse}, {Py_tp_clear, cond_clear},
    {Py_tp_finalize, cond_finalize}, {Py_tp_init, cond_init},         {Py_tp_methods, cond_methods},
    {Py_tp_getset, cond_getset},     {Py_tp_new, PyType_GenericNew},  {0, NULL},
};

static PyType_Spec cond_spec = {
    .name = "posixipc.Condition",
    .basicsize = sizeof(PosixIPCConditionObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = cond_slots,
};

PyObject *posixipc_cond_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot, PyObject *mutex)
{
    PosixIPCConditionObject *o;

    o = (PosixIPCConditionObject *)st->Condition_Type->tp_alloc(st->Condition_Type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->cond = NULL;
    o->region = NULL;
    o->mutex = mutex;
    Py_XINCREF(mutex);
    o->slot = slot;
    o->kind = POSIXIPC_KIND_COND;
    o->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_COND);
    o->size = posixipc_kind_size(POSIXIPC_KIND_COND, 0);
    o->init_flags = init_flags;
    atomic_store_explicit(&o->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

int posixipc_cond_bind(PosixIPCConditionObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                       uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)o);
    void *ptr;
    int rc;

    if (st == NULL) {
        return -1;
    }
    rc = posixipc_shm_offset_ptr(&r->core, slot->offset, slot->size, slot->align, &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(o->region, (PyObject *)r);
    o->cond = (pthread_cond_t *)ptr;
    o->slot = index;
    o->kind = slot->kind;
    o->align = slot->align;
    o->offset = slot->offset;
    o->size = slot->size;
    o->init_flags = slot->init_flags;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

/* --- Semaphore -------------------------------------------------------- */

static int sem_check_open(PosixIPCSemaphoreObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || o->sem == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static int sem_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_sem_wait_until((sem_t *)arg, slice);
}

static PyObject *sem_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    int rc;
    sem_t *sem;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;

    if (st == NULL || sem_check_open(o, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    sem = o->sem;
    rc = posixipc_sem_trywait(sem);
    if (rc != EAGAIN && rc != EBUSY) {
        if (rc == 0) {
            Py_RETURN_TRUE;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    if (!opts.blocking || (!opts.timeout_none && opts.timeout == 0.0)) {
        Py_RETURN_FALSE;
    }
    if (!opts.timeout_none) {
        rc = posixipc_deadline_from_seconds(posixipc_sem_clock(), opts.timeout, &user);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts.interruptible && opts.timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_sem_wait(sem);
        Py_END_ALLOW_THREADS if (rc == 0)
        {
            Py_RETURN_TRUE;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    rc = posixipc_blocking_wait(sem_until_cb, sem, userp, opts.interruptible, posixipc_sem_clock());
    if (rc == 0) {
        Py_RETURN_TRUE;
    }
    if (rc == ETIMEDOUT || rc == EAGAIN) {
        Py_RETURN_FALSE;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        return NULL;
    }
    posixipc_err(st, rc);
    return NULL;
}

static PyObject *sem_release(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"n", NULL};
    int n = 1;
    int i;
    int rc;

    if (st == NULL || sem_check_open(o, st) < 0) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|i:release", kwlist, &n)) {
        return NULL;
    }
    if (n < 1) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 1");
        return NULL;
    }
    for (i = 0; i < n; i++) {
        rc = posixipc_sem_post(o->sem);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *sem_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *ok = sem_acquire(self, NULL, 0, NULL);

    if (ok == NULL) {
        return NULL;
    }
    if (ok == Py_False) {
        Py_DECREF(ok);
        PyErr_SetString(PyExc_RuntimeError, "failed to acquire semaphore");
        return NULL;
    }
    Py_DECREF(ok);
    return Py_NewRef(self);
}

static PyObject *sem_exit(PyObject *self, PyObject *Py_UNUSED(args))
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || sem_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_sem_post(o->sem);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *semobj_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && o->sem != NULL) {
        (void)posixipc_sem_destroy(o->sem);
    }
    if (o->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)o->region);
        Py_CLEAR(o->region);
    }
    o->sem = NULL;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *sem_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PosixIPCSharedMemoryObject *r;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (st == NULL || shared_reduce_check(flags, o->region, st, &r) < 0) {
        return NULL;
    }
    return reduce_shared((PyObject *)Py_TYPE(self), r->core.name, o->slot, o->kind, r->core.hdr->layout_digest);
}

static int semobj_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"value", "process_shared", NULL};
    int value = 1;
    int process_shared = 0;
    posixipc_sem_config cfg;
    int rc;
    uint32_t flags;

    if (st == NULL) {
        return -1;
    }
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    if ((flags & POSIXIPC_FLAG_BOUND) != 0 || (flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "handle is already initialized");
        return -1;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|i$p:Semaphore", kwlist, &value, &process_shared)) {
        return -1;
    }
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared semaphore requires Layout.add()");
        return -1;
    }
    if (value < 0) {
        PyErr_SetString(PyExc_ValueError, "value must be non-negative");
        return -1;
    }
    cfg.flags = 0;
    cfg.value = (unsigned)value;
    rc = posixipc_sem_init(&o->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    o->sem = &o->inline_storage;
    o->kind = POSIXIPC_KIND_SEM;
    o->align = (uint16_t)posixipc_kind_align(o->kind);
    o->size = posixipc_kind_size(o->kind, 0);
    o->value = (unsigned)value;
    o->init_flags = POSIXIPC_FLAG_OWNS_STORAGE;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND, memory_order_release);
    return 0;
}

static int sem_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->region);
    return 0;
}

static int sem_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void sem_finalize(PyObject *self)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.Semaphore", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void sem_dealloc(PyObject *self)
{
    PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = semobj_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(o->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef sem_methods[] = {
    {"acquire", POSIXIPC_METH(sem_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", POSIXIPC_METH(sem_release), METH_VARARGS | METH_KEYWORDS, NULL},
    {"close", semobj_close, METH_NOARGS, NULL},
    {"__enter__", sem_enter, METH_NOARGS, NULL},
    {"__exit__", sem_exit, METH_VARARGS, NULL},
    {"__reduce__", sem_reduce, METH_NOARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyObject *sem_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSemaphoreObject *)self)->flags, POSIXIPC_FLAG_PROCESS_SHARED);
}

static PyObject *sem_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSemaphoreObject *)self)->flags, POSIXIPC_FLAG_BOUND);
}

static PyObject *sem_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSemaphoreObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static PyObject *sem_get_region(PyObject *self, void *Py_UNUSED(c))
{
    return handle_region(((PosixIPCSemaphoreObject *)self)->region);
}

static PyObject *sem_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSemaphoreObject *)self)->slot);
}

static PyObject *sem_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSemaphoreObject *)self)->kind);
}

static PyObject *sem_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    return handle_digest(((PosixIPCSemaphoreObject *)self)->region);
}

static PyObject *sem_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSemaphoreObject *)self)->offset);
}

static PyObject *sem_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSemaphoreObject *)self)->size);
}

static PyGetSetDef sem_getset[] = {
    {"process_shared", sem_get_process_shared, NULL, NULL, NULL},
    {"bound", sem_get_bound, NULL, NULL, NULL},
    {"closed", sem_get_closed, NULL, NULL, NULL},
    {"region", sem_get_region, NULL, NULL, NULL},
    {"slot", sem_get_slot, NULL, NULL, NULL},
    {"kind", sem_get_kind, NULL, NULL, NULL},
    {"digest", sem_get_digest, NULL, NULL, NULL},
    {"offset", sem_get_offset, NULL, NULL, NULL},
    {"size", sem_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot sem_slots[] = {
    {Py_tp_dealloc, sem_dealloc},   {Py_tp_traverse, sem_traverse}, {Py_tp_clear, sem_clear},
    {Py_tp_finalize, sem_finalize}, {Py_tp_init, semobj_init},      {Py_tp_methods, sem_methods},
    {Py_tp_getset, sem_getset},     {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec sem_spec = {
    .name = "posixipc.Semaphore",
    .basicsize = sizeof(PosixIPCSemaphoreObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = sem_slots,
};

PyObject *posixipc_sem_new_unbound(posixipc_state *st, uint32_t init_flags, unsigned value, uint32_t slot)
{
    PosixIPCSemaphoreObject *o;

    o = (PosixIPCSemaphoreObject *)st->Semaphore_Type->tp_alloc(st->Semaphore_Type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->sem = NULL;
    o->region = NULL;
    o->slot = slot;
    o->kind = POSIXIPC_KIND_SEM;
    o->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_SEM);
    o->size = posixipc_kind_size(POSIXIPC_KIND_SEM, 0);
    o->init_flags = init_flags;
    o->value = value;
    atomic_store_explicit(&o->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

int posixipc_sem_bind(PosixIPCSemaphoreObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                      uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)o);
    void *ptr;
    int rc;

    if (st == NULL) {
        return -1;
    }
    rc = posixipc_shm_offset_ptr(&r->core, slot->offset, slot->size, slot->align, &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(o->region, (PyObject *)r);
    o->sem = (sem_t *)ptr;
    o->slot = index;
    o->kind = slot->kind;
    o->align = slot->align;
    o->offset = slot->offset;
    o->size = slot->size;
    o->init_flags = slot->init_flags;
    o->value = slot->init_flags >> POSIXIPC_SLOT_AUX_SHIFT;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

int posixipc_sync_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;

    type = PyType_FromModuleAndSpec(mod, &rwlock_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->RWLock_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "RWLock", type) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &cm_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->RWLockCM_Type = (PyTypeObject *)type;
    type = PyType_FromModuleAndSpec(mod, &cond_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Condition_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Condition", type) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &sem_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Semaphore_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Semaphore", type) < 0) {
        return -1;
    }
    return 0;
}
