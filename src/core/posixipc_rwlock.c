#include "posixipc_rwlock.h"

#include "posixipc_clocksym.h"
#include "posixipc_layout.h"

#include <errno.h>

int posixipc_rwlock_init(pthread_rwlock_t *rw, const posixipc_rwlock_config *cfg)
{
    pthread_rwlockattr_t attr;
    int rc;
    uint32_t flags = cfg != NULL ? cfg->flags : 0;

    if (rw == NULL) {
        return EINVAL;
    }
    rc = pthread_rwlockattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    if (flags & POSIXIPC_FLAG_PROCESS_SHARED) {
        rc = pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            pthread_rwlockattr_destroy(&attr);
            return rc;
        }
    }
    rc = pthread_rwlock_init(rw, &attr);
    pthread_rwlockattr_destroy(&attr);
    return rc;
}

int posixipc_rwlock_rdlock(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_rdlock(rw);
}

int posixipc_rwlock_wrlock(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_wrlock(rw);
}

int posixipc_rwlock_tryrdlock(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_tryrdlock(rw);
}

int posixipc_rwlock_trywrlock(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_trywrlock(rw);
}

int posixipc_rwlock_unlock(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_unlock(rw);
}

int posixipc_rwlock_destroy(pthread_rwlock_t *rw)
{
    if (rw == NULL) {
        return EINVAL;
    }
    return pthread_rwlock_destroy(rw);
}

clockid_t posixipc_rwlock_clock(void)
{
    return posixipc_have_rwlock_clocklock() ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

int posixipc_rwlock_rdlock_until(pthread_rwlock_t *rw, const posixipc_deadline *d)
{
    if (rw == NULL || d == NULL) {
        return EINVAL;
    }
    if (posixipc_have_rwlock_clocklock()) {
        return posixipc_rwlock_clockrdlock(rw, d->clk, &d->ts);
    }
    if (d->clk != CLOCK_REALTIME) {
        return EINVAL;
    }
    return pthread_rwlock_timedrdlock(rw, &d->ts);
}

int posixipc_rwlock_wrlock_until(pthread_rwlock_t *rw, const posixipc_deadline *d)
{
    if (rw == NULL || d == NULL) {
        return EINVAL;
    }
    if (posixipc_have_rwlock_clocklock()) {
        return posixipc_rwlock_clockwrlock(rw, d->clk, &d->ts);
    }
    if (d->clk != CLOCK_REALTIME) {
        return EINVAL;
    }
    return pthread_rwlock_timedwrlock(rw, &d->ts);
}

size_t posixipc_rwlock_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_RWLOCK, 0);
}
