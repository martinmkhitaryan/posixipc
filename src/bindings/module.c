#include "posixipc_config.h"
#include "posixipc_clocksym.h"
#include "py_internal.h"

static int posixipc_dict_set_bool(PyObject *d, const char *key, int value);
static int posixipc_dict_set_ulong(PyObject *d, const char *key, unsigned long value);
static int posixipc_dict_set_string(PyObject *d, const char *key, const char *value);
static PyObject *posixipc_make_build_info(void);
static PyObject *posixipc_make_monotonic_timeouts(void);

static int posixipc_dict_set_bool(PyObject *d, const char *key, int value)
{
    PyObject *obj = value ? Py_True : Py_False;
    return PyDict_SetItemString(d, key, obj);
}

static int posixipc_dict_set_ulong(PyObject *d, const char *key, unsigned long value)
{
    PyObject *obj = PyLong_FromUnsignedLong(value);
    int rc;

    if (obj == NULL) {
        return -1;
    }
    rc = PyDict_SetItemString(d, key, obj);
    Py_DECREF(obj);
    return rc;
}

static int posixipc_dict_set_string(PyObject *d, const char *key, const char *value)
{
    PyObject *obj = PyUnicode_FromString(value);
    int rc;

    if (obj == NULL) {
        return -1;
    }
    rc = PyDict_SetItemString(d, key, obj);
    Py_DECREF(obj);
    return rc;
}

static PyObject *posixipc_make_build_info(void)
{
    PyObject *info = PyDict_New();

    if (info == NULL) {
        return NULL;
    }

    if (posixipc_dict_set_bool(info, "have_robust_mutex", POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETROBUST) < 0 ||
        posixipc_dict_set_bool(info, "have_process_shared", POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPSHARED) < 0 ||
        posixipc_dict_set_bool(info, "have_prio_inherit", POSIXIPC_HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL) < 0 ||
        posixipc_dict_set_bool(info, "have_barrier", POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT) < 0 ||
        posixipc_dict_set_bool(info, "have_spinlock", POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK) < 0 ||
        posixipc_dict_set_bool(info, "have_named_semaphore", POSIXIPC_HAVE_SEM_OPEN) < 0 ||
        posixipc_dict_set_bool(info, "have_cond_monotonic", POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK) < 0 ||
        posixipc_dict_set_bool(info, "have_memfd", POSIXIPC_HAVE_MEMFD_CREATE) < 0 ||
        posixipc_dict_set_bool(info, "have_eventfd", POSIXIPC_HAVE_EVENTFD) < 0 ||
        posixipc_dict_set_bool(info, "have_futex", POSIXIPC_HAVE_FUTEX) < 0 ||
        posixipc_dict_set_bool(info, "have_mq", POSIXIPC_HAVE_MQ_OPEN) < 0 ||
        posixipc_dict_set_bool(info, "have_mutex_clocklock", POSIXIPC_HAVE_PTHREAD_MUTEX_CLOCKLOCK) < 0 ||
        posixipc_dict_set_bool(info, "have_rwlock_clocklock", POSIXIPC_HAVE_PTHREAD_RWLOCK_CLOCKRDLOCK) < 0 ||
        posixipc_dict_set_bool(info, "have_sem_clockwait", POSIXIPC_HAVE_SEM_CLOCKWAIT) < 0 ||
        posixipc_dict_set_bool(info, "have_shm_open", POSIXIPC_HAVE_SHM_OPEN) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_pthread_mutex_t", POSIXIPC_SIZEOF_PTHREAD_MUTEX_T) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_pthread_rwlock_t", POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_pthread_cond_t", POSIXIPC_SIZEOF_PTHREAD_COND_T) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_pthread_barrier_t", POSIXIPC_SIZEOF_PTHREAD_BARRIER_T) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_pthread_spinlock_t", POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T) < 0 ||
        posixipc_dict_set_ulong(info, "sizeof_sem_t", POSIXIPC_SIZEOF_SEM_T) < 0 ||
        posixipc_dict_set_ulong(info, "cacheline_bytes", POSIXIPC_CACHELINE_BYTES) < 0 ||
        posixipc_dict_set_ulong(info, "abi_tag_seed", POSIXIPC_ABI_TAG_SEED) < 0 ||
        posixipc_dict_set_ulong(info, "libc_family", POSIXIPC_LIBC_FAMILY) < 0) {
        Py_DECREF(info);
        return NULL;
    }

