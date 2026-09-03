#include "posixipc_spin.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define NITERS 20000

static int g_fails;
static pthread_spinlock_t g_lock;
static int g_counter;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static void *worker(void *arg)
{
    int i;
    int rc;

    (void)arg;
    for (i = 0; i < NITERS; i++) {
        do {
            rc = posixipc_spin_trylock(&g_lock);
        } while (rc == EBUSY);
        CHECK(rc == 0);
        g_counter++;
        CHECK(posixipc_spin_unlock(&g_lock) == 0);
    }
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    int i;
    posixipc_spin_config cfg = {0};

    g_counter = 0;
    CHECK(posixipc_spin_init(&g_lock, &cfg) == 0);
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    CHECK(g_counter == NTHREADS * NITERS);
    CHECK(posixipc_spin_destroy(&g_lock) == 0);
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
