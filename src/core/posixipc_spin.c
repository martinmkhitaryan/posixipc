#include "posixipc_spin.h"

#include "posixipc_config.h"
#include "posixipc_layout.h"

#include <errno.h>

int posixipc_spin_init(pthread_spinlock_t *s, const posixipc_spin_config *cfg)
{
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    uint32_t flags = cfg != NULL ? cfg->flags : 0;
    int pshared = (flags & POSIXIPC_FLAG_PROCESS_SHARED) ? PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;

    if (s == NULL) {
        return EINVAL;
    }
    return pthread_spin_init(s, pshared);
#else
    (void)s;
    (void)cfg;
    return ENOTSUP;
#endif
}

int posixipc_spin_trylock(pthread_spinlock_t *s)
{
#if POSIXIPC_HAVE_PTHREAD_SPIN_TRYLOCK
    if (s == NULL) {
        return EINVAL;
    }
    return pthread_spin_trylock(s);
#else
    (void)s;
    return ENOTSUP;
#endif
}

int posixipc_spin_unlock(pthread_spinlock_t *s)
{
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    if (s == NULL) {
        return EINVAL;
    }
    return pthread_spin_unlock(s);
#else
    (void)s;
    return ENOTSUP;
#endif
}

int posixipc_spin_destroy(pthread_spinlock_t *s)
{
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    if (s == NULL) {
        return EINVAL;
    }
    return pthread_spin_destroy(s);
#else
    (void)s;
    return ENOTSUP;
#endif
}

size_t posixipc_spin_storage_size(void)
{
    return posixipc_kind_size(POSIXIPC_KIND_SPIN, 0);
}
