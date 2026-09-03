#include "posixipc_futex.h"

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
    uint32_t word = 0;
    posixipc_deadline dl;
    int rc;

    posixipc_futex_store(&word, 7u);
    CHECK(posixipc_futex_load(&word) == 7u);
    CHECK(posixipc_futex_add(&word, 1u) == 7u);
    CHECK(posixipc_futex_load(&word) == 8u);
    CHECK(posixipc_futex_wait(&word, 0u, NULL, 0) == 0);
    CHECK(posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 0.05, &dl) == 0);
    posixipc_futex_store(&word, 1u);
    rc = posixipc_futex_wait(&word, 1u, &dl, 0);
    CHECK(rc == ETIMEDOUT);
    CHECK(posixipc_futex_wake(&word, 1, 0) == 0);
#if !POSIXIPC_HAVE_FUTEX
    (void)rc;
#endif
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
