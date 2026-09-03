#include "py_internal.h"

#include "posixipc_config.h"
#include "posixipc_queue.h"
#include "posixipc_mq.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

typedef struct
{
    pthread_cond_t *cond;
    pthread_mutex_t *mutex;
} queue_cond_pair;

static int queue_is_closed(PosixIPCQueueObject *q)
{
    uint32_t flags = atomic_load_explicit(&q->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static int queue_check_open(PosixIPCQueueObject *q, posixipc_state *st)
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

static int queue_lock(PosixIPCQueueObject *q, pthread_mutex_t *lock)
{
    int rc = posixipc_mutex_lock(lock);

    if (rc == EOWNERDEAD) {
        rc = posixipc_queue_recover(&q->view);
        if (rc != 0) {
            (void)posixipc_mutex_unlock(lock);
            return rc;
        }
        rc = posixipc_mutex_consistent(lock);
        if (rc != 0) {
            (void)posixipc_mutex_unlock(lock);
            return rc;
        }
        (void)posixipc_cond_broadcast(q->not_full);
        (void)posixipc_cond_broadcast(q->not_empty);
        return 0;
    }
    return rc;
}

static int queue_cond_until_cb(void *arg, const posixipc_deadline *slice)
{
    queue_cond_pair *p = (queue_cond_pair *)arg;

    return posixipc_cond_wait_until(p->cond, p->mutex, slice);
}

static int queue_wait_cond(PosixIPCQueueObject *q, pthread_cond_t *cond, pthread_mutex_t *lock,
                           const posixipc_acquire_opts *opts, posixipc_state *st)
{
    queue_cond_pair pair;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    clockid_t clk = posixipc_cond_clock(q->cond_flags);
    int rc;

    pair.cond = cond;
    pair.mutex = lock;
    if (!opts->timeout_none) {
        rc = posixipc_deadline_from_seconds(clk, opts->timeout, &user);
        if (rc != 0) {
            return rc;
        }
        userp = &user;
    }
    if (!opts->interruptible && opts->timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_cond_wait(cond, lock);
        Py_END_ALLOW_THREADS
    } else {
        rc = posixipc_blocking_wait(queue_cond_until_cb, &pair, userp, opts->interruptible, clk);
    }
    if (rc == EOWNERDEAD) {
        rc = posixipc_queue_recover(&q->view);
        if (rc != 0) {
            return rc;
        }
        rc = posixipc_mutex_consistent(lock);
        if (rc != 0) {
            return rc;
        }
        (void)posixipc_cond_broadcast(q->not_full);
        (void)posixipc_cond_broadcast(q->not_empty);
        return 0;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        posixipc_err(st, rc);
        return POSIXIPC_ERROR_INTERRUPTED;
    }
    return rc;
}

PyObject *posixipc_queue_new_unbound(posixipc_state *st, uint32_t depth, uint32_t item_size, uint32_t first_slot)
{
    PosixIPCQueueObject *q;

    if (st->Queue_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Queue type is missing");
        return NULL;
    }
    q = (PosixIPCQueueObject *)st->Queue_Type->tp_alloc(st->Queue_Type, 0);
    if (q == NULL) {
        return NULL;
    }
    q->put_lock = NULL;
    q->get_lock = NULL;
    q->not_full = NULL;
    q->not_empty = NULL;
    q->region = NULL;
    q->first_slot = first_slot;
    q->depth = depth;
    q->item_size = item_size;
    q->cond_flags = POSIXIPC_FLAG_PROCESS_SHARED;
#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    q->cond_flags |= POSIXIPC_FLAG_MONOTONIC;
#endif
    memset(&q->view, 0, sizeof(q->view));
    atomic_store_explicit(&q->flags, POSIXIPC_FLAG_PROCESS_SHARED, memory_order_relaxed);
    return (PyObject *)q;
}

int posixipc_queue_init_on_shm(posixipc_shm *core, const posixipc_slot *bytes_slot, uint32_t depth, uint32_t item_size)
{
    void *ptr;
    posixipc_queue_view view;
    int rc;

    rc = posixipc_shm_offset_ptr(core, bytes_slot->offset, bytes_slot->size, bytes_slot->align, &ptr);
    if (rc != 0) {
        return rc;
    }
    rc = posixipc_queue_map(ptr, bytes_slot->size, depth, item_size, &view);
    if (rc != 0) {
        return rc;
    }
    return posixipc_queue_init_ctrl(&view);
}

int posixipc_queue_bind(PosixIPCQueueObject *q, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                        uint32_t first)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)q);
    void *ptr;
    int rc;

    if (st == NULL) {
        return -1;
    }
    if (slots[first].kind != POSIXIPC_KIND_ROBUST_MUTEX || slots[first + 1u].kind != POSIXIPC_KIND_ROBUST_MUTEX ||
        slots[first + 2u].kind != POSIXIPC_KIND_COND || slots[first + 3u].kind != POSIXIPC_KIND_COND ||
        slots[first + 4u].kind != POSIXIPC_KIND_BYTES) {
        PyErr_SetString(st->exc_LayoutMismatchError, "Queue slot sequence does not match");
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
    q->not_full = (pthread_cond_t *)ptr;
    rc = posixipc_shm_offset_ptr(&r->core, slots[first + 3u].offset, slots[first + 3u].size, slots[first + 3u].align,
                                 &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    q->not_empty = (pthread_cond_t *)ptr;
    rc = posixipc_shm_offset_ptr(&r->core, slots[first + 4u].offset, slots[first + 4u].size, slots[first + 4u].align,
                                 &ptr);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    rc = posixipc_queue_map(ptr, slots[first + 4u].size, q->depth, q->item_size, &q->view);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    if (q->view.ctrl->depth != q->depth || q->view.ctrl->item_size != q->item_size) {
        PyErr_SetString(st->exc_LayoutMismatchError, "Queue control word does not match layout");
        return -1;
    }
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(q->region, (PyObject *)r);
    q->first_slot = first;
    q->cond_flags = slots[first + 2u].init_flags;
    atomic_fetch_or_explicit(&q->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

static PyObject *queue_put(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    Py_buffer view;
    int rc;
    uint32_t n;

    if (st == NULL || queue_check_open(q, st) < 0) {
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
    Py_BEGIN_ALLOW_THREADS rc = queue_lock(q, q->put_lock);
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
        if (posixipc_queue_qsize(&q->view, &n) != 0) {
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
        rc = queue_wait_cond(q, q->not_full, q->put_lock, &opts, st);
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
    rc = posixipc_queue_put(&q->view, view.buf);
    (void)posixipc_mutex_unlock(q->put_lock);
    PyBuffer_Release(&view);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    (void)posixipc_cond_signal(q->not_empty);
    Py_RETURN_TRUE;
}

static PyObject *queue_get(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    PyObject *out;
    char *buf;
    int rc;
    uint32_t n;

    if (st == NULL || queue_check_open(q, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    buf = (char *)PyMem_Malloc(q->item_size);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    Py_BEGIN_ALLOW_THREADS rc = queue_lock(q, q->put_lock);
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
        if (posixipc_queue_qsize(&q->view, &n) != 0) {
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
        rc = queue_wait_cond(q, q->not_empty, q->put_lock, &opts, st);
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
    rc = posixipc_queue_get(&q->view, buf);
    (void)posixipc_mutex_unlock(q->put_lock);
    if (rc != 0) {
        PyMem_Free(buf);
        posixipc_err(st, rc);
        return NULL;
    }
    (void)posixipc_cond_signal(q->not_full);
    out = PyBytes_FromStringAndSize(buf, (Py_ssize_t)q->item_size);
    PyMem_Free(buf);
    return out;
}

static PyObject *queue_put_nowait(PyObject *self, PyObject *data)
{
    PyObject *args[3];
    PyObject *false_obj = Py_False;
    PyObject *names;
    PyObject *res;

    args[0] = data;
    args[1] = false_obj;
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
        res = queue_put(self, args, 1, tup);
        Py_DECREF(tup);
        return res;
    }
}

static PyObject *queue_get_nowait(PyObject *self, PyObject *Py_UNUSED(ignored))
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
    res = queue_get(self, &false_obj, 0, tup);
    Py_DECREF(tup);
    return res;
}

static PyObject *queue_qsize(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t n;
    int rc;

    if (st == NULL || queue_check_open(q, st) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = queue_lock(q, q->put_lock);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        if (rc == ENOTRECOVERABLE) {
            PyErr_SetString(st->exc_NotRecoverableError, "mutex is permanently unusable");
            return NULL;
        }
        posixipc_err(st, rc);
        return NULL;
    }
    rc = posixipc_queue_qsize(&q->view, &n);
    (void)posixipc_mutex_unlock(q->put_lock);
    if (rc != 0) {
        posixipc_err(st, EINVAL);
        return NULL;
    }
    return PyLong_FromUnsignedLong(n);
}

static PyObject *queue_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;

    if (queue_is_closed(q)) {
        Py_RETURN_NONE;
    }
    if (q->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)q->region);
        Py_CLEAR(q->region);
    }
    q->put_lock = NULL;
    q->get_lock = NULL;
    q->not_full = NULL;
    q->not_empty = NULL;
    atomic_fetch_or_explicit(&q->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *queue_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc handles cannot be copied");
    return NULL;
}

static PyObject *queue_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
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
    if (queue_is_closed(q)) {
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
    mod = PyType_GetModule(st->Queue_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_queue");
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

static PyObject *queue_get_bound(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
    uint32_t flags = atomic_load_explicit(&q->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_BOUND) != 0);
}

static PyObject *queue_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(queue_is_closed((PosixIPCQueueObject *)self));
}

static PyObject *queue_get_region(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;

    if (q->region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(q->region);
}

static PyObject *queue_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCQueueObject *)self)->first_slot);
}

static PyObject *queue_get_depth(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCQueueObject *)self)->depth);
}

