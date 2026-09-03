#include "posixipc_cond.h"

#include "posixipc_config.h"
#include "posixipc_layout.h"

#include <errno.h>

int posixipc_cond_init(pthread_cond_t *c, const posixipc_cond_config *cfg)
{
    pthread_condattr_t attr;
    int rc;
    uint32_t flags = cfg != NULL ? cfg->flags : 0;

    if (c == NULL) {
        return EINVAL;
    }
    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    if (flags & POSIXIPC_FLAG_PROCESS_SHARED) {
        rc = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            pthread_condattr_destroy(&attr);
            return rc;
        }
    }
#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    if (flags & POSIXIPC_FLAG_MONOTONIC) {
        rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (rc != 0) {
            pthread_condattr_destroy(&attr);
            return rc;
        }
    }
#endif
    rc = pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

int posixipc_cond_signal(pthread_cond_t *c)
{
    if (c == NULL) {
        return EINVAL;
    }
    return pthread_cond_signal(c);
}

int posixipc_cond_broadcast(pthread_cond_t *c)
{
    if (c == NULL) {
        return EINVAL;
    }
    return pthread_cond_broadcast(c);
}

int posixipc_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (c == NULL || m == NULL) {
        return EINVAL;
    }
    return pthread_cond_wait(c, m);
}

int posixipc_cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, const posixipc_deadline *d)
{
    if (c == NULL || m == NULL || d == NULL) {
        return EINVAL;
    }
    return pthread_cond_timedwait(c, m, &d->ts);
}

int posixipc_cond_destroy(pthread_cond_t *c)
{
    if (c == NULL) {
        return EINVAL;
    }
    return pthread_cond_destroy(c);
}

clockid_t posixipc_cond_clock(uint32_t flags)
{
    return (flags & POSIXIPC_FLAG_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

size_t posixipc_cond_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_COND, 0);
}
