static int bar_check_open(PosixIPCBarrierObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if ((flags & POSIXIPC_FLAG_BOUND) == 0 || o->bar == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static PyObject *bar_wait(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || bar_check_open(o, st) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_barrier_wait(o->bar);
    Py_END_ALLOW_THREADS if (rc == POSIXIPC_BARRIER_SERIAL)
    {
        Py_RETURN_TRUE;
    }
    if (rc == 0) {
        Py_RETURN_FALSE;
    }
    posixipc_err(st, rc);
    return NULL;
}

static PyObject *bar_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && o->bar != NULL) {
        (void)posixipc_barrier_destroy(o->bar);
    }
    if (o->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)o->region);
        Py_CLEAR(o->region);
    }
    o->bar = NULL;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *bar_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PosixIPCSharedMemoryObject *r;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (st == NULL || shared_reduce_check(flags, o->region, st, &r) < 0) {
        return NULL;
    }
    return reduce_shared((PyObject *)Py_TYPE(self), r->core.name, o->slot, o->kind, r->core.hdr->layout_digest);
}

static int bar_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"parties", "process_shared", NULL};
    Py_ssize_t parties = -1;
    int process_shared = 0;
    posixipc_barrier_config cfg;
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
    if (!PyArg_ParseTupleAndKeywords(args, kw, "n|$p:Barrier", kwlist, &parties, &process_shared)) {
        return -1;
    }
    if (process_shared) {
        PyErr_SetString(PyExc_ValueError, "process-shared barrier requires Layout.add()");
        return -1;
    }
    if (parties < 1 || parties > 0xffff) {
        PyErr_SetString(PyExc_ValueError, "parties out of range");
        return -1;
    }
    cfg.flags = 0;
    cfg.parties = (unsigned)parties;
    rc = posixipc_barrier_init(&o->inline_storage, &cfg);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    o->bar = &o->inline_storage;
    o->kind = POSIXIPC_KIND_BARRIER;
    o->align = (uint16_t)posixipc_kind_align(o->kind);
    o->size = posixipc_kind_size(o->kind, 0);
    o->parties = (unsigned)parties;
    o->init_flags = POSIXIPC_FLAG_OWNS_STORAGE;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_OWNS_STORAGE | POSIXIPC_FLAG_BOUND, memory_order_release);
    return 0;
}

static int bar_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->region);
    return 0;
}

static int bar_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void bar_finalize(PyObject *self)
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_OWNS_STORAGE) != 0 && !flags_closed(flags)) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.Barrier", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void bar_dealloc(PyObject *self)
{
    PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = bar_close(self, NULL);

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

static PyObject *bar_get_process_shared(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCBarrierObject *)self)->flags, POSIXIPC_FLAG_PROCESS_SHARED);
}

static PyObject *bar_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCBarrierObject *)self)->flags, POSIXIPC_FLAG_BOUND);
}

static PyObject *bar_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCBarrierObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static PyObject *bar_get_region(PyObject *self, void *Py_UNUSED(c))
{
    return handle_region(((PosixIPCBarrierObject *)self)->region);
}

static PyObject *bar_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBarrierObject *)self)->slot);
}

static PyObject *bar_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBarrierObject *)self)->kind);
}

static PyObject *bar_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    return handle_digest(((PosixIPCBarrierObject *)self)->region);
}

static PyObject *bar_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBarrierObject *)self)->offset);
}

static PyObject *bar_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBarrierObject *)self)->size);
}

static PyGetSetDef bar_getset[] = {
    {"process_shared", bar_get_process_shared, NULL, NULL, NULL},
    {"bound", bar_get_bound, NULL, NULL, NULL},
    {"closed", bar_get_closed, NULL, NULL, NULL},
    {"region", bar_get_region, NULL, NULL, NULL},
    {"slot", bar_get_slot, NULL, NULL, NULL},
    {"kind", bar_get_kind, NULL, NULL, NULL},
    {"digest", bar_get_digest, NULL, NULL, NULL},
    {"offset", bar_get_offset, NULL, NULL, NULL},
    {"size", bar_get_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef bar_methods[] = {
    {"wait", bar_wait, METH_NOARGS, "Wait at the barrier. True for exactly one waiter. Non-interruptible."},
    {"close", bar_close, METH_NOARGS, NULL},
    {"__reduce__", bar_reduce, METH_NOARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot bar_slots[] = {
    {Py_tp_dealloc, bar_dealloc},   {Py_tp_traverse, bar_traverse},
    {Py_tp_clear, bar_clear},       {Py_tp_finalize, bar_finalize},
    {Py_tp_init, bar_init},         {Py_tp_methods, bar_methods},
    {Py_tp_getset, bar_getset},     {Py_tp_doc, "Barrier. wait() is non-interruptible; a crash hangs everyone."},
    {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec bar_spec = {
    .name = "posixipc.Barrier",
    .basicsize = sizeof(PosixIPCBarrierObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = bar_slots,
};

PyObject *posixipc_barrier_new_unbound(posixipc_state *st, uint32_t init_flags, unsigned parties, uint32_t slot)
{
    PosixIPCBarrierObject *o;

    o = (PosixIPCBarrierObject *)st->Barrier_Type->tp_alloc(st->Barrier_Type, 0);
    if (o == NULL) {
        return NULL;
    }
    o->bar = NULL;
    o->region = NULL;
    o->slot = slot;
    o->kind = POSIXIPC_KIND_BARRIER;
    o->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_BARRIER);
    o->size = posixipc_kind_size(POSIXIPC_KIND_BARRIER, 0);
    o->init_flags = init_flags;
    o->parties = parties;
    atomic_store_explicit(&o->flags, init_flags & ~POSIXIPC_FLAG_BOUND, memory_order_relaxed);
    return (PyObject *)o;
}

int posixipc_barrier_bind(PosixIPCBarrierObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
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
    o->bar = (pthread_barrier_t *)ptr;
    o->slot = index;
    o->kind = slot->kind;
    o->align = slot->align;
    o->offset = slot->offset;
    o->size = slot->size;
    o->init_flags = slot->init_flags;
    o->parties = slot->init_flags >> POSIXIPC_SLOT_AUX_SHIFT;
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}
