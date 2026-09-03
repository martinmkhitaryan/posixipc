#ifndef POSIXIPC_ABI_H
#define POSIXIPC_ABI_H

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint32_t> posixipc_atomic_u32;
extern "C" {
#else
#include <stdatomic.h>
typedef _Atomic uint32_t posixipc_atomic_u32;
#endif

#define POSIXIPC_SHM_MAGIC 0x50495043u
#define POSIXIPC_HEADER_BYTES 64u

#define POSIXIPC_STATE_UNINIT 0u
#define POSIXIPC_STATE_INITIALIZING 1u
#define POSIXIPC_STATE_READY 2u
#define POSIXIPC_STATE_BROKEN 3u

#define POSIXIPC_MUTEX_CAPSULE_NAME "posixipc.mutex.v1"
#define POSIXIPC_MUTEX_CAPSULE_VERSION 1u

typedef struct
{
    uint32_t magic;
    uint16_t layout_version;
    uint16_t slot_count;
    uint32_t abi_tag;
    uint32_t flags;
    uint32_t total_size;
    uint32_t directory_bytes;
    posixipc_atomic_u32 state;
    uint32_t layout_digest;
    uint32_t reserved[8];
} posixipc_shm_header;

typedef struct
{
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
} posixipc_slot;

#ifndef POSIXIPC_NO_PYTHON

#include <Python.h>
#include <pthread.h>

typedef struct posixipc_mutex_capsule
{
    uint32_t version;
    pthread_mutex_t *mutex;
    int (*retain)(struct posixipc_mutex_capsule *);
    int (*release)(struct posixipc_mutex_capsule *);
} posixipc_mutex_capsule;

static inline posixipc_mutex_capsule *posixipc_mutex_capsule_get(PyObject *capsule)
{
    posixipc_mutex_capsule *ctx;

    ctx = (posixipc_mutex_capsule *)PyCapsule_GetPointer(capsule, POSIXIPC_MUTEX_CAPSULE_NAME);
    if (ctx == NULL) {
        return NULL;
    }
    if (ctx->version != POSIXIPC_MUTEX_CAPSULE_VERSION || ctx->mutex == NULL || ctx->retain == NULL ||
        ctx->release == NULL) {
        PyErr_SetString(PyExc_ValueError, "invalid posixipc mutex capsule");
        return NULL;
    }
    return ctx;
}

static inline pthread_mutex_t *posixipc_mutex_from_capsule(PyObject *capsule)
{
    posixipc_mutex_capsule *ctx = posixipc_mutex_capsule_get(capsule);

    if (ctx == NULL) {
        return NULL;
    }
    return ctx->mutex;
}

static inline int posixipc_mutex_capsule_retain(PyObject *capsule)
{
    posixipc_mutex_capsule *ctx = posixipc_mutex_capsule_get(capsule);

    if (ctx == NULL) {
        return -1;
    }
    return ctx->retain(ctx);
}

static inline int posixipc_mutex_capsule_release(PyObject *capsule)
{
    posixipc_mutex_capsule *ctx = posixipc_mutex_capsule_get(capsule);

    if (ctx == NULL) {
        return -1;
    }
    return ctx->release(ctx);
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* POSIXIPC_ABI_H / capsule */
