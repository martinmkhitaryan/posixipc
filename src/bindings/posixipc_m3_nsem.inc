static int nsem_check_open(PosixIPCNamedSemObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        return posixipc_raise_closed(st);
    }
    if (o->sem == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static int nsem_until_cb(void *arg, const posixipc_deadline *slice)
{
    return posixipc_sem_wait_until((sem_t *)arg, slice);
}

static PyObject *nsem_from_sem(posixipc_state *st, sem_t *s, const char *name)
{
    PosixIPCNamedSemObject *o;
    char *dup;

    o = (PosixIPCNamedSemObject *)st->NamedSemaphore_Type->tp_alloc(st->NamedSemaphore_Type, 0);
    if (o == NULL) {
        (void)posixipc_nsem_close(s);
        return NULL;
    }
    dup = strdup(name);
    if (dup == NULL) {
        (void)posixipc_nsem_close(s);
        Py_DECREF(o);
        return PyErr_NoMemory();
    }
    o->sem = s;
    o->name = dup;
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_BOUND, memory_order_release);
    return (PyObject *)o;
}

static PyObject *nsem_create(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    static char *kwlist[] = {"name", "value", NULL};
    const char *name;
    int value = 1;
    sem_t *s = NULL;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|i:create", kwlist, &name, &value)) {
        return NULL;
    }
    if (value < 0) {
        PyErr_SetString(PyExc_ValueError, "value must be non-negative");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_nsem_create(name, (unsigned)value, &s);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return nsem_from_sem(st, s, name);
}

static PyObject *nsem_attach(PyObject *cls, PyObject *args)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    sem_t *s = NULL;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "s:attach", &name)) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_nsem_attach(name, &s);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return nsem_from_sem(st, s, name);
}

static PyObject *nsem_unlink_name(PyObject *cls, PyObject *args)
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
    rc = posixipc_nsem_unlink(name);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *nsem_acquire(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    int rc;
    sem_t *sem;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;

    if (st == NULL || nsem_check_open(o, st) < 0) {
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
    rc = posixipc_blocking_wait(nsem_until_cb, sem, userp, opts.interruptible, posixipc_sem_clock());
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

static PyObject *nsem_release(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    static char *kwlist[] = {"n", NULL};
    int n = 1;
    int i;
    int rc;

    if (st == NULL || nsem_check_open(o, st) < 0) {
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

static PyObject *nsem_enter(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *ok = nsem_acquire(self, NULL, 0, NULL);

    if (ok == NULL) {
        return NULL;
    }
    if (ok == Py_False) {
        Py_DECREF(ok);
        PyErr_SetString(PyExc_RuntimeError, "failed to acquire named semaphore");
        return NULL;
    }
    Py_DECREF(ok);
    return Py_NewRef(self);
}

static PyObject *nsem_exit(PyObject *self, PyObject *Py_UNUSED(args))
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL || nsem_check_open(o, st) < 0) {
        return NULL;
    }
    rc = posixipc_sem_post(o->sem);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *nsem_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (flags_closed(flags)) {
        Py_RETURN_NONE;
    }
    if (o->sem != NULL) {
        (void)posixipc_nsem_close(o->sem);
        o->sem = NULL;
    }
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *nsem_unlink(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (o->name == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "named semaphore has no name");
        return NULL;
    }
    rc = posixipc_nsem_unlink(o->name);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *nsem_get_name(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;

    if (o->name == NULL) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(o->name);
}

static PyObject *nsem_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return handle_flag_bool(&((PosixIPCNamedSemObject *)self)->flags, POSIXIPC_FLAG_CLOSED);
}

static int nsem_init(PyObject *self, PyObject *args, PyObject *kw)
{
    (void)self;
    (void)args;
    (void)kw;
    PyErr_SetString(PyExc_TypeError, "NamedSemaphore: use create() or attach()");
    return -1;
}

static int nsem_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static int nsem_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void nsem_finalize(PyObject *self)
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if (!flags_closed(flags) && o->sem != NULL) {
        if (PyErr_WarnEx(PyExc_ResourceWarning, "unclosed posixipc.NamedSemaphore", 1) < 0) {
            PyErr_WriteUnraisable(self);
        }
    }
}

static void nsem_dealloc(PyObject *self)
{
    PosixIPCNamedSemObject *o = (PosixIPCNamedSemObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    uint32_t flags;
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_CallFinalizerFromDealloc(self);
    flags = atomic_load_explicit(&o->flags, memory_order_acquire);
    PyObject_GC_UnTrack(self);
    if (!flags_closed(flags)) {
        PyObject *r = nsem_close(self, NULL);

        Py_XDECREF(r);
        PyErr_Clear();
    }
    free(o->name);
    o->name = NULL;
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyGetSetDef nsem_getset[] = {
    {"name", nsem_get_name, NULL, NULL, NULL},
    {"closed", nsem_get_closed, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef nsem_methods[] = {
    {"create", POSIXIPC_METH(nsem_create), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"attach", nsem_attach, METH_VARARGS | METH_CLASS, NULL},
    {"unlink_name", nsem_unlink_name, METH_VARARGS | METH_CLASS, NULL},
    {"acquire", POSIXIPC_METH(nsem_acquire), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"release", POSIXIPC_METH(nsem_release), METH_VARARGS | METH_KEYWORDS, NULL},
    {"close", nsem_close, METH_NOARGS, NULL},
    {"unlink", nsem_unlink, METH_NOARGS, NULL},
    {"__enter__", nsem_enter, METH_NOARGS, NULL},
    {"__exit__", nsem_exit, METH_VARARGS, NULL},
    {"__copy__", copy_denied, METH_NOARGS, NULL},
    {"__deepcopy__", copy_denied, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot nsem_slots[] = {
    {Py_tp_dealloc, nsem_dealloc},   {Py_tp_traverse, nsem_traverse}, {Py_tp_clear, nsem_clear},
    {Py_tp_finalize, nsem_finalize}, {Py_tp_init, nsem_init},         {Py_tp_methods, nsem_methods},
    {Py_tp_getset, nsem_getset},     {Py_tp_new, PyType_GenericNew},  {0, NULL},
};

static PyType_Spec nsem_spec = {
    .name = "posixipc.NamedSemaphore",
    .basicsize = sizeof(PosixIPCNamedSemObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = nsem_slots,
};
