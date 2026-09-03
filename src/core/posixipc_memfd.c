#include "posixipc_memfd.h"

#include "posixipc_config.h"
#include "posixipc_result.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

#if POSIXIPC_HAVE_MEMFD_CREATE
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1u
#endif
#endif

static int memfd_map(int fd, size_t size, void **map)
{
    void *p;

    if (fd < 0 || size == 0 || map == NULL) {
        return EINVAL;
    }
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        return errno;
    }
    *map = p;
    return 0;
}

int posixipc_memfd_create(const char *name, size_t size, posixipc_memfd *out)
{
#if POSIXIPC_HAVE_MEMFD_CREATE
    int fd;
    int rc;
    const char *label = name != NULL && name[0] != '\0' ? name : "posixipc";

    if (out == NULL || size == 0) {
        return EINVAL;
    }
    memset(out, 0, sizeof(*out));
    fd = memfd_create(label, MFD_CLOEXEC);
    if (fd < 0) {
        return errno;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        rc = errno;
        (void)close(fd);
        return rc;
    }
    rc = posixipc_memfd_from_fd(fd, size, label, out);
    if (rc != 0) {
        (void)close(fd);
    }
    return rc;
#else
    (void)name;
    (void)size;
    (void)out;
    return ENOSYS;
#endif
}

int posixipc_memfd_from_fd(int fd, size_t size, const char *name, posixipc_memfd *out)
{
    void *map = NULL;
    char *copy = NULL;
    int rc;

    if (out == NULL || fd < 0 || size == 0) {
        return EINVAL;
    }
    rc = memfd_map(fd, size, &map);
    if (rc != 0) {
        return rc;
    }
    if (name != NULL) {
        copy = strdup(name);
        if (copy == NULL) {
            (void)munmap(map, size);
            return ENOMEM;
        }
    }
    out->map = map;
    out->size = size;
    out->fd = fd;
    out->name = copy;
    return 0;
}

int posixipc_memfd_close(posixipc_memfd *m)
{
    int rc = 0;
    int cr;

    if (m == NULL) {
        return EINVAL;
    }
    if (m->map != NULL && m->size > 0) {
        if (munmap(m->map, m->size) != 0 && rc == 0) {
            rc = errno;
        }
    }
    if (m->fd >= 0) {
        cr = POSIXIPC_ERRNO_OR(close(m->fd), -1);
        if (cr != 0 && rc == 0) {
            rc = cr;
        }
    }
    free(m->name);
    m->map = NULL;
    m->size = 0;
    m->fd = -1;
    m->name = NULL;
    return rc;
}
