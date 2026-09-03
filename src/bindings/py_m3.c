#include "py_internal.h"

#include "posixipc_config.h"
#include "posixipc_result.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
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

static int pin_region(PosixIPCSharedMemoryObject *r)
{
    if (posixipc_shmobj_pin(r) < 0) {
        return -1;
    }
    Py_INCREF(r);
    return 0;
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

#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
#include "posixipc_m3_barrier.inc"
#endif
#if POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK
#include "posixipc_m3_spin.inc"
#endif
#if POSIXIPC_HAVE_SEM_OPEN
#include "posixipc_m3_nsem.inc"
#endif

#if !POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
PyObject *posixipc_barrier_new_unbound(posixipc_state *st, uint32_t init_flags, unsigned parties, uint32_t slot)
{
    (void)st;
    (void)init_flags;
    (void)parties;
    (void)slot;
    PyErr_SetString(PyExc_NotImplementedError, "Barrier is not available");
    return NULL;
}

int posixipc_barrier_bind(PosixIPCBarrierObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                          uint32_t index)
{
    (void)o;
    (void)r;
    (void)slot;
    (void)index;
    PyErr_SetString(PyExc_NotImplementedError, "Barrier is not available");
    return -1;
}
#endif

#if !POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK
PyObject *posixipc_spin_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot)
{
    (void)st;
    (void)init_flags;
    (void)slot;
    PyErr_SetString(PyExc_NotImplementedError, "SpinLock is not available");
    return NULL;
}

int posixipc_spin_bind(PosixIPCSpinLockObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                       uint32_t index)
{
    (void)o;
    (void)r;
    (void)slot;
    (void)index;
    PyErr_SetString(PyExc_NotImplementedError, "SpinLock is not available");
    return -1;
}
#endif

int posixipc_m3_register(PyObject *mod, posixipc_state *st)
{
    PyObject *type;

    st->Barrier_Type = NULL;
    st->SpinLock_Type = NULL;
    st->NamedSemaphore_Type = NULL;

#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    type = PyType_FromModuleAndSpec(mod, &bar_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->Barrier_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "Barrier", type) < 0) {
        return -1;
    }
#endif
#if POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK
    type = PyType_FromModuleAndSpec(mod, &spin_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->SpinLock_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "SpinLock", type) < 0) {
        return -1;
    }
#endif
#if POSIXIPC_HAVE_SEM_OPEN
    type = PyType_FromModuleAndSpec(mod, &nsem_spec, NULL);
    if (type == NULL) {
        return -1;
    }
    st->NamedSemaphore_Type = (PyTypeObject *)type;
    if (PyModule_AddObjectRef(mod, "NamedSemaphore", type) < 0) {
        return -1;
    }
#endif
    return 0;
}
