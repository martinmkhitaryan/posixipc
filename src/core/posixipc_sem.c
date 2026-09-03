#include "posixipc_sem.h"

#include "posixipc_clocksym.h"
#include "posixipc_layout.h"

#include <errno.h>

int posixipc_sem_init(sem_t *s, const posixipc_sem_config *cfg)
{
    uint32_t flags = cfg != NULL ? cfg->flags : 0;
    unsigned value = cfg != NULL ? cfg->value : 0;
    int pshared = (flags & POSIXIPC_FLAG_PROCESS_SHARED) ? 1 : 0;

    if (s == NULL) {
        return EINVAL;
    }
    if (sem_init(s, pshared, value) != 0) {
        return errno;
    }
    return 0;
}

int posixipc_sem_wait(sem_t *s)
{
    if (s == NULL) {
        return EINVAL;
    }
    if (sem_wait(s) != 0) {
        return errno;
    }
    return 0;
}

int posixipc_sem_trywait(sem_t *s)
{
    if (s == NULL) {
        return EINVAL;
    }
    if (sem_trywait(s) != 0) {
        return errno;
    }
    return 0;
}

int posixipc_sem_post(sem_t *s)
{
    if (s == NULL) {
        return EINVAL;
    }
    if (sem_post(s) != 0) {
        return errno;
    }
    return 0;
}

int posixipc_sem_destroy(sem_t *s)
{
    if (s == NULL) {
        return EINVAL;
    }
    if (sem_destroy(s) != 0) {
        return errno;
    }
    return 0;
}

clockid_t posixipc_sem_clock(void)
{
    return posixipc_have_sem_clockwait() ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}

int posixipc_sem_wait_until(sem_t *s, const posixipc_deadline *d)
{
    if (s == NULL || d == NULL) {
        return EINVAL;
    }
    if (posixipc_have_sem_clockwait()) {
        return posixipc_sem_clockwait(s, d->clk, &d->ts) == 0 ? 0 : errno;
    }
    if (d->clk != CLOCK_REALTIME) {
        return EINVAL;
    }
    if (sem_timedwait(s, &d->ts) != 0) {
        return errno;
    }
    return 0;
}

size_t posixipc_sem_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_SEM, 0);
}