static PyObject *queue_get_item_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCQueueObject *)self)->item_size);
}

static int queue_init(PyObject *self, PyObject *args, PyObject *kw)
{
    (void)self;
    (void)args;
    (void)kw;
    PyErr_SetString(PyExc_TypeError, "Queue() cannot be called; use Layout.add(Queue, ...)");
    return -1;
}

static int queue_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(q->region);
    return 0;
}

static int queue_clear(PyObject *self)
{
    (void)self;
    return 0;
}

static void queue_dealloc(PyObject *self)
{
    PosixIPCQueueObject *q = (PosixIPCQueueObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_GC_UnTrack(self);
    if (!queue_is_closed(q)) {
        PyObject *r = queue_close(self, NULL);

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

static PyMethodDef queue_methods[] = {
    {"put", POSIXIPC_METH(queue_put), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"get", POSIXIPC_METH(queue_get), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"put_nowait", queue_put_nowait, METH_O, NULL},
    {"get_nowait", queue_get_nowait, METH_NOARGS, NULL},
    {"qsize", queue_qsize, METH_NOARGS, NULL},
    {"close", queue_close, METH_NOARGS, NULL},
    {"__reduce__", queue_reduce, METH_NOARGS, NULL},
    {"__copy__", queue_copy, METH_NOARGS, NULL},
    {"__deepcopy__", queue_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef queue_getset[] = {
    {"bound", queue_get_bound, NULL, NULL, NULL},
    {"closed", queue_get_closed, NULL, NULL, NULL},
    {"region", queue_get_region, NULL, NULL, NULL},
    {"slot", queue_get_slot, NULL, NULL, NULL},
    {"depth", queue_get_depth, NULL, NULL, NULL},
    {"item_size", queue_get_item_size, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot queue_slots[] = {
    {Py_tp_dealloc, queue_dealloc}, {Py_tp_traverse, queue_traverse},
    {Py_tp_clear, queue_clear},     {Py_tp_init, queue_init},
    {Py_tp_methods, queue_methods}, {Py_tp_getset, queue_getset},
    {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec queue_spec = {
    .name = "posixipc.Queue",
    .basicsize = sizeof(PosixIPCQueueObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = queue_slots,
};

#if POSIXIPC_HAVE_MQ_OPEN

typedef struct
{
    PyObject_HEAD mqd_t mq;
    char *name;
    long msgsize;
    _Atomic uint32_t flags;
    _Atomic int notify_armed;
    PyObject *notify_fn;
    PyObject *notify_arg;
} PosixIPCNamedMQObject;

static int nmq_check_open(PosixIPCNamedMQObject *o, posixipc_state *st)
{
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_CLOSED) != 0) {
        return posixipc_raise_closed(st);
    }
    if (o->mq == (mqd_t)-1) {
        return posixipc_raise_unbound();
    }
    return 0;
}

static PyObject *nmq_from_mq(posixipc_state *st, mqd_t mq, const char *name)
{
    PosixIPCNamedMQObject *o;
    long msgsize = 0;
    int rc;

    o = (PosixIPCNamedMQObject *)st->NamedMessageQueue_Type->tp_alloc(st->NamedMessageQueue_Type, 0);
    if (o == NULL) {
        (void)posixipc_mq_close(mq);
        return NULL;
    }
    rc = posixipc_mq_msgsize(mq, &msgsize);
    if (rc != 0) {
        (void)posixipc_mq_close(mq);
        Py_DECREF(o);
        posixipc_err(st, rc);
        return NULL;
    }
    o->name = strdup(name);
    if (o->name == NULL) {
        (void)posixipc_mq_close(mq);
        Py_DECREF(o);
        return PyErr_NoMemory();
    }
    o->mq = mq;
    o->msgsize = msgsize;
    o->notify_fn = NULL;
    o->notify_arg = NULL;
    atomic_store_explicit(&o->notify_armed, 0, memory_order_relaxed);
    atomic_store_explicit(&o->flags, POSIXIPC_FLAG_BOUND, memory_order_release);
    return (PyObject *)o;
}

static PyObject *nmq_create(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    static char *kwlist[] = {"name", "maxmsg", "msgsize", NULL};
    const char *name;
    long maxmsg = 8;
    long msgsize = 256;
    mqd_t mq = (mqd_t)-1;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|ll:create", kwlist, &name, &maxmsg, &msgsize)) {
        return NULL;
    }
    if (maxmsg < 1 || msgsize < 1) {
        PyErr_SetString(PyExc_ValueError, "maxmsg and msgsize must be >= 1");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_create(name, maxmsg, msgsize, &mq);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return nmq_from_mq(st, mq, name);
}

static PyObject *nmq_attach(PyObject *cls, PyObject *args)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    const char *name;
    mqd_t mq = (mqd_t)-1;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "s:attach", &name)) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_attach(name, &mq);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return nmq_from_mq(st, mq, name);
}

static PyObject *nmq_open_or_create(PyObject *cls, PyObject *args, PyObject *kw)
{
    posixipc_state *st = posixipc_state_from_type((PyTypeObject *)cls);
    static char *kwlist[] = {"name", "maxmsg", "msgsize", NULL};
    const char *name;
    long maxmsg = 8;
    long msgsize = 256;
    mqd_t mq = (mqd_t)-1;
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|ll:open_or_create", kwlist, &name, &maxmsg, &msgsize)) {
        return NULL;
    }
    if (maxmsg < 1 || msgsize < 1) {
        PyErr_SetString(PyExc_ValueError, "maxmsg and msgsize must be >= 1");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_open_or_create(name, maxmsg, msgsize, &mq);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    return nmq_from_mq(st, mq, name);
}

static PyObject *nmq_unlink_name(PyObject *cls, PyObject *args)
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
    rc = posixipc_mq_unlink(name);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static int parse_nmq_put(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, posixipc_acquire_opts *opts,
                         unsigned *prio)
{
    PyObject *timeout_obj = Py_None;
    Py_ssize_t nkw = kwnames != NULL ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t i;

    opts->timeout_none = 1;
    opts->timeout = 0.0;
    opts->blocking = 1;
    opts->interruptible = 1;
    *prio = 0;
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "put() takes at most 1 positional argument");
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
            opts->blocking = PyObject_IsTrue(val);
            if (opts->blocking < 0) {
                return -1;
            }
        } else if (strcmp(key, "interruptible") == 0) {
            opts->interruptible = PyObject_IsTrue(val);
            if (opts->interruptible < 0) {
                return -1;
            }
        } else if (strcmp(key, "priority") == 0) {
            unsigned long v = PyLong_AsUnsignedLong(val);

            if (PyErr_Occurred()) {
                return -1;
            }
            *prio = (unsigned)v;
        } else {
            PyErr_Format(PyExc_TypeError, "unexpected keyword argument '%s'", key);
            return -1;
        }
    }
    if (timeout_obj == Py_None) {
        opts->timeout_none = 1;
        opts->timeout = 0.0;
    } else {
        opts->timeout_none = 0;
        opts->timeout = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred()) {
            return -1;
        }
        if (opts->timeout != opts->timeout || opts->timeout < 0.0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be non-negative");
            return -1;
        }
    }
    if (!opts->blocking && !opts->timeout_none && opts->timeout > 0.0) {
        PyErr_SetString(PyExc_ValueError, "can't specify a timeout for a non-blocking call");
        return -1;
    }
    return 0;
}

typedef struct
{
    mqd_t mq;
    const void *buf;
    size_t len;
    unsigned prio;
} nmq_send_arg;

typedef struct
{
    mqd_t mq;
    void *buf;
    size_t len;
    unsigned *prio;
    ssize_t *got;
} nmq_recv_arg;

static int nmq_send_cb(void *arg, const posixipc_deadline *slice)
{
    nmq_send_arg *a = (nmq_send_arg *)arg;

    return posixipc_mq_send_until(a->mq, a->buf, a->len, a->prio, slice);
}

static int nmq_recv_cb(void *arg, const posixipc_deadline *slice)
{
    nmq_recv_arg *a = (nmq_recv_arg *)arg;

    return posixipc_mq_receive_until(a->mq, a->buf, a->len, a->prio, a->got, slice);
}

static PyObject *nmq_put(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    Py_buffer view;
    unsigned prio = 0;
    int rc;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    nmq_send_arg sarg;

    if (st == NULL || nmq_check_open(o, st) < 0) {
        return NULL;
    }
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "put() missing data");
        return NULL;
    }
    if (parse_nmq_put(args + 1, nargs - 1, kwnames, &opts, &prio) < 0) {
        return NULL;
    }
    if (PyObject_GetBuffer(args[0], &view, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    if (view.len < 1 || view.len > o->msgsize) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "message length out of range");
        return NULL;
    }
    if (!opts.blocking) {
        /* try once with a zero deadline */
        rc = posixipc_deadline_from_seconds(CLOCK_REALTIME, 0.0, &user);
        if (rc != 0) {
            PyBuffer_Release(&view);
            posixipc_err(st, rc);
            return NULL;
        }
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_send_until(o->mq, view.buf, (size_t)view.len, prio, &user);
        Py_END_ALLOW_THREADS PyBuffer_Release(&view);
        if (rc == ETIMEDOUT || rc == EAGAIN || rc == EWOULDBLOCK) {
            Py_RETURN_FALSE;
        }
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        Py_RETURN_TRUE;
    }
    sarg.mq = o->mq;
    sarg.buf = view.buf;
    sarg.len = (size_t)view.len;
    sarg.prio = prio;
    if (!opts.timeout_none) {
        rc = posixipc_deadline_from_seconds(CLOCK_REALTIME, opts.timeout, &user);
        if (rc != 0) {
            PyBuffer_Release(&view);
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts.interruptible && opts.timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_send(o->mq, view.buf, (size_t)view.len, prio);
        Py_END_ALLOW_THREADS
    } else {
        rc = posixipc_blocking_wait(nmq_send_cb, &sarg, userp, opts.interruptible, CLOCK_REALTIME);
    }
    PyBuffer_Release(&view);
    if (rc == ETIMEDOUT || rc == EAGAIN) {
        Py_RETURN_FALSE;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        return NULL;
    }
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_TRUE;
}

