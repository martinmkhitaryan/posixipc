#include "posixipc_eventfd.h"

#include "posixipc_config.h"
#include "posixipc_result.h"

#include <errno.h>
#include <stddef.h>
#include <unistd.h>

#if POSIXIPC_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

int posixipc_eventfd_create(unsigned initval, int flags, int *fd)
{
#if POSIXIPC_HAVE_EVENTFD
    int efd;

    if (fd == NULL) {
        return EINVAL;
    }
    efd = eventfd(initval, flags);
    if (efd < 0) {
        return errno;
    }
    *fd = efd;
    return 0;
#else
    (void)initval;
    (void)flags;
    (void)fd;
    return ENOSYS;
#endif
}

int posixipc_eventfd_write(int fd, uint64_t value)
{
    ssize_t n;

    if (fd < 0) {
        return EBADF;
    }
    n = write(fd, &value, sizeof(value));
    if (n < 0) {
        return errno;
    }
    if ((size_t)n != sizeof(value)) {
        return EIO;
    }
    return 0;
}

int posixipc_eventfd_read(int fd, uint64_t *value)
{
    ssize_t n;

    if (fd < 0 || value == NULL) {
        return EINVAL;
    }
    n = read(fd, value, sizeof(*value));
    if (n < 0) {
        return errno;
    }
    if ((size_t)n != sizeof(*value)) {
        return EIO;
    }
    return 0;
}

int posixipc_eventfd_close(int fd)
{
    if (fd < 0) {
        return EBADF;
    }
    return POSIXIPC_ERRNO_OR(close(fd), -1);
}
