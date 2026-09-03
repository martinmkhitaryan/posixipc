#include "posixipc_clocksym.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

static int (*g_mutex_clocklock)(pthread_mutex_t *, clockid_t, const struct timespec *);
static int (*g_rwlock_clockrdlock)(pthread_rwlock_t *, clockid_t, const struct timespec *);
static int (*g_rwlock_clockwrlock)(pthread_rwlock_t *, clockid_t, const struct timespec *);
static int (*g_sem_clockwait)(sem_t *, clockid_t, const struct timespec *);
static int g_inited;

void posixipc_clocksym_init(void)
{
    void *sym;

    if (g_inited) {
        return;
    }
    sym = dlsym(RTLD_DEFAULT, "pthread_mutex_clocklock");
    memcpy(&g_mutex_clocklock, &sym, sizeof(g_mutex_clocklock));
    sym = dlsym(RTLD_DEFAULT, "pthread_rwlock_clockrdlock");
    memcpy(&g_rwlock_clockrdlock, &sym, sizeof(g_rwlock_clockrdlock));
    sym = dlsym(RTLD_DEFAULT, "pthread_rwlock_clockwrlock");
    memcpy(&g_rwlock_clockwrlock, &sym, sizeof(g_rwlock_clockwrlock));
    sym = dlsym(RTLD_DEFAULT, "sem_clockwait");
    memcpy(&g_sem_clockwait, &sym, sizeof(g_sem_clockwait));
    g_inited = 1;
}

void posixipc_clocksym_set_mutex_clocklock(int (*fn)(pthread_mutex_t *, clockid_t, const struct timespec *))
{
    g_mutex_clocklock = fn;
    g_inited = 1;
}

bool posixipc_have_mutex_clocklock(void)
{
    posixipc_clocksym_init();
    return g_mutex_clocklock != NULL;
}

int posixipc_mutex_clocklock(pthread_mutex_t *m, clockid_t clk, const struct timespec *abs)
{
    posixipc_clocksym_init();
    if (g_mutex_clocklock == NULL) {
        return ENOSYS;
    }
    return g_mutex_clocklock(m, clk, abs);
}

bool posixipc_have_rwlock_clocklock(void)
{
    posixipc_clocksym_init();
    return g_rwlock_clockrdlock != NULL && g_rwlock_clockwrlock != NULL;
}

int posixipc_rwlock_clockrdlock(pthread_rwlock_t *rw, clockid_t clk, const struct timespec *abs)
{
    posixipc_clocksym_init();
    if (g_rwlock_clockrdlock == NULL) {
        return ENOSYS;
    }
    return g_rwlock_clockrdlock(rw, clk, abs);
}

int posixipc_rwlock_clockwrlock(pthread_rwlock_t *rw, clockid_t clk, const struct timespec *abs)
{
    posixipc_clocksym_init();
    if (g_rwlock_clockwrlock == NULL) {
        return ENOSYS;
    }
    return g_rwlock_clockwrlock(rw, clk, abs);
}

bool posixipc_have_sem_clockwait(void)
{
    posixipc_clocksym_init();
    return g_sem_clockwait != NULL;
}

int posixipc_sem_clockwait(sem_t *sem, clockid_t clk, const struct timespec *abs)
{
    posixipc_clocksym_init();
    if (g_sem_clockwait == NULL) {
        return ENOSYS;
    }
    return g_sem_clockwait(sem, clk, abs);
}
