#include "posixipc_clocksym.h"
#include "posixipc_cond.h"
#include "posixipc_config.h"
#include "posixipc_layout.h"
#include "posixipc_mutex.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fails;
static pthread_mutex_t g_mutex;
static pthread_cond_t g_cond;
static int g_ready;
static int g_value;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static void *waiter(void *arg)
{
    (void)arg;
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    g_ready = 1;
    CHECK(posixipc_cond_signal(&g_cond) == 0);
    while (g_value == 0) {
        CHECK(posixipc_cond_wait(&g_cond, &g_mutex) == 0);
    }
    CHECK(g_value == 7);
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    return NULL;
}

static void test_signal(void)
{
    pthread_t th;
    posixipc_mutex_config mcfg = {0};
    posixipc_cond_config ccfg = {0};

#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    ccfg.flags = POSIXIPC_FLAG_MONOTONIC;
#endif
    g_ready = 0;
    g_value = 0;
    CHECK(posixipc_mutex_init(&g_mutex, &mcfg) == 0);
    CHECK(posixipc_cond_init(&g_cond, &ccfg) == 0);
    CHECK(pthread_create(&th, NULL, waiter, NULL) == 0);
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    while (!g_ready) {
        CHECK(posixipc_cond_wait(&g_cond, &g_mutex) == 0);
    }
    g_value = 7;
    CHECK(posixipc_cond_signal(&g_cond) == 0);
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(posixipc_cond_destroy(&g_cond) == 0);
    CHECK(posixipc_mutex_destroy(&g_mutex) == 0);
}

static void *timed_waiter(void *arg)
{
    posixipc_deadline *dl = arg;
    int rc;

    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    rc = posixipc_cond_wait_until(&g_cond, &g_mutex, dl);
    CHECK(rc == ETIMEDOUT);
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    return NULL;
}

static void test_timeout(void)
{
    pthread_t th;
    posixipc_deadline dl;
    posixipc_mutex_config mcfg = {0};
    posixipc_cond_config ccfg = {0};
    clockid_t clk;

#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    ccfg.flags = POSIXIPC_FLAG_MONOTONIC;
#endif
    clk = posixipc_cond_clock(ccfg.flags);
    CHECK(posixipc_mutex_init(&g_mutex, &mcfg) == 0);
    CHECK(posixipc_cond_init(&g_cond, &ccfg) == 0);
    CHECK(posixipc_deadline_from_seconds(clk, 0.15, &dl) == 0);
    CHECK(pthread_create(&th, NULL, timed_waiter, &dl) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    CHECK(posixipc_cond_destroy(&g_cond) == 0);
    CHECK(posixipc_mutex_destroy(&g_mutex) == 0);
}

static volatile int g_waiting;

static void *robust_waiter(void *arg)
{
    int rc;

    (void)arg;
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    g_waiting = 1;
    CHECK(posixipc_cond_signal(&g_cond) == 0);
    rc = posixipc_cond_wait(&g_cond, &g_mutex);
    CHECK(rc == EOWNERDEAD);
    CHECK(posixipc_mutex_consistent(&g_mutex) == 0);
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    return NULL;
}

static void *robust_holder(void *arg)
{
    (void)arg;
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    pthread_exit(NULL);
    return NULL;
}

static void test_robust_reacquire(void)
{
    pthread_t waiter_th;
    pthread_t holder_th;
    posixipc_mutex_config mcfg = {.flags = POSIXIPC_FLAG_ROBUST};
    posixipc_cond_config ccfg = {0};

#if POSIXIPC_HAVE_PTHREAD_CONDATTR_SETCLOCK
    ccfg.flags = POSIXIPC_FLAG_MONOTONIC;
#endif
    g_waiting = 0;
    CHECK(posixipc_mutex_init(&g_mutex, &mcfg) == 0);
    CHECK(posixipc_cond_init(&g_cond, &ccfg) == 0);
    CHECK(posixipc_mutex_lock(&g_mutex) == 0);
    CHECK(pthread_create(&waiter_th, NULL, robust_waiter, NULL) == 0);
    while (!g_waiting) {
        CHECK(posixipc_cond_wait(&g_cond, &g_mutex) == 0);
    }
    CHECK(posixipc_mutex_unlock(&g_mutex) == 0);
    CHECK(pthread_create(&holder_th, NULL, robust_holder, NULL) == 0);
    CHECK(pthread_join(holder_th, NULL) == 0);
    CHECK(posixipc_cond_broadcast(&g_cond) == 0);
    CHECK(pthread_join(waiter_th, NULL) == 0);
    CHECK(posixipc_cond_destroy(&g_cond) == 0);
    CHECK(posixipc_mutex_destroy(&g_mutex) == 0);
}

int main(void)
{
    posixipc_clocksym_init();
    test_signal();
    test_timeout();
    test_robust_reacquire();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
