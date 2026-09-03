#include "posixipc_eventfd.h"

#include "posixipc_config.h"

#include <errno.h>
#include <stdio.h>

#if POSIXIPC_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

static int g_fails;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

int main(void)
{
#if POSIXIPC_HAVE_EVENTFD
    int fd = -1;
    uint64_t v = 0;

    CHECK(posixipc_eventfd_create(0u, EFD_CLOEXEC, &fd) == 0);
    CHECK(fd >= 0);
    CHECK(posixipc_eventfd_write(fd, 3u) == 0);
    CHECK(posixipc_eventfd_read(fd, &v) == 0);
    CHECK(v == 3u);
    CHECK(posixipc_eventfd_close(fd) == 0);
#else
    int fd = -1;

    CHECK(posixipc_eventfd_create(0u, 0, &fd) == ENOSYS);
#endif
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
