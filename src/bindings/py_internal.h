#ifndef POSIXIPC_PY_INTERNAL_H
#define POSIXIPC_PY_INTERNAL_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdatomic.h>
#include <pthread.h>

#define POSIXIPC_METH(fn) ((PyCFunction)(void (*)(void))(fn))

#include "posixipc_layout.h"
#include "posixipc_mutex.h"
#include "posixipc_rwlock.h"
#include "posixipc_cond.h"
#include "posixipc_sem.h"
#include "posixipc_barrier.h"
#include "posixipc_spin.h"
#include "posixipc_nsem.h"
#include "posixipc_queue.h"
#include "posixipc_futexq.h"
#include "posixipc_result.h"
#include "posixipc_shm.h"
#include "posixipc_time.h"

typedef struct
{
    PyObject *build_info;
    PyObject *monotonic_timeouts;
    PyObject *exc_PosixIPCError;
    PyObject *exc_NotRecoverableError;
    PyObject *exc_ClosedError;
    PyObject *exc_LayoutMismatchError;
    PyTypeObject *SharedMemory_Type;
    PyTypeObject *Mutex_Type;
    PyTypeObject *RobustMutex_Type;
    PyTypeObject *Layout_Type;
    PyTypeObject *Bytes_Type;
    PyTypeObject *RWLock_Type;
    PyTypeObject *RWLockCM_Type;
    PyTypeObject *Condition_Type;
    PyTypeObject *Semaphore_Type;
    PyTypeObject *Barrier_Type;
    PyTypeObject *SpinLock_Type;
    PyTypeObject *NamedSemaphore_Type;
    PyTypeObject *MutexArray_Type;
    PyTypeObject *MutexArrayItem_Type;
    PyTypeObject *Queue_Type;
    PyTypeObject *NamedMessageQueue_Type;
    PyTypeObject *Futex_Type;
    PyTypeObject *EventFD_Type;
    PyTypeObject *MemFD_Type;
    PyTypeObject *FutexQueue_Type;
} posixipc_state;

typedef struct
{
    PyObject_HEAD posixipc_shm core;
    _Atomic uint32_t pins;
    _Atomic uint32_t flags;
    int created;
} PosixIPCSharedMemoryObject;

typedef struct
{
    PyObject_HEAD pthread_mutex_t *lock;
    PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    _Atomic int locked;
    pthread_mutex_t inline_storage;
    PyObject *on_owner_died;
} PosixIPCMutexObject;

typedef struct
{
    PyObject_HEAD PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
} PosixIPCBytesObject;

typedef struct
{
    PyObject_HEAD PyObject *handles;
    posixipc_slot *slots;
    uint16_t nslots;
    uint16_t cap;
    int sealed;
} PosixIPCLayoutObject;

posixipc_state *posixipc_get_state(PyObject *mod);
posixipc_state *posixipc_state_from_type(PyTypeObject *tp);
posixipc_state *posixipc_state_from_obj(PyObject *obj);

int posixipc_err(posixipc_state *st, int rc);
int posixipc_raise_closed(posixipc_state *st);
int posixipc_raise_unbound(void);

int posixipc_blocking_wait(int (*fn)(void *, const posixipc_deadline *), void *arg, const posixipc_deadline *user,
                           int interruptible, clockid_t clk);

typedef struct
{
    int timeout_none;
    double timeout;
    int blocking;
    int interruptible;
} posixipc_acquire_opts;

int posixipc_parse_acquire(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, posixipc_acquire_opts *out);

int posixipc_shmobj_pin(PosixIPCSharedMemoryObject *r);
void posixipc_shmobj_unpin(PosixIPCSharedMemoryObject *r);
PyObject *posixipc_shmobj_from_core(posixipc_state *st, posixipc_shm *core, int created);
int posixipc_shmobj_close(PosixIPCSharedMemoryObject *r, int force);

PyObject *posixipc_mutex_new_unbound(posixipc_state *st, PyTypeObject *type, uint16_t kind, uint32_t init_flags,
                                     PyObject *on_owner_died, uint32_t slot);
int posixipc_mutex_bind(PosixIPCMutexObject *m, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                        uint32_t index, int do_init);
PyObject *posixipc_mutex_after_lock(PosixIPCMutexObject *m, int rc);
int posixipc_mutex_register(PyObject *mod, posixipc_state *st);
PyObject *posixipc_mutex_capsule_new(PosixIPCSharedMemoryObject *r, pthread_mutex_t *lock);

typedef struct
{
    PyObject_HEAD pthread_mutex_t **locks;
    uint32_t count;
    uint32_t first_slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    PyObject *region;
    PyObject *on_owner_died;
    _Atomic uint32_t flags;
} PosixIPCMutexArrayObject;

PyObject *posixipc_mutexarray_new_unbound(posixipc_state *st, uint16_t kind, uint32_t init_flags,
                                          PyObject *on_owner_died, uint32_t first_slot, uint32_t count);