static PyObject *nmq_get(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_acquire_opts opts;
    char *buf;
    ssize_t got = 0;
    unsigned prio = 0;
    int rc;
    posixipc_deadline user;
    const posixipc_deadline *userp = NULL;
    nmq_recv_arg rarg;
    PyObject *out;

    if (st == NULL || nmq_check_open(o, st) < 0) {
        return NULL;
    }
    if (posixipc_parse_acquire(args, nargs, kwnames, &opts) < 0) {
        return NULL;
    }
    buf = (char *)PyMem_Malloc((size_t)o->msgsize);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    if (!opts.blocking) {
        rc = posixipc_deadline_from_seconds(CLOCK_REALTIME, 0.0, &user);
        if (rc != 0) {
            PyMem_Free(buf);
            posixipc_err(st, rc);
            return NULL;
        }
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_receive_until(o->mq, buf, (size_t)o->msgsize, &prio, &got, &user);
        Py_END_ALLOW_THREADS if (rc == ETIMEDOUT || rc == EAGAIN || rc == EWOULDBLOCK)
        {
            PyMem_Free(buf);
            Py_RETURN_NONE;
        }
        if (rc != 0) {
            PyMem_Free(buf);
            posixipc_err(st, rc);
            return NULL;
        }
        out = PyBytes_FromStringAndSize(buf, got);
        PyMem_Free(buf);
        return out;
    }
    rarg.mq = o->mq;
    rarg.buf = buf;
    rarg.len = (size_t)o->msgsize;
    rarg.prio = &prio;
    rarg.got = &got;
    if (!opts.timeout_none) {
        rc = posixipc_deadline_from_seconds(CLOCK_REALTIME, opts.timeout, &user);
        if (rc != 0) {
            PyMem_Free(buf);
            posixipc_err(st, rc);
            return NULL;
        }
        userp = &user;
    }
    if (!opts.interruptible && opts.timeout_none) {
        Py_BEGIN_ALLOW_THREADS rc = posixipc_mq_receive(o->mq, buf, (size_t)o->msgsize, &prio, &got);
        Py_END_ALLOW_THREADS
    } else {
        rc = posixipc_blocking_wait(nmq_recv_cb, &rarg, userp, opts.interruptible, CLOCK_REALTIME);
    }
    if (rc == ETIMEDOUT || rc == EAGAIN) {
        PyMem_Free(buf);
        Py_RETURN_NONE;
    }
    if (rc == POSIXIPC_ERROR_INTERRUPTED) {
        PyMem_Free(buf);
        return NULL;
    }
    if (rc != 0) {
        PyMem_Free(buf);
        posixipc_err(st, rc);
        return NULL;
    }
    out = PyBytes_FromStringAndSize(buf, got);
    PyMem_Free(buf);
    return out;
}

