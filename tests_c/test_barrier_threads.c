#include "posixipc_barrier.h"
#include "posixipc_result.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4

static int g_fails;
static pthread_barrier_t g_bar;
static _Atomic int g_serial;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static void *worker(void *arg)
{
    int rc;

    (void)arg;
    rc = posixipc_barrier_wait(&g_bar);
    CHECK(rc == 0 || rc == POSIXIPC_BARRIER_SERIAL);
    if (rc == POSIXIPC_BARRIER_SERIAL) {
        atomic_fetch_add_explicit(&g_serial, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    int i;
    posixipc_barrier_config cfg = {.flags = 0, .parties = NTHREADS};

    atomic_store_explicit(&g_serial, 0, memory_order_relaxed);
    CHECK(posixipc_barrier_init(&g_bar, &cfg) == 0);
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    CHECK(atomic_load_explicit(&g_serial, memory_order_relaxed) == 1);
    CHECK(posixipc_barrier_destroy(&g_bar) == 0);
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