int posixipc_mutexarray_bind(PosixIPCMutexArrayObject *a, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                             uint32_t first, uint32_t count);
int posixipc_array_register(PyObject *mod, posixipc_state *st);

typedef struct
{
    PyObject_HEAD pthread_rwlock_t *lock;
    PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    pthread_rwlock_t inline_storage;
} PosixIPCRWLockObject;

typedef struct
{
    PyObject_HEAD pthread_cond_t *cond;
    PyObject *region;
    PyObject *mutex;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    pthread_cond_t inline_storage;
} PosixIPCConditionObject;

typedef struct
{
    PyObject_HEAD sem_t *sem;
    PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    unsigned value;
    sem_t inline_storage;
} PosixIPCSemaphoreObject;

PyObject *posixipc_rwlock_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot);
int posixipc_rwlock_bind(PosixIPCRWLockObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                         uint32_t index);
PyObject *posixipc_cond_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot, PyObject *mutex);
int posixipc_cond_bind(PosixIPCConditionObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                       uint32_t index);
PyObject *posixipc_sem_new_unbound(posixipc_state *st, uint32_t init_flags, unsigned value, uint32_t slot);
int posixipc_sem_bind(PosixIPCSemaphoreObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                      uint32_t index);
int posixipc_sync_register(PyObject *mod, posixipc_state *st);

typedef struct
{
    PyObject_HEAD pthread_barrier_t *bar;
    PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    unsigned parties;
    pthread_barrier_t inline_storage;
} PosixIPCBarrierObject;

typedef struct
{
    PyObject_HEAD pthread_spinlock_t *lock;
    PyObject *region;
    uint32_t slot;
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
    _Atomic uint32_t flags;
    pthread_spinlock_t inline_storage;
} PosixIPCSpinLockObject;

typedef struct
{
    PyObject_HEAD sem_t *sem;
    char *name;
    _Atomic uint32_t flags;
} PosixIPCNamedSemObject;

PyObject *posixipc_barrier_new_unbound(posixipc_state *st, uint32_t init_flags, unsigned parties, uint32_t slot);
int posixipc_barrier_bind(PosixIPCBarrierObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                          uint32_t index);
PyObject *posixipc_spin_new_unbound(posixipc_state *st, uint32_t init_flags, uint32_t slot);
int posixipc_spin_bind(PosixIPCSpinLockObject *o, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                       uint32_t index);
int posixipc_m3_register(PyObject *mod, posixipc_state *st);

PyObject *posixipc_bytes_new_unbound(posixipc_state *st, uint32_t size, uint32_t slot);
int posixipc_bytes_bind(PosixIPCBytesObject *b, PosixIPCSharedMemoryObject *r, const posixipc_slot *slot,
                        uint32_t index);
PyObject *posixipc_bytes_view(PosixIPCSharedMemoryObject *r, uint32_t offset, uint32_t size);
int posixipc_bytes_register(PyObject *mod, posixipc_state *st);

typedef struct
{
    PyObject_HEAD pthread_mutex_t *put_lock;
    pthread_mutex_t *get_lock;
    pthread_cond_t *not_full;
    pthread_cond_t *not_empty;
    PyObject *region;
    uint32_t first_slot;
    uint32_t depth;
    uint32_t item_size;
    uint32_t cond_flags;
    posixipc_queue_view view;
    _Atomic uint32_t flags;
} PosixIPCQueueObject;

PyObject *posixipc_queue_new_unbound(posixipc_state *st, uint32_t depth, uint32_t item_size, uint32_t first_slot);
int posixipc_queue_init_on_shm(posixipc_shm *core, const posixipc_slot *bytes_slot, uint32_t depth, uint32_t item_size);
int posixipc_queue_bind(PosixIPCQueueObject *q, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                        uint32_t first);
int posixipc_queue_register(PyObject *mod, posixipc_state *st);

typedef struct
{
    PyObject_HEAD pthread_mutex_t *put_lock;
    pthread_mutex_t *get_lock;
    PyObject *region;
    uint32_t first_slot;
    uint32_t depth;
    uint32_t item_size;
    posixipc_futexq_view view;
    _Atomic uint32_t flags;
} PosixIPCFutexQueueObject;

PyObject *posixipc_futexq_new_unbound(posixipc_state *st, uint32_t depth, uint32_t item_size, uint32_t first_slot);
int posixipc_futexq_init_on_shm(posixipc_shm *core, const posixipc_slot *bytes_slot, uint32_t depth,
                                uint32_t item_size);
int posixipc_futexq_bind(PosixIPCFutexQueueObject *q, PosixIPCSharedMemoryObject *r, const posixipc_slot *slots,
                         uint32_t first);
int posixipc_linux_register(PyObject *mod, posixipc_state *st);

int posixipc_shm_register(PyObject *mod, posixipc_state *st);
int posixipc_layout_register(PyObject *mod, posixipc_state *st);

clockid_t posixipc_mutex_clock(void);

#endif
