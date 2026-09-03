#include "py_internal.h"

#include "posixipc_config.h"
#include "posixipc_queue.h"

#include <string.h>

static int bytes_is_closed(PosixIPCBytesObject *b)
{
    uint32_t flags = atomic_load_explicit(&b->flags, memory_order_acquire);

    return (flags & POSIXIPC_FLAG_CLOSED) != 0;
}

static int bytes_check_open(PosixIPCBytesObject *b, posixipc_state *st)
{
    if (bytes_is_closed(b)) {
        return posixipc_raise_closed(st);
    }
    if ((atomic_load_explicit(&b->flags, memory_order_acquire) & POSIXIPC_FLAG_BOUND) == 0 || b->region == NULL) {
        return posixipc_raise_unbound();
    }
    return 0;
}

PyObject *posixipc_bytes_new_unbound(posixipc_state *st, uint32_t size, uint32_t slot)
{
    PosixIPCBytesObject *b;

    b = (PosixIPCBytesObject *)st->Bytes_Type->tp_alloc(st->Bytes_Type, 0);
    if (b == NULL) {
        return NULL;
    }
    b->region = NULL;
    b->slot = slot;
    b->kind = POSIXIPC_KIND_BYTES;
    b->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_BYTES);
    b->offset = 0;
    b->size = size;
    b->init_flags = 0;
    atomic_store_explicit(&b->flags, 0u, memory_order_relaxed);
    return (PyObject *)b;
}

int posixipc_bytes_bind(PosixIPCBytesObject *b, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                        uint32_t index)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)b);
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
    (void)ptr;
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    Py_XSETREF(b->region, (PyObject *)r);
    b->slot = index;
    b->kind = slot->kind;
    b->align = slot->align;
    b->offset = slot->offset;
    b->size = slot->size;
    b->init_flags = slot->init_flags;
    atomic_fetch_or_explicit(&b->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
    return 0;
}

PyObject *posixipc_bytes_view(PosixIPCSharedMemoryObject *r, uint32_t offset, uint32_t size)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)r);
    posixipc_slot slot;
    PyObject *obj;

    if (st == NULL) {
        return NULL;
    }
    obj = posixipc_bytes_new_unbound(st, size, 0);
    if (obj == NULL) {
        return NULL;
    }
    memset(&slot, 0, sizeof(slot));
    slot.kind = POSIXIPC_KIND_BYTES;
    slot.align = 1;
    slot.offset = offset;
    slot.size = size == 0 ? 1u : size;
    slot.init_flags = 0;
    if (size == 0) {
        PosixIPCBytesObject *b = (PosixIPCBytesObject *)obj;

        if (posixipc_shmobj_pin(r) < 0) {
            Py_DECREF(obj);
            return NULL;
        }
        Py_INCREF(r);
        b->region = (PyObject *)r;
        b->offset = offset;
        b->size = 0;
        b->align = 1;
        atomic_store_explicit(&b->flags, POSIXIPC_FLAG_BOUND | POSIXIPC_FLAG_PROCESS_SHARED, memory_order_release);
        return obj;
    }
    if (posixipc_bytes_bind((PosixIPCBytesObject *)obj, r, &slot, 0) < 0) {
        Py_DECREF(obj);
        return NULL;
    }
    return obj;
}

static PyObject *bytes_close(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)self;

    if (bytes_is_closed(b)) {
        Py_RETURN_NONE;
    }
    if (b->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)b->region);
        Py_CLEAR(b->region);
    }
    atomic_fetch_or_explicit(&b->flags, POSIXIPC_FLAG_CLOSED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *bytes_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    uint32_t flags = atomic_load_explicit(&b->flags, memory_order_acquire);
    PosixIPCSharedMemoryObject *r;
    PyObject *mod;
    PyObject *fn;
    PyObject *args;
    PyObject *tuple;

    if (st == NULL) {
        return NULL;
    }
    if (bytes_is_closed(b)) {
        posixipc_raise_closed(st);
        return NULL;
    }
    if ((flags & POSIXIPC_FLAG_PROCESS_SHARED) == 0 || (flags & POSIXIPC_FLAG_BOUND) == 0 || b->region == NULL) {
        PyErr_SetString(PyExc_TypeError, "cannot pickle a process-private or unbound handle");
        return NULL;
    }
    r = (PosixIPCSharedMemoryObject *)b->region;
    if (r->core.hdr == NULL || r->core.name == NULL) {
        posixipc_raise_closed(st);
        return NULL;
    }
    mod = PyType_GetModule(st->Bytes_Type);
    if (mod == NULL) {
        return NULL;
    }
    fn = PyObject_GetAttrString(mod, "_attach_slot");
    if (fn == NULL) {
        return NULL;
    }
    args = Py_BuildValue("(sIII)", r->core.name, b->slot, (unsigned)b->kind, r->core.hdr->layout_digest);
    if (args == NULL) {
        Py_DECREF(fn);
        return NULL;
    }
    tuple = PyTuple_Pack(2, fn, args);
    Py_DECREF(fn);
    Py_DECREF(args);
    return tuple;
}

static PyObject *bytes_copy(PyObject *self, PyObject *Py_UNUSED(args))
{
    (void)self;
    PyErr_SetString(PyExc_TypeError, "posixipc byte handles cannot be copied");
    return NULL;
}

static int bytes_getbuffer(PyObject *exporter, Py_buffer *view, int flags)
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)exporter;
    posixipc_state *st = posixipc_state_from_obj(exporter);
    PosixIPCSharedMemoryObject *r;
    void *ptr;

    if (bytes_check_open(b, st) < 0) {
        return -1;
    }
    r = (PosixIPCSharedMemoryObject *)b->region;
    ptr = (char *)r->core.map + b->offset;
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    if (PyBuffer_FillInfo(view, exporter, ptr, (Py_ssize_t)b->size, 0, flags) < 0) {
        posixipc_shmobj_unpin(r);
        return -1;
    }
    return 0;
}

static void bytes_releasebuffer(PyObject *exporter, Py_buffer *Py_UNUSED(view))
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)exporter;

    if (b->region != NULL) {
        posixipc_shmobj_unpin((PosixIPCSharedMemoryObject *)b->region);
    }
}

static PyObject *bytes_get_region(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)self;

    if (b->region == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(b->region);
}

static PyObject *bytes_get_slot(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBytesObject *)self)->slot);
}

static PyObject *bytes_get_kind(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBytesObject *)self)->kind);
}

static PyObject *bytes_get_offset(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBytesObject *)self)->offset);
}

static PyObject *bytes_get_size(PyObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromUnsignedLong(((PosixIPCBytesObject *)self)->size);
}

static PyObject *bytes_get_closed(PyObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(bytes_is_closed((PosixIPCBytesObject *)self));
}

static int bytes_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(b->region);
    return 0;
}

static int bytes_clear(PyObject *Py_UNUSED(self))
{
    return 0;
}

