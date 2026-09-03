#include "posixipc_nsem.h"

#include "posixipc_config.h"
#include "posixipc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

int posixipc_nsem_validate_name(const char *name)
{
    int rc = posixipc_shm_validate_name(name);
    size_t len;

    if (rc != 0) {
        return rc;
    }
    len = strlen(name + 1);
#if POSIXIPC_LIBC_FAMILY == 1
    if (len > (size_t)NAME_MAX - 4u) {
        return ENAMETOOLONG;
    }
#endif
    return 0;
}

int posixipc_nsem_create(const char *name, unsigned value, sem_t **out)
{
#if POSIXIPC_HAVE_SEM_OPEN
    sem_t *s;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    *out = NULL;
    rc = posixipc_nsem_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    s = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (s == SEM_FAILED) {
        return errno;
    }
    *out = s;
    return 0;
#else
    (void)name;
    (void)value;
    (void)out;
    return ENOTSUP;
#endif
}

int posixipc_nsem_attach(const char *name, sem_t **out)
{
#if POSIXIPC_HAVE_SEM_OPEN
    sem_t *s;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    *out = NULL;
    rc = posixipc_nsem_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    s = sem_open(name, 0);
    if (s == SEM_FAILED) {
        return errno;
    }
    *out = s;
    return 0;
#else
    (void)name;
    (void)out;
    return ENOTSUP;
#endif
}

int posixipc_nsem_close(sem_t *s)
{
#if POSIXIPC_HAVE_SEM_OPEN
    if (s == NULL) {
        return EINVAL;
    }
    if (sem_close(s) != 0) {
        return errno;
    }
    return 0;
#else
    (void)s;
    return ENOTSUP;
#endif
}

int posixipc_nsem_unlink(const char *name)
{
#if POSIXIPC_HAVE_SEM_OPEN
    int rc = posixipc_nsem_validate_name(name);

    if (rc != 0) {
        return rc;
    }
    if (sem_unlink(name) != 0) {
        return errno;
    }
    return 0;
#else
    (void)name;
    return ENOTSUP;
#endif
}
