#include "posixipc_clocksym.h"
#include "posixipc_sem.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define NITERS 10000

static int g_fails;
static sem_t g_sem;
static int g_counter;
static pthread_mutex_t g_tally;

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
        CHECK(posixipc_sem_wait(&g_sem) == 0);
        CHECK(pthread_mutex_lock(&g_tally) == 0);
        g_counter++;
        CHECK(pthread_mutex_unlock(&g_tally) == 0);
        CHECK(posixipc_sem_post(&g_sem) == 0);
    }
    return NULL;
}

static void test_mutex_like(void)
{
    pthread_t th[NTHREADS];
    int i;
    posixipc_sem_config cfg = {.flags = 0, .value = 1};

    g_counter = 0;
    CHECK(pthread_mutex_init(&g_tally, NULL) == 0);
    CHECK(posixipc_sem_init(&g_sem, &cfg) == 0);
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    CHECK(g_counter == NTHREADS * NITERS);
    CHECK(posixipc_sem_destroy(&g_sem) == 0);
    CHECK(pthread_mutex_destroy(&g_tally) == 0);
}

static void *blocker(void *arg)
{
    posixipc_deadline *dl = arg;
    int rc = posixipc_sem_wait_until(&g_sem, dl);

    CHECK(rc == ETIMEDOUT || rc == EINVAL);
    return NULL;
}

static void test_timeout(void)
{
    pthread_t th;
    posixipc_deadline dl;
    posixipc_sem_config cfg = {.flags = 0, .value = 0};
    clockid_t clk = posixipc_sem_clock();

    CHECK(posixipc_sem_init(&g_sem, &cfg) == 0);
    CHECK(posixipc_deadline_from_seconds(clk, 0.15, &dl) == 0);
    CHECK(pthread_create(&th, NULL, blocker, &dl) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(posixipc_sem_destroy(&g_sem) == 0);
}

int main(void)
{
    posixipc_clocksym_init();
    test_mutex_like();
    test_timeout();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
