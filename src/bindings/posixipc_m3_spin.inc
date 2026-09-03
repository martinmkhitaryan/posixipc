static const char spin_doc[] = "posixipc.SpinLock: pthread_spinlock_t via trylock+yield with the GIL "
                               "released. A Python thread that spins while holding the GIL can deadlock "
                               "the interpreter; this implementation does not.";

static int spin_check_open(PosixIPCSpinLockObject *o, posixipc_state *st)
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

static PyObject *spin_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    int rc;
    pthread_spinlock_t *lock;

    if (st == NULL || spin_check_open(o, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    lock = o->lock;
    rc = posixipc_spin_trylock(lock);
    if (rc == 0) {
        Py_RETURN_TRUE;
    }
    if (rc != EBUSY) {
        posixipc_err(st, rc);
        return NULL;
    }
    if (!opts.blocking) {
        Py_RETURN_FALSE;
    }
    if (!opts.timeout_none) {
        PyErr_SetString(PyExc_ValueError, "SpinLock.acquire() has no timeout");
        return NULL;
    }
    for (;;) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_spin_trylock(lock);
        if (rc == EBUSY) {
            sched_yield();
        }
        Py_END_ALLOW_THREADS if (rc == 0)
        {
            Py_RETURN_TRUE;
        }
        if (rc != EBUSY) {
            posixipc_err(st, rc);
            return NULL;
        }
        if (opts.interruptible && PyErr_CheckSignals() < 0) {
            return NULL;
        }
    }
}

static PyObject *spin_release(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || spin_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_spin_unlock(o->lock);
    if (rc == EPERM) {
        PyErr_SetString(PyExc_RuntimeError, "release unlocked spinlock");
        return NULL;
    }
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *spin_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *ok = spin_acquire(self, NULL, 0, NULL);

    if (ok == NULL) {
        return NULL;
    }
    if (ok == Py_False) {
        Py_DECREF(ok);
        PyErr_SetString(PyExc_RuntimeError, "failed to acquire spinlock");
        return NULL;
    }
    Py_DECREF(ok);
    return Py_NewRef(self);
}

static PyObject *spin_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && o->lock != NULL) {
        (void)posixipc_spin_destroy(o->lock);
    }
    if (o->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)o->region);
        Py_CLEAR(o->region);
    }
    o->lock = NULL;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *spin_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PosixIPCSharedMemoryObject *r;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (st == NULL || shared_reduce_check(flags, o->region, st, &r) < 0) {
        return NULL;
    }
    return reduce_shared((PyObject *)Py_TYPE(self), r->core.name, o->slot, o->kind, r->core.hdr->layout_digest);
}

static int spin_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"process_shared", NULL};
    int process_shared = 0;
    posixipc_spin_config cfg;
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
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|$p:SpinLock", kwlist, &process_shared)) {
        return -1;
    }
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared spinlock requires Layout.add()");
        return -1;
    }
    cfg.flags = 0;
    rc = posixipc_spin_init(&o->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    o->lock = &o->inline_storage;
    o->kind = POSIXIPC_KIND_SPIN;
    o->align = (uint16_t)posixipc_kind_align(o->kind);
    o->size = posixipc_kind_size(o->kind, 0);
    o->init_flags = POSIXIPC_FLAG_OWNS_STORAGE;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND, memory_order_release);
    return 0;
}

static int spin_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->region);
    return 0;
}

static int spin_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void spin_finalize(PyObject *self)
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.SpinLock", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void spin_dealloc(PyObject *self)
{
    PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = spin_close(self, NULL);

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

static PyObject *spin_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSpinLockObject *)self)->flags, POSIXIPC_FLAG_PROCESS_SHARED);
}

static PyObject *spin_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSpinLockObject *)self)->flags, POSIXIPC_FLAG_BOUND);
}

static PyObject *spin_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCSpinLockObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static PyObject *spin_get_region(PyObject *self, void *Py_UNUSED(c))
{
    return handle_region(((PosixIPCSpinLockObject *)self)->region);
}

static PyObject *spin_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSpinLockObject *)self)->slot);
}

static PyObject *spin_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSpinLockObject *)self)->kind);
}

static PyObject *spin_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    return handle_digest(((PosixIPCSpinLockObject *)self)->region);
}

static PyObject *spin_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSpinLockObject *)self)->offset);
}

static PyObject *spin_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCSpinLockObject *)self)->size);
}

static PyGetSetDef spin_getset[] = {
    {"process_shared", spin_get_process_shared, NULL, NULL, NULL},
    {"bound", spin_get_bound, NULL, NULL, NULL},
    {"closed", spin_get_closed, NULL, NULL, NULL},
    {"region", spin_get_region, NULL, NULL, NULL},
    {"slot", spin_get_slot, NULL, NULL, NULL},
    {"kind", spin_get_kind, NULL, NULL, NULL},
    {"digest", spin_get_digest, NULL, NULL, NULL},
    {"offset", spin_get_offset, NULL, NULL, NULL},
    {"size", spin_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef spin_methods[] = {
    {"acquire", POSIXIPC_METH(spin_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", spin_release, METH_NOARGS, NULL},
    {"close", spin_close, METH_NOARGS, NULL},
    {"__enter__", spin_enter, METH_NOARGS, NULL},
    {"__exit__", spin_release, METH_VARARGS, NULL},
    {"__reduce__", spin_reduce, METH_NOARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot spin_slots[] = {
    {Py_tp_dealloc, spin_dealloc},  {Py_tp_traverse, spin_traverse},
    {Py_tp_clear, spin_clear},      {Py_tp_finalize, spin_finalize},
    {Py_tp_init, spin_init},        {Py_tp_methods, spin_methods},
    {Py_tp_getset, spin_getset},    {Py_tp_doc, (void *)spin_doc},
    {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec spin_spec = {
    .name = "posixipc.SpinLock",
    .basicsize = sizeof(PosixIPCSpinLockObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = spin_slots,
};

PyObject *posixipc_spin_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot)
{
    PosixIPCSpinLockObject *o;

    o = (PosixIPCSpinLockObject *)st->SpinLock_Type->tp_alloc(st->SpinLock_Type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->lock = NULL;
    o->region = NULL;
    o->slot = slot;
    o->kind = POSIXIPC_KIND_SPIN;
    o->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_SPIN);
    o->size = posixipc_kind_size(POSIXIPC_KIND_SPIN, 0);
    o->init_flags = init_flags;
    atomic_store_explicit(&o->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

int posixipc_spin_bind(PosixIPCSpinLockObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
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
    if (pin_region(r) < 0) {
        return -1;
    }
    Py_XSETREF(o->region, (PyObject *)r);
    o->lock = (pthread_spinlock_t *)ptr;
    o->slot = index;
    o->kind = slot->kind;
    o->align = slot->align;
    o->offset = slot->offset;
    o->size = slot->size;
    o->init_flags = slot->init_flags;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}
