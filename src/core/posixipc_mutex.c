#include "posixipc_mutex.h"

#include "posixipc_clocksym.h"
#include "posixipc_layout.h"

#include <errno.h>
#include <string.h>

int posixipc_mutex_init(pthread_mutex_t *m, const posixipc_mutex_config *cfg)
{
    pthread_mutexattr_t attr;
    int rc;
    uint32_t flags = cfg != NULL ? cfg->flags : 0;

    if (m == NULL) {
        return EINVAL;
    }
    rc = pthread_mutexattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (rc != 0) {
        goto cleanup;
    }
    if (flags & POSIXIPC_FLAG_PROCESS_SHARED) {
        rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            goto cleanup;
        }
    }
    if (flags & POSIXIPC_FLAG_ROBUST) {
        rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (rc != 0) {
            goto cleanup;
        }
    }
    if (flags & POSIXIPC_FLAG_PRIORITY_INHERIT) {
        rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (rc != 0) {
            goto cleanup;
        }
    }
    rc = pthread_mutex_init(m, &attr);
cleanup:
    pthread_mutexattr_destroy(&attr);
    return rc;
}

int posixipc_mutex_lock(pthread_mutex_t *m)
{
    if (m == NULL) {
        return EINVAL;
    }
    return pthread_mutex_lock(m);
}

int posixipc_mutex_trylock(pthread_mutex_t *m)
{
    if (m == NULL) {
        return EINVAL;
    }
    return pthread_mutex_trylock(m);
}

int posixipc_mutex_unlock(pthread_mutex_t *m)
{
    if (m == NULL) {
        return EINVAL;
    }
    return pthread_mutex_unlock(m);
}

int posixipc_mutex_consistent(pthread_mutex_t *m)
{
    if (m == NULL) {
        return EINVAL;
    }
    return pthread_mutex_consistent(m);
}

int posixipc_mutex_destroy(pthread_mutex_t *m)
{
    if (m == NULL) {
        return EINVAL;
    }
    return pthread_mutex_destroy(m);
}

int posixipc_mutex_lock_until(pthread_mutex_t *m, const posixipc_deadline *d)
{
    if (m == NULL || d == NULL) {
        return EINVAL;
    }
    if (posixipc_have_mutex_clocklock()) {
        return posixipc_mutex_clocklock(m, d->clk, &d->ts);
    }
    if (d->clk != CLOCK_REALTIME) {
        return EINVAL;
    }
    return pthread_mutex_timedlock(m, &d->ts);
}

size_t posixipc_mutex_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_MUTEX, 0);
}
