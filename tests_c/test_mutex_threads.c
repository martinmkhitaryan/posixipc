#include "posixipc_clocksym.h"
#include "posixipc_mutex.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define NITERS 25000

static int g_fails;
static pthread_mutex_t g_mutex;
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

    (void)arg;
    for (i = 0; i < NITERS; i++) {
        CHECK(posixipc_mutex_lock(&g_mutex) == 0);
        g_counter++;
        CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    }
    return NULL;
}

static void test_increments(void)
{
    pthread_t th[NTHREADS];
    int i;
    posixipc_mutex_config cfg = {0};

    g_counter = 0;
    CHECK(posixipc_mutex_init(&g_mutex, &cfg) == 0);
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    CHECK(g_counter == NTHREADS * NITERS);
    CHECK(posixipc_mutex_unlock(&g_mutex) == EPERM);
    CHECK(posixipc_mutex_destroy(&g_mutex) == 0);
}

static void *blocker(void *arg)
{
    posixipc_deadline *dl = arg;
    int rc = posixipc_mutex_lock_until(&g_mutex, dl);

    CHECK(rc == ETIMEDOUT || rc == EINVAL);
    return NULL;
}

static void test_timed_contention(void)
{
    pthread_t th;
    posixipc_deadline dl;
    posixipc_mutex_config cfg = {0};
    clockid_t clk = CLOCK_REALTIME;

    CHECK(posixipc_mutex_init(&g_mutex, &cfg) == 0);
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    if (posixipc_have_mutex_clocklock()) {
        clk = CLOCK_MONOTONIC;
    }
    CHECK(posixipc_deadline_from_seconds(clk, 0.15, &dl) == 0);
    CHECK(pthread_create(&th, NULL, blocker, &dl) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    CHECK(posixipc_mutex_destroy(&g_mutex) == 0);
}

int main(void)
{
    posixipc_clocksym_init();
    test_increments();
    test_timed_contention();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