static void nmq_disarm(PosixIPCNamedMQObject *o)
{
    if (o->mq != (mqd_t)-1) {
        (void)posixipc_mq_notify_cancel(o->mq);
    }
    if (atomic_exchange_explicit(&o->notify_armed, 0, memory_order_acq_rel)) {
        Py_CLEAR(o->notify_fn);
        Py_CLEAR(o->notify_arg);
        Py_DECREF(o);
    }
}

static void nmq_notify_thread(union sigval v)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)v.sival_ptr;
    PyGILState_STATE g;
    PyObject *fn;
    PyObject *arg;
    PyObject *res;

    if (o == NULL) {
        return;
    }
    g = PyGILState_Ensure();
    if (!atomic_exchange_explicit(&o->notify_armed, 0, memory_order_acq_rel)) {
        PyGILState_Release(g);
        return;
    }
    fn = o->notify_fn;
    arg = o->notify_arg;
    o->notify_fn = NULL;
    o->notify_arg = NULL;
    if (fn != NULL && (atomic_load_explicit(&o->flags, memory_order_acquire) & POSIXIPC_FLAG_CLOSED) == 0) {
        res = arg != NULL ? PyObject_CallOneArg(fn, arg) : PyObject_CallNoArgs(fn);
        Py_XDECREF(res);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(fn);
        }
    }
    Py_XDECREF(fn);
    Py_XDECREF(arg);
    Py_DECREF(o);
    PyGILState_Release(g);
}

