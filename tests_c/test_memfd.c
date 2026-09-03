#include "posixipc_memfd.h"

#include "posixipc_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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
    posixipc_memfd m;

    memset(&m, 0, sizeof(m));
#if POSIXIPC_HAVE_MEMFD_CREATE
    CHECK(posixipc_memfd_create("posixipc-test", 64, &m) == 0);
    CHECK(m.map != NULL);
    CHECK(m.size == 64);
    CHECK(m.fd >= 0);
    memcpy(m.map, "hello", 5);
    CHECK(memcmp(m.map, "hello", 5) == 0);
    CHECK(posixipc_memfd_close(&m) == 0);
    CHECK(m.map == NULL);
    CHECK(m.fd == -1);
#else
    CHECK(posixipc_memfd_create("posixipc-test", 64, &m) == ENOSYS);
#endif
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