static void bytes_dealloc(PyObject *self)
{
    PosixIPCBytesObject *b = (PosixIPCBytesObject *)self;
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *pending = PyErr_GetRaisedException();

    PyObject_GC_UnTrack(self);
    if (!bytes_is_closed(b)) {
        PyObject *r = bytes_close(self, NULL);

        Py_XDECREF(r);
        if (PyErr_Occurred()) {
            PyErr_WriteUnraisable(self);
        }
    }
    Py_CLEAR(b->region);
    PyErr_SetRaisedException(pending);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef bytes_methods[] = {
    {"close", bytes_close, METH_NOARGS, NULL},
    {"__reduce__", bytes_reduce, METH_NOARGS, NULL},
    {"__copy__", bytes_copy, METH_NOARGS, NULL},
    {"__deepcopy__", bytes_copy, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef bytes_getset[] = {
    {"region", bytes_get_region, NULL, NULL, NULL},
    {"slot", bytes_get_slot, NULL, NULL, NULL},
    {"kind", bytes_get_kind, NULL, NULL, NULL},
    {"offset", bytes_get_offset, NULL, NULL, NULL},
    {"size", bytes_get_size, NULL, NULL, NULL},
    {"closed", bytes_get_closed, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot bytes_slots[] = {
    {Py_tp_dealloc, bytes_dealloc},
    {Py_tp_traverse, bytes_traverse},
    {Py_tp_clear, bytes_clear},
    {Py_tp_methods, bytes_methods},
    {Py_tp_getset, bytes_getset},
    {Py_bf_getbuffer, bytes_getbuffer},
    {Py_bf_releasebuffer, bytes_releasebuffer},
    {Py_tp_new, PyType_GenericNew},
    {0, NULL},
};

static PyType_Spec bytes_spec = {
    .name = "posixipc.SharedBytes",
    .basicsize = sizeof(PosixIPCBytesObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = bytes_slots,
};

static int layout_ensure(PosixIPCLayoutObject *l, uint32_t need)
{
    uint16_t ncap;
    posixipc_slot *p;

    if (need > 0xffffu) {
        PyErr_SetString(PyExc_OverflowError, "too many layout slots");
        return -1;
    }
    if (l->cap >= need) {
        return 0;
    }
    ncap = l->cap == 0 ? 4u : l->cap;
    while (ncap < need) {
        uint16_t next = (uint16_t)(ncap * 2u);

        if (next <= ncap) {
            ncap = 0xffffu;
            break;
        }
        ncap = next;
    }
    p = (posixipc_slot *)PyMem_Realloc(l->slots, (size_t)ncap * sizeof(*p));
    if (p == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    l->slots = p;
    l->cap = ncap;
    return 0;
}

static int layout_reserve(PosixIPCLayoutObject *l)
{
    return layout_ensure(l, (uint32_t)l->nslots + 1u);
}

static int layout_handle_span(posixipc_state *st, PyObject *h, uint32_t *first, uint32_t *count)
{
    if (st->MutexArray_Type != NULL && PyObject_TypeCheck(h, st->MutexArray_Type)) {
        PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)h;

        *first = a->first_slot;
        *count = a->count;
        return 0;
    }
    if (PyObject_TypeCheck(h, st->Mutex_Type)) {
        *first = ((PosixIPCMutexObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (PyObject_TypeCheck(h, st->Bytes_Type)) {
        *first = ((PosixIPCBytesObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (PyObject_TypeCheck(h, st->RWLock_Type)) {
        *first = ((PosixIPCRWLockObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (PyObject_TypeCheck(h, st->Condition_Type)) {
        *first = ((PosixIPCConditionObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (PyObject_TypeCheck(h, st->Semaphore_Type)) {
        *first = ((PosixIPCSemaphoreObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (st->Barrier_Type != NULL && PyObject_TypeCheck(h, st->Barrier_Type)) {
        *first = ((PosixIPCBarrierObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (st->SpinLock_Type != NULL && PyObject_TypeCheck(h, st->SpinLock_Type)) {
        *first = ((PosixIPCSpinLockObject *)h)->slot;
        *count = 1;
        return 0;
    }
    if (st->Queue_Type != NULL && PyObject_TypeCheck(h, st->Queue_Type)) {
        *first = ((PosixIPCQueueObject *)h)->first_slot;
        *count = POSIXIPC_QUEUE_SLOTS;
        return 0;
    }
    if (st->FutexQueue_Type != NULL && PyObject_TypeCheck(h, st->FutexQueue_Type)) {
        *first = ((PosixIPCFutexQueueObject *)h)->first_slot;
        *count = POSIXIPC_FUTEX_QUEUE_SLOTS;
        return 0;
    }
    PyErr_SetString(PyExc_TypeError, "unknown layout handle");
    return -1;
}

static void layout_apply_slot_meta(posixipc_state *st, PyObject *h, const posixipc_slot *slot)
{
    if (st->MutexArray_Type != NULL && PyObject_TypeCheck(h, st->MutexArray_Type)) {
        PosixIPCMutexArrayObject *a = (PosixIPCMutexArrayObject *)h;

        a->align = slot->align;
        a->offset = slot->offset;
        a->size = slot->size;
        return;
    }
    if (PyObject_TypeCheck(h, st->Mutex_Type)) {
        PosixIPCMutexObject *m = (PosixIPCMutexObject *)h;

        m->align = slot->align;
        m->offset = slot->offset;
        m->size = slot->size;
        return;
    }
    if (PyObject_TypeCheck(h, st->Bytes_Type)) {
        PosixIPCBytesObject *b = (PosixIPCBytesObject *)h;

        b->align = slot->align;
        b->offset = slot->offset;
        b->size = slot->size;
        return;
    }
    if (PyObject_TypeCheck(h, st->RWLock_Type)) {
        PosixIPCRWLockObject *o = (PosixIPCRWLockObject *)h;

        o->align = slot->align;
        o->offset = slot->offset;
        o->size = slot->size;
        return;
    }
    if (PyObject_TypeCheck(h, st->Condition_Type)) {
        PosixIPCConditionObject *o = (PosixIPCConditionObject *)h;

        o->align = slot->align;
        o->offset = slot->offset;
        o->size = slot->size;
        return;
    }
    if (PyObject_TypeCheck(h, st->Semaphore_Type)) {
        PosixIPCSemaphoreObject *o = (PosixIPCSemaphoreObject *)h;

        o->align = slot->align;
        o->offset = slot->offset;
        o->size = slot->size;
        return;
    }
    if (st->Barrier_Type != NULL && PyObject_TypeCheck(h, st->Barrier_Type)) {
        PosixIPCBarrierObject *o = (PosixIPCBarrierObject *)h;

        o->align = slot->align;
        o->offset = slot->offset;
        o->size = slot->size;
        return;
    }
    if (st->SpinLock_Type != NULL && PyObject_TypeCheck(h, st->SpinLock_Type)) {
        PosixIPCSpinLockObject *o = (PosixIPCSpinLockObject *)h;

        o->align = slot->align;
        o->offset = slot->offset;
        o->size = slot->size;
        return;
    }
    (void)slot;
}

static int layout_sync(PosixIPCLayoutObject *l)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)l);
    posixipc_shm_expect expect;
    int rc;
    Py_ssize_t i;
    Py_ssize_t n;

    rc = posixipc_layout_build(l->slots, l->nslots, &expect);
    if (rc != 0) {
        posixipc_err(st, rc);
        return -1;
    }
    n = PyList_GET_SIZE(l->handles);
    for (i = 0; i < n; i++) {
        PyObject *h = PyList_GET_ITEM(l->handles, i);
        uint32_t first;
        uint32_t count;

        if (layout_handle_span(st, h, &first, &count) < 0) {
            return -1;
        }
        if (count == 0 || first >= l->nslots || first + count > l->nslots) {
            PyErr_SetString(PyExc_RuntimeError, "layout handle slot is invalid");
            return -1;
        }
        layout_apply_slot_meta(st, h, &l->slots[first]);
    }
    return 0;
}

static int layout_init_primitives(PosixIPCLayoutObject *l, posixipc_shm *core)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)l);
    uint16_t i;
    int rc;
    void *ptr;

    for (i = 0; i < l->nslots; i++) {
        rc = posixipc_shm_offset_ptr(core, l->slots[i].offset, l->slots[i].size, l->slots[i].align, &ptr);
        if (rc != 0) {
            posixipc_err(st, rc);
            return -1;
        }
        if (l->slots[i].kind == POSIXIPC_KIND_MUTEX || l->slots[i].kind == POSIXIPC_KIND_ROBUST_MUTEX) {
            posixipc_mutex_config cfg = {.flags = l->slots[i].init_flags};

            rc = posixipc_mutex_init((pthread_mutex_t *)ptr, &cfg);
        } else if (l->slots[i].kind == POSIXIPC_KIND_RWLOCK) {
            posixipc_rwlock_config cfg = {.flags = l->slots[i].init_flags};

            rc = posixipc_rwlock_init((pthread_rwlock_t *)ptr, &cfg);
        } else if (l->slots[i].kind == POSIXIPC_KIND_COND) {
            posixipc_cond_config cfg = {.flags = l->slots[i].init_flags};

            rc = posixipc_cond_init((pthread_cond_t *)ptr, &cfg);
        } else if (l->slots[i].kind == POSIXIPC_KIND_SEM) {
            posixipc_sem_config cfg = {
                .flags = l->slots[i].init_flags,
                .value = l->slots[i].init_flags >> POSIXIPC_SLOT_AUX_SHIFT,
            };

            rc = posixipc_sem_init((sem_t *)ptr, &cfg);
        } else if (l->slots[i].kind == POSIXIPC_KIND_BARRIER) {
            posixipc_barrier_config cfg = {
                .flags = l->slots[i].init_flags,
                .parties = l->slots[i].init_flags >> POSIXIPC_SLOT_AUX_SHIFT,
            };

            rc = posixipc_barrier_init((pthread_barrier_t *)ptr, &cfg);
        } else if (l->slots[i].kind == POSIXIPC_KIND_SPIN) {
            posixipc_spin_config cfg = {.flags = l->slots[i].init_flags};

            rc = posixipc_spin_init((pthread_spinlock_t *)ptr, &cfg);
        } else {
            continue;
        }
        if (rc != 0) {
            posixipc_err(st, rc);
            return -1;
        }
    }
    return 0;
}

static int layout_init_queue_ctrls(PosixIPCLayoutObject *l, posixipc_shm *core)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)l);
    Py_ssize_t i;
    Py_ssize_t n;

    if (st == NULL || st->Queue_Type == NULL) {
        return 0;
    }
    n = PyList_GET_SIZE(l->handles);
    for (i = 0; i < n; i++) {
        PyObject *h = PyList_GET_ITEM(l->handles, i);
        PosixIPCQueueObject *q;
        uint32_t first;
        int rc;

        if (!PyObject_TypeCheck(h, st->Queue_Type)) {
            continue;
        }
        q = (PosixIPCQueueObject *)h;
        first = q->first_slot;
        if (first + POSIXIPC_QUEUE_SLOTS > l->nslots) {
            PyErr_SetString(PyExc_RuntimeError, "Queue slot span is invalid");
            return -1;
        }
        rc = posixipc_queue_init_on_shm(core, &l->slots[first + 4u], q->depth, q->item_size);
        if (rc != 0) {
            posixipc_err(st, rc);
            return -1;
        }
    }
    if (st->FutexQueue_Type == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        PyObject *h = PyList_GET_ITEM(l->handles, i);
        PosixIPCFutexQueueObject *q;
        uint32_t first;
        int rc;

        if (!PyObject_TypeCheck(h, st->FutexQueue_Type)) {
            continue;
        }
        q = (PosixIPCFutexQueueObject *)h;
        first = q->first_slot;
        if (first + POSIXIPC_FUTEX_QUEUE_SLOTS > l->nslots) {
            PyErr_SetString(PyExc_RuntimeError, "FutexQueue slot span is invalid");
            return -1;
        }
        rc = posixipc_futexq_init_on_shm(core, &l->slots[first + 2u], q->depth, q->item_size);
        if (rc != 0) {
            posixipc_err(st, rc);
            return -1;
        }
    }
    return 0;
}

static int layout_bind_all(PosixIPCLayoutObject *l, PosixIPCSharedMemoryObject *r, int do_init)
{
    posixipc_state *st = posixipc_state_from_obj((PyObject *)l);
    Py_ssize_t i;
    Py_ssize_t n;

    (void)do_init;
    n = PyList_GET_SIZE(l->handles);
    for (i = 0; i < n; i++) {
        PyObject *h = PyList_GET_ITEM(l->handles, i);
        uint32_t first;
        uint32_t count;

        if (layout_handle_span(st, h, &first, &count) < 0) {
            return -1;
        }
        if (st->MutexArray_Type != NULL && PyObject_TypeCheck(h, st->MutexArray_Type)) {
            if (posixipc_mutexarray_bind((PosixIPCMutexArrayObject *)h, r, l->slots, first, count) < 0) {
                return -1;
            }
        } else if (PyObject_TypeCheck(h, st->Mutex_Type)) {
            if (posixipc_mutex_bind((PosixIPCMutexObject *)h, r, &l->slots[first], first, 0) < 0) {
                return -1;
            }
        } else if (PyObject_TypeCheck(h, st->Bytes_Type)) {
            if (posixipc_bytes_bind((PosixIPCBytesObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (PyObject_TypeCheck(h, st->RWLock_Type)) {
            if (posixipc_rwlock_bind((PosixIPCRWLockObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (PyObject_TypeCheck(h, st->Condition_Type)) {
            if (posixipc_cond_bind((PosixIPCConditionObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (PyObject_TypeCheck(h, st->Semaphore_Type)) {
            if (posixipc_sem_bind((PosixIPCSemaphoreObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (st->Barrier_Type != NULL && PyObject_TypeCheck(h, st->Barrier_Type)) {
            if (posixipc_barrier_bind((PosixIPCBarrierObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (st->SpinLock_Type != NULL && PyObject_TypeCheck(h, st->SpinLock_Type)) {
            if (posixipc_spin_bind((PosixIPCSpinLockObject *)h, r, &l->slots[first], first) < 0) {
                return -1;
            }
        } else if (st->Queue_Type != NULL && PyObject_TypeCheck(h, st->Queue_Type)) {
            if (posixipc_queue_bind((PosixIPCQueueObject *)h, r, l->slots, first) < 0) {
                return -1;
            }
        } else if (st->FutexQueue_Type != NULL && PyObject_TypeCheck(h, st->FutexQueue_Type)) {
            if (posixipc_futexq_bind((PosixIPCFutexQueueObject *)h, r, l->slots, first) < 0) {
                return -1;
            }
        } else {
            PyErr_SetString(PyExc_TypeError, "unknown layout handle");
            return -1;
        }
    }
    return 0;
}

static int parse_attach_timeout(PyObject *timeout_obj, posixipc_deadline *out, const posixipc_deadline **ptr)
{
    double seconds;
    int rc;

    if (timeout_obj == NULL) {
        seconds = 5.0;
    } else if (timeout_obj == Py_None) {
        *ptr = NULL;
        return 0;
    } else {
        seconds = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred()) {
            return -1;
        }
        if (seconds != seconds || seconds < 0.0) {
            PyErr_SetString(PyExc_ValueError, "timeout must be non-negative");
            return -1;
        }
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

static PyObject *layout_commit(PosixIPCLayoutObject *l, PyObject *handle, posixipc_slot *slot)
{
    if (PyList_Append(l->handles, handle) < 0) {
        Py_DECREF(handle);
        return NULL;
    }
    l->nslots += 1;
    (void)slot;
    if (layout_sync(l) < 0) {
        Py_DECREF(handle);
        PySequence_DelItem(l->handles, PyList_GET_SIZE(l->handles) - 1);
        l->nslots -= 1;
        return NULL;
    }
    return handle;
}

static int layout_mutex_index(PosixIPCLayoutObject *l, PyObject *mutex)
{
    Py_ssize_t i;
    Py_ssize_t n = PyList_GET_SIZE(l->handles);

    for (i = 0; i < n; i++) {
        if (PyList_GET_ITEM(l->handles, i) == mutex) {
            return (int)((PosixIPCMutexObject *)mutex)->slot;
        }
    }
    return -1;
}

static PyObject *layout_commit_n(PosixIPCLayoutObject *l, PyObject *handle, uint16_t n)
{
    uint16_t start = l->nslots;
    uint16_t i;

    if (n == 0) {
        Py_DECREF(handle);
        PyErr_SetString(PyExc_ValueError, "array count must be positive");
        return NULL;
    }
    if (PyList_Append(l->handles, handle) < 0) {
        Py_DECREF(handle);
        return NULL;
    }
    for (i = 1; i < n; i++) {
        l->slots[start + i] = l->slots[start];
    }
    l->nslots = (uint16_t)(start + n);
    if (layout_sync(l) < 0) {
        Py_DECREF(handle);
        PySequence_DelItem(l->handles, PyList_GET_SIZE(l->handles) - 1);
        l->nslots = start;
        return NULL;
    }
    return handle;
}

static PyObject *layout_commit_span(PosixIPCLayoutObject *l, PyObject *handle, uint16_t n)
{
    uint16_t start = l->nslots;

    if (n == 0) {
        Py_DECREF(handle);
        PyErr_SetString(PyExc_ValueError, "slot span must be positive");
        return NULL;
    }
    if (PyList_Append(l->handles, handle) < 0) {
        Py_DECREF(handle);
        return NULL;
    }
    l->nslots = (uint16_t)(start + n);
    if (layout_sync(l) < 0) {
        Py_DECREF(handle);
        PySequence_DelItem(l->handles, PyList_GET_SIZE(l->handles) - 1);
        l->nslots = start;
        return NULL;
    }
    return handle;
}

static PyObject *layout_add_queue(PosixIPCLayoutObject *l, posixipc_state *st, Py_ssize_t depth, Py_ssize_t item_size,
                                  PyObject *on_owner_died)
{
    uint32_t nbytes;
    uint32_t padded;
    uint32_t first;
    uint32_t mutex_flags;
    uint32_t cond0;
    uint32_t cond1;
    PyObject *handle;

    if (on_owner_died != NULL && on_owner_died != Py_None) {
        PyErr_SetString(PyExc_TypeError, "Queue does not take on_owner_died");
        return NULL;
    }
    if (depth < 1 || item_size < 1 || depth > (Py_ssize_t)0xffffffffu || item_size > (Py_ssize_t)0xffffffffu) {
        PyErr_SetString(PyExc_ValueError, "Queue requires depth= and item_size= >= 1");
        return NULL;
    }
    nbytes = posixipc_queue_bytes_size((uint32_t)depth, (uint32_t)item_size);
    if (nbytes == 0) {
        PyErr_SetString(PyExc_ValueError, "Queue size out of range");
        return NULL;
    }
    padded = posixipc_kind_size(POSIXIPC_KIND_BYTES, nbytes);
    if (padded == 0) {
        PyErr_SetString(PyExc_ValueError, "Queue size out of range");
        return NULL;
    }
    if (layout_ensure(l, (uint32_t)l->nslots + POSIXIPC_QUEUE_SLOTS) < 0) {
        return NULL;
    }
    first = l->nslots;
    mutex_flags = POSIXIPC_FLAG_PROCESS_SHARED | POSIXIPC_FLAG_ROBUST;
    cond0 = POSIXIPC_FLAG_PROCESS_SHARED | (first << POSIXIPC_SLOT_AUX_SHIFT);
    cond1 = POSIXIPC_FLAG_PROCESS_SHARED | ((first + 1u) << POSIXIPC_SLOT_AUX_SHIFT);
#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    cond0 |= POSIXIPC_FLAG_MONOTONIC;
    cond1 |= POSIXIPC_FLAG_MONOTONIC;
#endif
    memset(&l->slots[first], 0, sizeof(posixipc_slot) * POSIXIPC_QUEUE_SLOTS);
    l->slots[first].kind = POSIXIPC_KIND_ROBUST_MUTEX;
    l->slots[first].init_flags = mutex_flags;
    l->slots[first + 1u].kind = POSIXIPC_KIND_ROBUST_MUTEX;
    l->slots[first + 1u].init_flags = mutex_flags;
    l->slots[first + 2u].kind = POSIXIPC_KIND_COND;
    l->slots[first + 2u].init_flags = cond0;
    l->slots[first + 3u].kind = POSIXIPC_KIND_COND;
    l->slots[first + 3u].init_flags = cond1;
    l->slots[first + 4u].kind = POSIXIPC_KIND_BYTES;
    l->slots[first + 4u].align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_BYTES);
    l->slots[first + 4u].size = padded;
    l->slots[first + 4u].init_flags = (uint32_t)depth * 0x9e3779b1u ^ (uint32_t)item_size * 0x85ebca77u;
    if (l->slots[first + 4u].init_flags == 0u) {
        l->slots[first + 4u].init_flags = 1u;
    }
    handle = posixipc_queue_new_unbound(st, (uint32_t)depth, (uint32_t)item_size, first);
    return handle == NULL ? NULL : layout_commit_span(l, handle, POSIXIPC_QUEUE_SLOTS);
}

static PyObject *layout_add_futex_queue(PosixIPCLayoutObject *l, posixipc_state *st, Py_ssize_t depth,
                                        Py_ssize_t item_size, PyObject *on_owner_died)
{
    uint32_t nbytes;
    uint32_t padded;
    uint32_t first;
    uint32_t mutex_flags;
    PyObject *handle;

    if (on_owner_died != NULL && on_owner_died != Py_None) {
        PyErr_SetString(PyExc_TypeError, "FutexQueue does not take on_owner_died");
        return NULL;
    }
    if (depth < 1 || item_size < 1 || depth > (Py_ssize_t)0xffffffffu || item_size > (Py_ssize_t)0xffffffffu) {
        PyErr_SetString(PyExc_ValueError, "FutexQueue requires depth= and item_size= >= 1");
        return NULL;
    }
    nbytes = posixipc_futexq_bytes_size((uint32_t)depth, (uint32_t)item_size);
    if (nbytes == 0) {
        PyErr_SetString(PyExc_ValueError, "FutexQueue size out of range");
        return NULL;
    }
    padded = posixipc_kind_size(POSIXIPC_KIND_BYTES, nbytes);
    if (padded == 0) {
        PyErr_SetString(PyExc_ValueError, "FutexQueue size out of range");
        return NULL;
    }
    if (layout_ensure(l, (uint32_t)l->nslots + POSIXIPC_FUTEX_QUEUE_SLOTS) < 0) {
        return NULL;
    }
    first = l->nslots;
    mutex_flags = POSIXIPC_FLAG_PROCESS_SHARED | POSIXIPC_FLAG_ROBUST;
    memset(&l->slots[first], 0, sizeof(posixipc_slot) * POSIXIPC_FUTEX_QUEUE_SLOTS);
    l->slots[first].kind = POSIXIPC_KIND_ROBUST_MUTEX;
    l->slots[first].init_flags = mutex_flags;
    l->slots[first + 1u].kind = POSIXIPC_KIND_ROBUST_MUTEX;
    l->slots[first + 1u].init_flags = mutex_flags;
    l->slots[first + 2u].kind = POSIXIPC_KIND_BYTES;
    l->slots[first + 2u].align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_BYTES);
    l->slots[first + 2u].size = padded;
    l->slots[first + 2u].init_flags = (uint32_t)depth * 0x9e3779b1u ^ (uint32_t)item_size * 0x85ebca77u ^ 0x46555121u;
    if (l->slots[first + 2u].init_flags == 0u) {
        l->slots[first + 2u].init_flags = 1u;
    }
    handle = posixipc_futexq_new_unbound(st, (uint32_t)depth, (uint32_t)item_size, first);
    return handle == NULL ? NULL : layout_commit_span(l, handle, POSIXIPC_FUTEX_QUEUE_SLOTS);
}

static PyObject *layout_add(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *kind = NULL;
    PyObject *on_owner_died = NULL;
    PyObject *mutex = NULL;
    int prio_inherit = 0;
    int process_shared = 1;
    Py_ssize_t value = 1;
    Py_ssize_t parties = -1;
    Py_ssize_t depth = -1;
    Py_ssize_t item_size = -1;
    static char *kwlist[] = {"kind",  "on_owner_died", "prio_inherit", "process_shared", "mutex",
                             "value", "parties",       "depth",        "item_size",      NULL};
    uint32_t init_flags;
    PyObject *handle;
    posixipc_slot *slot;
    int mutex_index;

    if (st == NULL) {
        return NULL;
    }
    if (l->sealed) {
        PyErr_SetString(PyExc_RuntimeError, "layout is sealed");
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|$OppOnnnn:add", kwlist, &kind, &on_owner_died, &prio_inherit,
                                     &process_shared, &mutex, &value, &parties, &depth, &item_size)) {
        return NULL;
    }
    if (!process_shared) {
        PyErr_SetString(PyExc_ValueError, "Layout.add() always creates process-shared slots");
        return NULL;
    }
    if (st->Queue_Type != NULL && kind == (PyObject *)st->Queue_Type) {
        return layout_add_queue(l, st, depth, item_size, on_owner_died);
    }
    if (st->FutexQueue_Type != NULL && kind == (PyObject *)st->FutexQueue_Type) {
        return layout_add_futex_queue(l, st, depth, item_size, on_owner_died);
    }
    if (st->Futex_Type != NULL && kind == (PyObject *)st->Futex_Type) {
        PyErr_SetString(PyExc_TypeError, "Futex is not a Layout kind");
        return NULL;
    }
    if (st->EventFD_Type != NULL && kind == (PyObject *)st->EventFD_Type) {
        PyErr_SetString(PyExc_TypeError, "EventFD is not a Layout kind");
        return NULL;
    }
    if (st->MemFD_Type != NULL && kind == (PyObject *)st->MemFD_Type) {
        PyErr_SetString(PyExc_TypeError, "MemFD is not a Layout kind");
        return NULL;
    }
    if (st->NamedMessageQueue_Type != NULL && kind == (PyObject *)st->NamedMessageQueue_Type) {
        PyErr_SetString(PyExc_TypeError, "NamedMessageQueue is not a Layout kind; use create() or attach()");
        return NULL;
    }
    if (layout_reserve(l) < 0) {
        return NULL;
    }
    slot = &l->slots[l->nslots];
    memset(slot, 0, sizeof(*slot));

    if (kind == (PyObject *)st->Mutex_Type ||
        (PyType_Check(kind) && PyType_IsSubtype((PyTypeObject *)kind, st->Mutex_Type) &&
         !PyType_IsSubtype((PyTypeObject *)kind, st->RobustMutex_Type))) {
        if (on_owner_died != NULL && on_owner_died != Py_None) {
            PyErr_SetString(PyExc_TypeError, "Mutex does not take on_owner_died");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED;
        if (prio_inherit) {
            init_flags |= POSIXIPC_FLAG_PRIORITY_INHERIT;
        }
        slot->kind = POSIXIPC_KIND_MUTEX;
        slot->init_flags = init_flags;
        handle = posixipc_mutex_new_unbound(st, st->Mutex_Type, POSIXIPC_KIND_MUTEX, init_flags, NULL, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (kind == (PyObject *)st->RobustMutex_Type ||
        (PyType_Check(kind) && PyType_IsSubtype((PyTypeObject *)kind, st->RobustMutex_Type))) {
        if (on_owner_died == NULL || on_owner_died == Py_None) {
            PyErr_SetString(PyExc_TypeError, "RobustMutex requires on_owner_died");
            return NULL;
        }
        if (!PyCallable_Check(on_owner_died)) {
            PyErr_SetString(PyExc_TypeError, "on_owner_died must be callable");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED | POSIXIPC_FLAG_ROBUST;
        if (prio_inherit) {
            init_flags |= POSIXIPC_FLAG_PRIORITY_INHERIT;
        }
        slot->kind = POSIXIPC_KIND_ROBUST_MUTEX;
        slot->init_flags = init_flags;
        handle = posixipc_mutex_new_unbound(st, st->RobustMutex_Type, POSIXIPC_KIND_ROBUST_MUTEX, init_flags,
                                            on_owner_died, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (kind == (PyObject *)st->RWLock_Type) {
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED;
        slot->kind = POSIXIPC_KIND_RWLOCK;
        slot->init_flags = init_flags;
        handle = posixipc_rwlock_new_unbound(st, init_flags, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (kind == (PyObject *)st->Condition_Type) {
        if (mutex == NULL || mutex == Py_None) {
            PyErr_SetString(PyExc_TypeError, "Condition requires mutex=");
            return NULL;
        }
        if (!PyObject_TypeCheck(mutex, st->Mutex_Type)) {
            PyErr_SetString(PyExc_TypeError, "mutex must be a posixipc.Mutex");
            return NULL;
        }
        mutex_index = layout_mutex_index(l, mutex);
        if (mutex_index < 0) {
            PyErr_SetString(PyExc_ValueError, "Condition mutex must be added to the same Layout");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED | ((uint32_t)mutex_index << POSIXIPC_SLOT_AUX_SHIFT);
#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
        init_flags |= POSIXIPC_FLAG_MONOTONIC;
#endif
        slot->kind = POSIXIPC_KIND_COND;
        slot->init_flags = init_flags;
        handle = posixipc_cond_new_unbound(st, init_flags, l->nslots, mutex);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (kind == (PyObject *)st->Semaphore_Type) {
        if (value < 0 || value > 0xffff) {
            PyErr_SetString(PyExc_ValueError, "Semaphore value out of range");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED | ((uint32_t)value << POSIXIPC_SLOT_AUX_SHIFT);
        slot->kind = POSIXIPC_KIND_SEM;
        slot->init_flags = init_flags;
        handle = posixipc_sem_new_unbound(st, init_flags, (unsigned)value, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (st->Barrier_Type != NULL && kind == (PyObject *)st->Barrier_Type) {
        if (parties < 1 || parties > 0xffff) {
            PyErr_SetString(PyExc_TypeError, "Barrier requires parties=");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED | ((uint32_t)parties << POSIXIPC_SLOT_AUX_SHIFT);
        slot->kind = POSIXIPC_KIND_BARRIER;
        slot->init_flags = init_flags;
        handle = posixipc_barrier_new_unbound(st, init_flags, (unsigned)parties, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    if (st->SpinLock_Type != NULL && kind == (PyObject *)st->SpinLock_Type) {
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED;
        slot->kind = POSIXIPC_KIND_SPIN;
        slot->init_flags = init_flags;
        handle = posixipc_spin_new_unbound(st, init_flags, l->nslots);
        return handle == NULL ? NULL : layout_commit(l, handle, slot);
    }
    PyErr_SetString(PyExc_TypeError, "unsupported layout kind");
    return NULL;
}

static PyObject *layout_add_bytes(PyObject *self, PyObject *args)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    Py_ssize_t size;
    PyObject *handle;
    posixipc_slot *slot;
    uint32_t padded;

    if (st == NULL) {
        return NULL;
    }
    if (l->sealed) {
        PyErr_SetString(PyExc_RuntimeError, "layout is sealed");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "n:add_bytes", &size)) {
        return NULL;
    }
    if (size <= 0 || (size_t)size > UINT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return NULL;
    }
    padded = posixipc_kind_size(POSIXIPC_KIND_BYTES, (uint32_t)size);
    if (padded == 0) {
        PyErr_SetString(PyExc_ValueError, "size out of range");
        return NULL;
    }
    if (layout_reserve(l) < 0) {
        return NULL;
    }
    slot = &l->slots[l->nslots];
    memset(slot, 0, sizeof(*slot));
    slot->kind = POSIXIPC_KIND_BYTES;
    slot->align = (uint16_t)posixipc_kind_align(POSIXIPC_KIND_BYTES);
    slot->size = padded;
    handle = posixipc_bytes_new_unbound(st, padded, l->nslots);
    if (handle == NULL) {
        return NULL;
    }
    if (PyList_Append(l->handles, handle) < 0) {
        Py_DECREF(handle);
        return NULL;
    }
    l->nslots += 1;
    if (layout_sync(l) < 0) {
        Py_DECREF(handle);
        PySequence_DelItem(l->handles, PyList_GET_SIZE(l->handles) - 1);
        l->nslots -= 1;
        return NULL;
    }
    return handle;
}

static PyObject *layout_add_array(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *kind = NULL;
    PyObject *on_owner_died = NULL;
    int prio_inherit = 0;
    int process_shared = 1;
    Py_ssize_t count = 0;
    static char *kwlist[] = {"kind", "count", "on_owner_died", "prio_inherit", "process_shared", NULL};
    uint32_t init_flags;
    PyObject *handle;
    posixipc_slot *slot;
    uint16_t kind_code;

    if (st == NULL) {
        return NULL;
    }
    if (l->sealed) {
        PyErr_SetString(PyExc_RuntimeError, "layout is sealed");
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "On|$Opp:add_array", kwlist, &kind, &count, &on_owner_died,
                                     &prio_inherit, &process_shared)) {
        return NULL;
    }
    if (!process_shared) {
        PyErr_SetString(PyExc_ValueError, "Layout.add_array() always creates process-shared slots");
        return NULL;
    }
    if (count < 1 || count > 0xffff) {
        PyErr_SetString(PyExc_ValueError, "array count must be in 1..65535");
        return NULL;
    }
    if (kind == (PyObject *)st->Mutex_Type ||
        (PyType_Check(kind) && PyType_IsSubtype((PyTypeObject *)kind, st->Mutex_Type) &&
         !PyType_IsSubtype((PyTypeObject *)kind, st->RobustMutex_Type))) {
        if (on_owner_died != NULL && on_owner_died != Py_None) {
            PyErr_SetString(PyExc_TypeError, "Mutex does not take on_owner_died");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED;
        if (prio_inherit) {
            init_flags |= POSIXIPC_FLAG_PRIORITY_INHERIT;
        }
        kind_code = POSIXIPC_KIND_MUTEX;
        on_owner_died = NULL;
    } else if (kind == (PyObject *)st->RobustMutex_Type ||
               (PyType_Check(kind) && PyType_IsSubtype((PyTypeObject *)kind, st->RobustMutex_Type))) {
        if (on_owner_died == NULL || on_owner_died == Py_None) {
            PyErr_SetString(PyExc_TypeError, "RobustMutex requires on_owner_died");
            return NULL;
        }
        if (!PyCallable_Check(on_owner_died)) {
            PyErr_SetString(PyExc_TypeError, "on_owner_died must be callable");
            return NULL;
        }
        init_flags = POSIXIPC_FLAG_PROCESS_SHARED | POSIXIPC_FLAG_ROBUST;
        if (prio_inherit) {
            init_flags |= POSIXIPC_FLAG_PRIORITY_INHERIT;
        }
        kind_code = POSIXIPC_KIND_ROBUST_MUTEX;
    } else {
        PyErr_SetString(PyExc_TypeError, "add_array() supports Mutex and RobustMutex");
        return NULL;
    }
    if (layout_ensure(l, (uint32_t)l->nslots + (uint32_t)count) < 0) {
        return NULL;
    }
    slot = &l->slots[l->nslots];
    memset(slot, 0, sizeof(*slot));
    slot->kind = kind_code;
    slot->init_flags = init_flags;
    handle = posixipc_mutexarray_new_unbound(st, kind_code, init_flags, on_owner_died, l->nslots, (uint32_t)count);
    return handle == NULL ? NULL : layout_commit_n(l, handle, (uint16_t)count);
}

static void fail_created_shm(posixipc_shm *core, const char *name)
{
    posixipc_shm_mark_broken(core);
    posixipc_shm_close(core);
    posixipc_shm_unlink(name);
}

static PyObject *layout_create(PyObject *self, PyObject *args)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    const char *name;
    posixipc_shm_expect expect;
    posixipc_shm core;
    int rc;
    PyObject *region;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "s:create", &name)) {
        return NULL;
    }
    if (layout_sync(l) < 0) {
        return NULL;
    }
    rc = posixipc_layout_build(l->slots, l->nslots, &expect);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_create(name, &expect, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    if (layout_init_primitives(l, &core) < 0) {
        fail_created_shm(&core, name);
        return NULL;
    }
    if (layout_init_queue_ctrls(l, &core) < 0) {
        fail_created_shm(&core, name);
        return NULL;
    }
    rc = posixipc_shm_publish(&core);
    if (rc != 0) {
        fail_created_shm(&core, name);
        posixipc_err(st, rc);
        return NULL;
    }
    region = posixipc_shmobj_from_core(st, &core, 1);
    if (region == NULL) {
        posixipc_shm_unlink(name);
        return NULL;
    }
    if (layout_bind_all(l, (PosixIPCSharedMemoryObject *)region, 0) < 0) {
        Py_DECREF(region);
        return NULL;
    }
    l->sealed = 1;
    return region;
}

static PyObject *layout_attach(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    const char *name;
    PyObject *timeout_obj = NULL;
    static char *kwlist[] = {"name", "timeout", NULL};
    posixipc_shm_expect expect;
    posixipc_deadline dl;
    const posixipc_deadline *dlp;
    posixipc_shm core;
    int rc;
    PyObject *region;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|O:attach", kwlist, &name, &timeout_obj)) {
        return NULL;
    }
    if (layout_sync(l) < 0) {
        return NULL;
    }
    rc = posixipc_layout_build(l->slots, l->nslots, &expect);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    if (parse_attach_timeout(timeout_obj, &dl, &dlp) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, &expect, dlp, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    region = posixipc_shmobj_from_core(st, &core, 0);
    if (region == NULL) {
        return NULL;
    }
    if (layout_bind_all(l, (PosixIPCSharedMemoryObject *)region, 0) < 0) {
        Py_DECREF(region);
        return NULL;
    }
    l->sealed = 1;
    return region;
}

static PyObject *layout_open_or_create(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    const char *name;
    PyObject *timeout_obj = NULL;
    static char *kwlist[] = {"name", "timeout", NULL};
    posixipc_shm_expect expect;
    posixipc_deadline dl;
    const posixipc_deadline *dlp;
    posixipc_shm core;
    int rc;
    int created = 0;
    PyObject *region;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|O:open_or_create", kwlist, &name, &timeout_obj)) {
        return NULL;
    }
    if (layout_sync(l) < 0) {
        return NULL;
    }
    rc = posixipc_layout_build(l->slots, l->nslots, &expect);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    if (parse_attach_timeout(timeout_obj, &dl, &dlp) < 0) {
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_open_or_create(name, &expect, dlp, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    if (atomic_load_explicit(&core.hdr->state, memory_order_acquire) == POSIXIPC_STATE_INITIALIZING) {
        created = 1;
        if (layout_init_primitives(l, &core) < 0) {
            fail_created_shm(&core, name);
            return NULL;
        }
        if (layout_init_queue_ctrls(l, &core) < 0) {
            fail_created_shm(&core, name);
            return NULL;
        }
        rc = posixipc_shm_publish(&core);
        if (rc != 0) {
            fail_created_shm(&core, name);
            posixipc_err(st, rc);
            return NULL;
        }
    }
    region = posixipc_shmobj_from_core(st, &core, created);
    if (region == NULL) {
        if (created) {
            posixipc_shm_unlink(name);
        }
        return NULL;
    }
    if (layout_bind_all(l, (PosixIPCSharedMemoryObject *)region, 0) < 0) {
        Py_DECREF(region);
        return NULL;
    }
    l->sealed = 1;
    return region;
}

static PyObject *layout_get_digest(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    posixipc_shm_expect expect;
    int rc;

    rc = posixipc_layout_build(l->slots, l->nslots, &expect);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    return PyLong_FromUnsignedLong(expect.layout_digest);
}

static PyObject *layout_get_slots(PyObject *self, void *Py_UNUSED(c))
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    posixipc_state *st = posixipc_state_from_obj(self);
    PyObject *out;
    uint16_t i;

    if (layout_sync(l) < 0) {
        return NULL;
    }
    (void)st;
    out = PyList_New(l->nslots);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < l->nslots; i++) {
        PyObject *item = Py_BuildValue("(HHIII)", l->slots[i].kind, l->slots[i].align, l->slots[i].offset,
                                       l->slots[i].size, l->slots[i].init_flags);

        if (item == NULL) {
            Py_DECREF(out);
            return NULL;
        }
        PyList_SET_ITEM(out, i, item);
    }
    return out;
}

static int layout_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;

    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kw, "|:Layout", kwlist)) {
        return -1;
    }
    if (l->handles != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Layout is already initialized");
        return -1;
    }
    l->handles = PyList_New(0);
    if (l->handles == NULL) {
        return -1;
    }
    l->slots = NULL;
    l->nslots = 0;
    l->cap = 0;
    l->sealed = 0;
    return 0;
}

static int layout_traverse(PyObject *self, visitproc visit, void *arg)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;

    Py_VISIT(Py_TYPE(self));
    Py_VISIT(l->handles);
    return 0;
}

static int layout_clear(PyObject *self)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;

    Py_CLEAR(l->handles);
    return 0;
}

static void layout_dealloc(PyObject *self)
{
    PosixIPCLayoutObject *l = (PosixIPCLayoutObject *)self;
    PyTypeObject *tp = Py_TYPE(self);

    PyObject_GC_UnTrack(self);
    Py_CLEAR(l->handles);
    PyMem_Free(l->slots);
    l->slots = NULL;
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyMethodDef layout_methods[] = {
    {"add", POSIXIPC_METH(layout_add), METH_VARARGS | METH_KEYWORDS, NULL},
    {"add_array", POSIXIPC_METH(layout_add_array), METH_VARARGS | METH_KEYWORDS, NULL},
    {"add_bytes", layout_add_bytes, METH_VARARGS, NULL},
    {"create", layout_create, METH_VARARGS, NULL},
    {"attach", POSIXIPC_METH(layout_attach), METH_VARARGS | METH_KEYWORDS, NULL},
    {"open_or_create", POSIXIPC_METH(layout_open_or_create), METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef layout_getset[] = {
    {"digest", layout_get_digest, NULL, NULL, NULL},
    {"slots", layout_get_slots, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot layout_slots[] = {
    {Py_tp_dealloc, layout_dealloc}, {Py_tp_traverse, layout_traverse},
    {Py_tp_clear, layout_clear},     {Py_tp_init, layout_init},
    {Py_tp_methods, layout_methods}, {Py_tp_getset, layout_getset},
    {Py_tp_new, PyType_GenericNew},  {0, NULL},
};

static PyType_Spec layout_spec = {
    .name = "posixipc.Layout",
    .basicsize = sizeof(PosixIPCLayoutObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = layout_slots,
};

static PyObject *mod_attach_slot(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    const char *name;
    unsigned int slot;
    unsigned int kind;
    unsigned int digest;
    posixipc_deadline dl;
    posixipc_shm core;
    int rc;
    posixipc_shm_header *hdr;
    posixipc_slot *dir;
    PyObject *region;
    PyObject *handle;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "sIII:_attach_slot", &name, &slot, &kind, &digest)) {
        return NULL;
    }
    rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 5.0, &dl);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, NULL, &dl, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    hdr = core.hdr;
    if (hdr == NULL || hdr->layout_digest != digest || slot >= hdr->slot_count) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled slot does not match segment");
        return NULL;
    }
    dir = posixipc_shm_directory(&core);
    if (dir == NULL || dir[slot].kind != kind) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled slot kind does not match segment");
        return NULL;
    }
    region = posixipc_shmobj_from_core(st, &core, 0);
    if (region == NULL) {
        return NULL;
    }
    if (kind == POSIXIPC_KIND_MUTEX || kind == POSIXIPC_KIND_ROBUST_MUTEX) {
        PyTypeObject *type = kind == POSIXIPC_KIND_ROBUST_MUTEX ? st->RobustMutex_Type : st->Mutex_Type;

        handle = posixipc_mutex_new_unbound(st, type, (uint16_t)kind, dir[slot].init_flags, NULL, slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_mutex_bind((PosixIPCMutexObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot], slot,
                                0) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_BYTES) {
        handle = posixipc_bytes_new_unbound(st, dir[slot].size, slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_bytes_bind((PosixIPCBytesObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot], slot) <
            0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_RWLOCK) {
        handle = posixipc_rwlock_new_unbound(st, dir[slot].init_flags, slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_rwlock_bind((PosixIPCRWLockObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot],
                                 slot) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_COND) {
        unsigned int mutex_slot = dir[slot].init_flags >> POSIXIPC_SLOT_AUX_SHIFT;
        PyObject *mutex;

        if (mutex_slot >= hdr->slot_count ||
            (dir[mutex_slot].kind != POSIXIPC_KIND_MUTEX && dir[mutex_slot].kind != POSIXIPC_KIND_ROBUST_MUTEX)) {
            Py_DECREF(region);
            PyErr_SetString(st->exc_LayoutMismatchError, "condition mutex slot is invalid");
            return NULL;
        }
        mutex = posixipc_mutex_new_unbound(
            st, dir[mutex_slot].kind == POSIXIPC_KIND_ROBUST_MUTEX ? st->RobustMutex_Type : st->Mutex_Type,
            dir[mutex_slot].kind, dir[mutex_slot].init_flags, NULL, mutex_slot);
        if (mutex == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_mutex_bind((PosixIPCMutexObject *)mutex, (PosixIPCSharedMemoryObject *)region, &dir[mutex_slot],
                                mutex_slot, 0) < 0) {
            Py_DECREF(mutex);
            Py_DECREF(region);
            return NULL;
        }
        handle = posixipc_cond_new_unbound(st, dir[slot].init_flags, slot, mutex);
        Py_DECREF(mutex);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_cond_bind((PosixIPCConditionObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot],
                               slot) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_SEM) {
        handle =
            posixipc_sem_new_unbound(st, dir[slot].init_flags, dir[slot].init_flags >> POSIXIPC_SLOT_AUX_SHIFT, slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_sem_bind((PosixIPCSemaphoreObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot],
                              slot) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_BARRIER && st->Barrier_Type != NULL) {
        handle = posixipc_barrier_new_unbound(st, dir[slot].init_flags, dir[slot].init_flags >> POSIXIPC_SLOT_AUX_SHIFT,
                                              slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_barrier_bind((PosixIPCBarrierObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot],
                                  slot) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    if (kind == POSIXIPC_KIND_SPIN && st->SpinLock_Type != NULL) {
        handle = posixipc_spin_new_unbound(st, dir[slot].init_flags, slot);
        if (handle == NULL) {
            Py_DECREF(region);
            return NULL;
        }
        if (posixipc_spin_bind((PosixIPCSpinLockObject *)handle, (PosixIPCSharedMemoryObject *)region, &dir[slot],
                               slot) < 0) {
            Py_DECREF(handle);
            Py_DECREF(region);
            return NULL;
        }
        Py_DECREF(region);
        return handle;
    }
    Py_DECREF(region);
    PyErr_SetString(PyExc_TypeError, "unsupported pickled kind");
    return NULL;
}

static PyObject *mod_layout_digest(PyObject *mod, PyObject *args)
{
    unsigned int version;
    unsigned int abi_tag;
    PyObject *seq;
    Py_ssize_t n;
    Py_ssize_t i;
    posixipc_slot *slots;
    uint32_t digest;
    uint16_t nslots;

    (void)mod;
    if (!PyArg_ParseTuple(args, "IIO:_layout_digest", &version, &abi_tag, &seq)) {
        return NULL;
    }
    n = PySequence_Size(seq);
    if (n < 0) {
        return NULL;
    }
    if (n > 0xffff) {
        PyErr_SetString(PyExc_OverflowError, "too many slots");
        return NULL;
    }
    nslots = (uint16_t)n;
    slots = (posixipc_slot *)PyMem_Calloc((size_t)nslots, sizeof(*slots));
    if (slots == NULL && nslots != 0) {
        return PyErr_NoMemory();
    }
    for (i = 0; i < n; i++) {
        PyObject *item = PySequence_GetItem(seq, i);
        unsigned int kind;
        unsigned int align;
        unsigned int size;
        unsigned int init_flags;

        if (item == NULL) {
            PyMem_Free(slots);
            return NULL;
        }
        if (!PyArg_ParseTuple(item, "IIII", &kind, &align, &size, &init_flags)) {
            Py_DECREF(item);
            PyMem_Free(slots);
            return NULL;
        }
        Py_DECREF(item);
        slots[i].kind = (uint16_t)kind;
        slots[i].align = (uint16_t)align;
        slots[i].size = size;
        slots[i].init_flags = init_flags;
    }
    digest = posixipc_layout_digest((uint16_t)version, abi_tag, slots, nslots);
    PyMem_Free(slots);
    return PyLong_FromUnsignedLong(digest);
}

static PyObject *mod_attach_array(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    const char *name;
    unsigned int slot;
    unsigned int count;
    unsigned int kind;
    unsigned int digest;
    posixipc_deadline dl;
    posixipc_shm core;
    int rc;
    posixipc_shm_header *hdr;
    posixipc_slot *dir;
    PyObject *region;
    PyObject *handle;
    unsigned int i;

    if (st == NULL) {
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "sIIII:_attach_array", &name, &slot, &count, &kind, &digest)) {
        return NULL;
    }
    if (count < 1) {
        PyErr_SetString(PyExc_ValueError, "array count must be positive");
        return NULL;
    }
    rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 5.0, &dl);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, NULL, &dl, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    hdr = core.hdr;
    if (hdr == NULL || hdr->layout_digest != digest || slot >= hdr->slot_count ||
        (uint32_t)slot + count > hdr->slot_count) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled array does not match segment");
        return NULL;
    }
    dir = posixipc_shm_directory(&core);
    if (dir == NULL) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled array does not match segment");
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (dir[slot + i].kind != kind) {
            posixipc_shm_close(&core);
            PyErr_SetString(st->exc_LayoutMismatchError, "pickled array kind does not match segment");
            return NULL;
        }
    }
    region = posixipc_shmobj_from_core(st, &core, 0);
    if (region == NULL) {
        return NULL;
    }
    handle = posixipc_mutexarray_new_unbound(st, (uint16_t)kind, dir[slot].init_flags, NULL, slot, count);
    if (handle == NULL) {
        Py_DECREF(region);
        return NULL;
    }
    if (posixipc_mutexarray_bind((PosixIPCMutexArrayObject *)handle, (PosixIPCSharedMemoryObject *)region, dir, slot,
                                 count) < 0) {
        Py_DECREF(handle);
        Py_DECREF(region);
        return NULL;
    }
    Py_DECREF(region);
    return handle;
}

static PyObject *mod_attach_queue(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    const char *name;
    unsigned int first;
    unsigned int depth;
    unsigned int item_size;
    unsigned int digest;
    posixipc_deadline dl;
    posixipc_shm core;
    int rc;
    posixipc_shm_header *hdr;
    posixipc_slot *dir;
    PyObject *region;
    PyObject *handle;
    uint32_t need;

    if (st == NULL || st->Queue_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Queue type is missing");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "sIIII:_attach_queue", &name, &first, &depth, &item_size, &digest)) {
        return NULL;
    }
    if (depth < 1u || item_size < 1u) {
        PyErr_SetString(PyExc_ValueError, "Queue depth and item_size must be >= 1");
        return NULL;
    }
    rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 5.0, &dl);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, NULL, &dl, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    hdr = core.hdr;
    if (hdr == NULL || hdr->layout_digest != digest || (uint32_t)first + POSIXIPC_QUEUE_SLOTS > hdr->slot_count) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled Queue does not match segment");
        return NULL;
    }
    dir = posixipc_shm_directory(&core);
    if (dir == NULL || dir[first].kind != POSIXIPC_KIND_ROBUST_MUTEX ||
        dir[first + 1u].kind != POSIXIPC_KIND_ROBUST_MUTEX || dir[first + 2u].kind != POSIXIPC_KIND_COND ||
        dir[first + 3u].kind != POSIXIPC_KIND_COND || dir[first + 4u].kind != POSIXIPC_KIND_BYTES) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled Queue slot sequence does not match");
        return NULL;
    }
    need = posixipc_queue_bytes_size(depth, item_size);
    if (need == 0 || dir[first + 4u].size < need) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled Queue size does not match segment");
        return NULL;
    }
    region = posixipc_shmobj_from_core(st, &core, 0);
    if (region == NULL) {
        return NULL;
    }
    handle = posixipc_queue_new_unbound(st, depth, item_size, first);
    if (handle == NULL) {
        Py_DECREF(region);
        return NULL;
    }
    if (posixipc_queue_bind((PosixIPCQueueObject *)handle, (PosixIPCSharedMemoryObject *)region, dir, first) < 0) {
        Py_DECREF(handle);
        Py_DECREF(region);
        return NULL;
    }
    Py_DECREF(region);
    return handle;
}

static PyObject *mod_attach_futex_queue(PyObject *mod, PyObject *args)
{
    posixipc_state *st = posixipc_get_state(mod);
    const char *name;
    unsigned int first;
    unsigned int depth;
    unsigned int item_size;
    unsigned int digest;
    posixipc_deadline dl;
    posixipc_shm core;
    int rc;
    posixipc_shm_header *hdr;
    posixipc_slot *dir;
    PyObject *region;
    PyObject *handle;
    uint32_t need;

    if (st == NULL || st->FutexQueue_Type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "FutexQueue type is missing");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "sIIII:_attach_futex_queue", &name, &first, &depth, &item_size, &digest)) {
        return NULL;
    }
    if (depth < 1u || item_size < 1u) {
        PyErr_SetString(PyExc_ValueError, "FutexQueue depth and item_size must be >= 1");
        return NULL;
    }
    rc = posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 5.0, &dl);
    if (rc != 0) {
        posixipc_err(st, rc);
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS rc = posixipc_shm_attach(name, NULL, &dl, &core);
    Py_END_ALLOW_THREADS if (rc != 0)
    {
        posixipc_err(st, rc);
        return NULL;
    }
    hdr = core.hdr;
    if (hdr == NULL || hdr->layout_digest != digest || (uint32_t)first + POSIXIPC_FUTEX_QUEUE_SLOTS > hdr->slot_count) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled FutexQueue does not match segment");
        return NULL;
    }
    dir = posixipc_shm_directory(&core);
    if (dir == NULL || dir[first].kind != POSIXIPC_KIND_ROBUST_MUTEX ||
        dir[first + 1u].kind != POSIXIPC_KIND_ROBUST_MUTEX || dir[first + 2u].kind != POSIXIPC_KIND_BYTES) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled FutexQueue slot sequence does not match");
        return NULL;
    }
    need = posixipc_futexq_bytes_size(depth, item_size);
    if (need == 0 || dir[first + 2u].size < need) {
        posixipc_shm_close(&core);
        PyErr_SetString(st->exc_LayoutMismatchError, "pickled FutexQueue size does not match segment");
        return NULL;
    }
    region = posixipc_shmobj_from_core(st, &core, 0);
    if (region == NULL) {
        return NULL;
    }
    handle = posixipc_futexq_new_unbound(st, depth, item_size, first);
    if (handle == NULL) {
        Py_DECREF(region);
        return NULL;
    }
    if (posixipc_futexq_bind((PosixIPCFutexQueueObject *)handle, (PosixIPCSharedMemoryObject *)region, dir, first) <
        0) {
        Py_DECREF(handle);
        Py_DECREF(region);
        return NULL;
    }
    Py_DECREF(region);
    return handle;
}

int posixipc_bytes_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;

    type = PyType_FromModuleAndSpec(mod, &bytes_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Bytes_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "SharedBytes", type) < 0) {
        return -1;
    }
    return 0;
}

int posixipc_layout_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;
    static PyMethodDef extra[] = {
        {"_attach_slot", mod_attach_slot, METH_VARARGS, NULL},
        {"_attach_array", mod_attach_array, METH_VARARGS, NULL},
        {"_attach_queue", mod_attach_queue, METH_VARARGS, NULL},
        {"_attach_futex_queue", mod_attach_futex_queue, METH_VARARGS, NULL},
        {"_layout_digest", mod_layout_digest, METH_VARARGS, NULL},
        {NULL, NULL, 0, NULL},
    };
    PyMethodDef *m;

    if (posixipc_bytes_register(mod, st) < 0) {
        return -1;
    }
    type = PyType_FromModuleAndSpec(mod, &layout_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Layout_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Layout", type) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "LAYOUT_VERSION", (long)POSIXIPC_LAYOUT_VERSION) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "CACHELINE_BYTES", (long)POSIXIPC_CACHELINE_BYTES) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_BYTES", (long)POSIXIPC_KIND_BYTES) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_MUTEX", (long)POSIXIPC_KIND_MUTEX) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_ROBUST_MUTEX", (long)POSIXIPC_KIND_ROBUST_MUTEX) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_RWLOCK", (long)POSIXIPC_KIND_RWLOCK) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_COND", (long)POSIXIPC_KIND_COND) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_SEM", (long)POSIXIPC_KIND_SEM) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_BARRIER", (long)POSIXIPC_KIND_BARRIER) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "KIND_SPIN", (long)POSIXIPC_KIND_SPIN) < 0) {
        return -1;
    }
    if (PyModule_AddIntConstant(mod, "abi_tag", (long)posixipc_abi_tag()) < 0) {
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