static PyObject *nmq_request_notification(PyObject *self, PyObject *args)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *notification = Py_None;
    PyObject *fn;
    PyObject *arg;
    long signo;
    int rc;

    if (st == NULL || nmq_check_open(o, st) < 0) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "|O:request_notification", &notification)) {
        return NULL;
    }
    nmq_disarm(o);
    if (notification == Py_None) {
        Py_RETURN_NONE;
    }
    if (PyLong_Check(notification)) {
        signo = PyLong_AsLong(notification);
        if (PyErr_Occurred()) {
            return NULL;
        }
        if (signo < 1) {
            PyErr_SetString(PyExc_ValueError, "signal must be a positive signo");
            return NULL;
        }
        rc = posixipc_mq_notify_signal(o->mq, (int)signo);
        if (rc != 0) {
            posixipc_err(st, rc);
            return NULL;
        }
        Py_RETURN_NONE;
    }
    fn = notification;
    arg = NULL;
    if (PyTuple_Check(notification) && PyTuple_GET_SIZE(notification) == 2) {
        fn = PyTuple_GET_ITEM(notification, 0);
        arg = PyTuple_GET_ITEM(notification, 1);
    }
    if (!PyCallable_Check(fn)) {
        PyErr_SetString(PyExc_TypeError, "notification must be None, a signal number, a callable, "
                                         "or (callable, arg)");
        return NULL;
    }
    Py_INCREF(fn);
    Py_XINCREF(arg);
    o->notify_fn = fn;
    o->notify_arg = arg;
    atomic_store_explicit(&o->notify_armed, 1, memory_order_release);
    Py_INCREF(o);
    rc = posixipc_mq_notify_thread(o->mq, nmq_notify_thread, o);
    if (rc != 0) {
        atomic_store_explicit(&o->notify_armed, 0, memory_order_release);
        o->notify_fn = NULL;
        o->notify_arg = NULL;
        Py_DECREF(fn);
        Py_XDECREF(arg);
        Py_DECREF(o);
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *nmq_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    if ((flags & POSIXIPC_FLAG_CLOSED) != 0) {
        Py_RETURN_NONE;
    }
    nmq_disarm(o);
    if (o->mq != (mqd_t)-1) {
        (void)posixipc_mq_close(o->mq);
        o->mq = (mqd_t)-1;
    }
    atomic_fetch_or_explicit(&o->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *nmq_unlink(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    int rc;

    if (st == NULL) {
        return NULL;
    }
    if (o->name == NULL) {
        PyErr_SetString(PyExc_ValueError, "queue has no name");
        return NULL;
    }
    rc = posixipc_mq_unlink(o->name);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *nmq_get_name(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;

    if (o->name == NULL) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(o->name);
}

static PyObject *nmq_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    uint32_t flags = atomic_load_explicit(&o->flags, memory_order_acquire);

    return PyBool_FromLong((flags & POSIXIPC_FLAG_CLOSED) != 0);
}

static int nmq_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(o->notify_fn);
    Py_VISIT(o->notify_arg);
    return 0;
}

static int nmq_clear(PyObject *self)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;

    Py_CLEAR(o->notify_fn);
    Py_CLEAR(o->notify_arg);
    return 0;
}