#if POSIXIPC_LIBC_FAMILY == 1
    if (posixipc_dict_set_string(info, "libc", "glibc") < 0) {
        Py_DECREF(info);
        return NULL;
    }
#elif POSIXIPC_LIBC_FAMILY == 2
    if (posixipc_dict_set_string(info, "libc", "musl") < 0) {
        Py_DECREF(info);
        return NULL;
    }
#else
    if (posixipc_dict_set_string(info, "libc", "other") < 0) {
        Py_DECREF(info);
        return NULL;
    }
#endif

    return info;
}

static PyObject *posixipc_make_monotonic_timeouts(void)
{
    PyObject *d = PyDict_New();

    if (d == NULL) {
        return NULL;
    }
    if (posixipc_dict_set_bool(d, "mutex", posixipc_have_mutex_clocklock()) < 0 ||
        posixipc_dict_set_bool(d, "rwlock", posixipc_have_rwlock_clocklock()) < 0 ||
        posixipc_dict_set_bool(d, "semaphore", posixipc_have_sem_clockwait()) < 0 ||
        posixipc_dict_set_bool(d, "condition", POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK) < 0) {
        Py_DECREF(d);
        return NULL;
    }
    return d;
}

static PyObject *posixipc_empty_call(PyObject *Py_UNUSED(mod), PyObject *Py_UNUSED(args))
{
    Py_RETURN_NONE;
}

static PyMethodDef posixipc_empty_call_def = {
    "_empty_call",
    posixipc_empty_call,
    METH_NOARGS,
    NULL,
};

static int posixipc_mod_exec(PyObject *mod)
{
    posixipc_state *st = posixipc_get_state(mod);
    PyObject *exc_mod;

    if (st == NULL) {
        return -1;
    }

    posixipc_clocksym_init();

    exc_mod = PyImport_ImportModule("posixipc.exceptions");
    if (exc_mod == NULL) {
        return -1;
    }
    st->exc_PosixIPCError = PyObject_GetAttrString(exc_mod, "PosixIPCError");
    st->exc_NotRecoverableError = PyObject_GetAttrString(exc_mod, "NotRecoverableError");
    st->exc_ClosedError = PyObject_GetAttrString(exc_mod, "ClosedError");
    st->exc_LayoutMismatchError = PyObject_GetAttrString(exc_mod, "LayoutMismatchError");
    Py_DECREF(exc_mod);
    if (st->exc_PosixIPCError == NULL || st->exc_NotRecoverableError == NULL || st->exc_ClosedError == NULL ||
        st->exc_LayoutMismatchError == NULL) {
        return -1;
    }

    st->build_info = posixipc_make_build_info();
    if (st->build_info == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(mod, "__build_info__", st->build_info) < 0) {
        return -1;
    }

    st->monotonic_timeouts = posixipc_make_monotonic_timeouts();
    if (st->monotonic_timeouts == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(mod, "_monotonic_timeouts", st->monotonic_timeouts) < 0) {
        return -1;
    }

    if (posixipc_shm_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_mutex_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_sync_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_m3_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_array_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_queue_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_linux_register(mod, st) < 0) {
        return -1;
    }
    if (posixipc_layout_register(mod, st) < 0) {
        return -1;
    }
    {
        PyObject *fn = PyCFunction_New(&posixipc_empty_call_def, mod);

        if (fn == NULL) {
            return -1;
        }
        if (PyModule_AddObject(mod, "_empty_call", fn) < 0) {
            Py_DECREF(fn);
            return -1;
        }
    }
    return 0;
}

