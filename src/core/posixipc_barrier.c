#include "posixipc_barrier.h"

#include "posixipc_config.h"
#include "posixipc_layout.h"
#include "posixipc_result.h"

#include <errno.h>

int posixipc_barrier_init(pthread_barrier_t *b, const posixipc_barrier_config *cfg)
{
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    pthread_barrierattr_t attr;
    int rc;
    uint32_t flags = cfg != NULL ? cfg->flags : 0;
    unsigned parties = cfg != NULL ? cfg->parties : 0;

    if (b == NULL || parties == 0) {
        return EINVAL;
    }
    rc = pthread_barrierattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    if (flags & POSIXIPC_FLAG_PROCESS_SHARED) {
#if POSIXIPC_HAVE_PTHREAD_BARRIERATTR_SETPSHARED
        rc = pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            pthread_barrierattr_destroy(&attr);
            return rc;
        }
#else
        pthread_barrierattr_destroy(&attr);
        return ENOTSUP;
#endif
    }
    rc = pthread_barrier_init(b, &attr, parties);
    pthread_barrierattr_destroy(&attr);
    return rc;
#else
    (void)b;
    (void)cfg;
    return ENOTSUP;
#endif
}

int posixipc_barrier_wait(pthread_barrier_t *b)
{
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    int rc;

    if (b == NULL) {
        return EINVAL;
    }
    rc = pthread_barrier_wait(b);
    if (rc == 0) {
        return 0;
    }
    if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
        return POSIXIPC_BARRIER_SERIAL;
    }
    return rc;
#else
    (void)b;
    return ENOTSUP;
#endif
}

int posixipc_barrier_destroy(pthread_barrier_t *b)
{
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    if (b == NULL) {
        return EINVAL;
    }
    return pthread_barrier_destroy(b);
#else
    (void)b;
    return ENOTSUP;
#endif
}

size_t posixipc_barrier_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_BARRIER, 0);
}
