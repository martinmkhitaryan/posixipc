#include "posixipc_clocksym.h"
#include "posixipc_rwlock.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define NITERS 20000

static int g_fails;
static pthread_rwlock_t g_lock;
static int g_counter;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static void *reader(void *arg)
{
    int i;
    int snap;

    (void)arg;
    for (i = 0; i < NITERS; i++) {
        CHECK(posixipc_rwlock_rdlock(&g_lock) == 0);
        snap = g_counter;
        CHECK(snap >= 0);
        CHECK(posixipc_rwlock_unlock(&g_lock) == 0);
    }
    return NULL;
}

static void *writer(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < NITERS; i++) {
        CHECK(posixipc_rwlock_wrlock(&g_lock) == 0);
        g_counter++;
        CHECK(posixipc_rwlock_unlock(&g_lock) == 0);
    }
    return NULL;
}

static void test_read_write(void)
{
    pthread_t th[NTHREADS];
    int i;
    posixipc_rwlock_config cfg = {0};

    g_counter = 0;
    CHECK(posixipc_rwlock_init(&g_lock, &cfg) == 0);
    CHECK(pthread_create(&th[0], NULL, writer, NULL) == 0);
    CHECK(pthread_create(&th[1], NULL, writer, NULL) == 0);
    for (i = 2; i < NTHREADS; i++) {
        CHECK(pthread_create(&th[i], NULL, reader, NULL) == 0);
    }
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
    }
    CHECK(g_counter == 2 * NITERS);
    CHECK(posixipc_rwlock_destroy(&g_lock) == 0);
}

static void *wr_blocker(void *arg)
{
    posixipc_deadline *dl = arg;
    int rc = posixipc_rwlock_wrlock_until(&g_lock, dl);

    CHECK(rc == ETIMEDOUT || rc == EINVAL);
    return NULL;
}

static void test_timed_write(void)
{
    pthread_t th;
    posixipc_deadline dl;
    posixipc_rwlock_config cfg = {0};
    clockid_t clk = posixipc_rwlock_clock();

    CHECK(posixipc_rwlock_init(&g_lock, &cfg) == 0);
    CHECK(posixipc_rwlock_wrlock(&g_lock) == 0);
    CHECK(posixipc_deadline_from_seconds(clk, 0.15, &dl) == 0);
    CHECK(pthread_create(&th, NULL, wr_blocker, &dl) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(posixipc_rwlock_unlock(&g_lock) == 0);
    CHECK(posixipc_rwlock_destroy(&g_lock) == 0);
}

int main(void)
{
    posixipc_clocksym_init();
    test_read_write();
    test_timed_write();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