static int posixipc_mod_traverse(PyObject *mod, visitproc visit, void *arg)
{
    posixipc_state *st = posixipc_get_state(mod);

    if (st == NULL) {
        return 0;
    }
    Py_VISIT(st->build_info);
    Py_VISIT(st->monotonic_timeouts);
    Py_VISIT(st->exc_PosixIPCError);
    Py_VISIT(st->exc_NotRecoverableError);
    Py_VISIT(st->exc_ClosedError);
    Py_VISIT(st->exc_LayoutMismatchError);
    Py_VISIT(st->SharedMemory_Type);
    Py_VISIT(st->Mutex_Type);
    Py_VISIT(st->RobustMutex_Type);
    Py_VISIT(st->Layout_Type);
    Py_VISIT(st->Bytes_Type);
    Py_VISIT(st->RWLock_Type);
    Py_VISIT(st->RWLockCM_Type);
    Py_VISIT(st->Condition_Type);
    Py_VISIT(st->Semaphore_Type);
    Py_VISIT(st->Barrier_Type);
    Py_VISIT(st->SpinLock_Type);
    Py_VISIT(st->NamedSemaphore_Type);
    Py_VISIT(st->MutexArray_Type);
    Py_VISIT(st->MutexArrayItem_Type);
    Py_VISIT(st->Queue_Type);
    Py_VISIT(st->NamedMessageQueue_Type);
    Py_VISIT(st->Futex_Type);
    Py_VISIT(st->EventFD_Type);
    Py_VISIT(st->MemFD_Type);
    Py_VISIT(st->FutexQueue_Type);
    return 0;
}

static int posixipc_mod_clear(PyObject *mod)
{
    posixipc_state *st = posixipc_get_state(mod);

    if (st == NULL) {
        return 0;
    }
    Py_CLEAR(st->build_info);
    Py_CLEAR(st->monotonic_timeouts);
    Py_CLEAR(st->exc_PosixIPCError);
    Py_CLEAR(st->exc_NotRecoverableError);
    Py_CLEAR(st->exc_ClosedError);
    Py_CLEAR(st->exc_LayoutMismatchError);
    Py_CLEAR(st->SharedMemory_Type);
    Py_CLEAR(st->Mutex_Type);
    Py_CLEAR(st->RobustMutex_Type);
    Py_CLEAR(st->Layout_Type);
    Py_CLEAR(st->Bytes_Type);
    Py_CLEAR(st->RWLock_Type);
    Py_CLEAR(st->RWLockCM_Type);
    Py_CLEAR(st->Condition_Type);
    Py_CLEAR(st->Semaphore_Type);
    Py_CLEAR(st->Barrier_Type);
    Py_CLEAR(st->SpinLock_Type);
    Py_CLEAR(st->NamedSemaphore_Type);
    Py_CLEAR(st->MutexArray_Type);
    Py_CLEAR(st->MutexArrayItem_Type);
    Py_CLEAR(st->Queue_Type);
    Py_CLEAR(st->NamedMessageQueue_Type);
    Py_CLEAR(st->Futex_Type);
    Py_CLEAR(st->EventFD_Type);
    Py_CLEAR(st->MemFD_Type);
    Py_CLEAR(st->FutexQueue_Type);
    return 0;
}

static void posixipc_mod_free(void *mod)
{
    (void)posixipc_mod_clear((PyObject *)mod);
}

static PyModuleDef_Slot posixipc_slots[] = {
    {Py_mod_exec, posixipc_mod_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL},
};

static PyModuleDef posixipc_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_posixipc",
    .m_doc = "posixipc C extension",
    .m_size = sizeof(posixipc_state),
    .m_slots = posixipc_slots,
    .m_traverse = posixipc_mod_traverse,
    .m_clear = posixipc_mod_clear,
    .m_free = posixipc_mod_free,
};

PyMODINIT_FUNC PyInit__posixipc(void);

PyMODINIT_FUNC PyInit__posixipc(void)
{
    return PyModuleDef_Init(&posixipc_module);
}