static void nmq_dealloc(PyObject *self)
{
    PosixIPCNamedMQObject *o = (PosixIPCNamedMQObject *)self;
    PyTypeObject *tp = Py_TYPE(self);

    PyObject_GC_UnTrack(self);
    if ((atomic_load_explicit(&o->flags, memory_order_acquire) & POSIXIPC_FLAG_CLOSED) == 0) {
        nmq_disarm(o);
        if (o->mq != (mqd_t)-1) {
            (void)posixipc_mq_close(o->mq);
            o->mq = (mqd_t)-1;
        }
    }
    Py_CLEAR(o->notify_fn);
    Py_CLEAR(o->notify_arg);
    free(o->name);
    o->name = NULL;
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef nmq_methods[] = {
    {"create", POSIXIPC_METH(nmq_create), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"attach", POSIXIPC_METH(nmq_attach), METH_VARARGS | METH_CLASS, NULL},
    {"open_or_create", POSIXIPC_METH(nmq_open_or_create), METH_VARARGS | METH_KEYWORDS | METH_CLASS, NULL},
    {"unlink_name", POSIXIPC_METH(nmq_unlink_name), METH_VARARGS | METH_CLASS, NULL},
    {"put", POSIXIPC_METH(nmq_put), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"get", POSIXIPC_METH(nmq_get), METH_FASTCALL | METH_KEYWORDS, NULL},
    {"request_notification", nmq_request_notification, METH_VARARGS, NULL},
    {"close", nmq_close, METH_NOARGS, NULL},
    {"unlink", nmq_unlink, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef nmq_getset[] = {
    {"name", nmq_get_name, NULL, NULL, NULL},
    {"closed", nmq_get_closed, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static int nmq_init(PyObject *self, PyObject *args, PyObject *kw)
{
    (void)self;
    (void)args;
    (void)kw;
    PyErr_SetString(PyExc_TypeError, "NamedMessageQueue() cannot be called; use create() or attach()");
    return -1;
}

static PyType_Slot nmq_slots[] = {
    {Py_tp_dealloc, nmq_dealloc},   {Py_tp_traverse, nmq_traverse},
    {Py_tp_clear, nmq_clear},       {Py_tp_init, nmq_init},
    {Py_tp_methods, nmq_methods},   {Py_tp_getset, nmq_getset},
    {Py_tp_new, PyType_GenericNew}, {0, NULL},
};

static PyType_Spec nmq_spec = {
    .name = "posixipc.NamedMessageQueue",
    .basicsize = sizeof(PosixIPCNamedMQObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = nmq_slots,
};

#endif

int posixipc_queue_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;

    type = PyType_FromModuleAndSpec(mod, &queue_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Queue_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Queue", type) < 0) {
        return -1;
    }
#if POSIXIPC_HAVE_MQ_OPEN
    type = PyType_FromModuleAndSpec(mod, &nmq_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->NamedMessageQueue_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "NamedMessageQueue", type) < 0) {
        return -1;
    }
#endif
    return 0;
}
