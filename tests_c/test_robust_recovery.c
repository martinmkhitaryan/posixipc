#include "posixipc_clocksym.h"
#include "posixipc_layout.h"
#include "posixipc_mutex.h"
#include "posixipc_result.h"
#include "posixipc_shm.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fails;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

typedef struct
{
    posixipc_shm shm;
    posixipc_slot slots[2];
    posixipc_shm_expect expect;
    pthread_mutex_t *mutex;
    atomic_uint *ready;
} fixture;

static int wait_exit(pid_t pid)
{
    int st = 0;

    if (waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    if (WIFSIGNALED(st)) {
        return 128 + WTERMSIG(st);
    }
    if (!WIFEXITED(st)) {
        return -1;
    }
    return WEXITSTATUS(st);
}

static int setup(fixture *f, const char *tag, int robust)
{
    char name[80];
    posixipc_mutex_config cfg;
    int rc;
    void *p;

    memset(f, 0, sizeof(*f));
    snprintf(name, sizeof(name), "/posixipc-mx-%s-%d", tag, (int)getpid());
    f->slots[0].kind = robust ? POSIXIPC_KIND_ROBUST_MUTEX : POSIXIPC_KIND_MUTEX;
    f->slots[0].init_flags = POSIXIPC_FLAG_PROCESS_SHARED | (robust ? POSIXIPC_FLAG_ROBUST : 0);
    f->slots[1].kind = POSIXIPC_KIND_BYTES;
    f->slots[1].size = 64;
    rc = posixipc_layout_build(f->slots, 2, &f->expect);
    if (rc != 0) {
        return rc;
    }
    rc = posixipc_shm_create(name, &f->expect, &f->shm);
    if (rc != 0) {
        return rc;
    }
    rc = posixipc_shm_offset_ptr(&f->shm, f->slots[0].offset, f->slots[0].size, f->slots[0].align, &p);
    if (rc != 0) {
        return rc;
    }
    f->mutex = p;
    rc = posixipc_shm_offset_ptr(&f->shm, f->slots[1].offset, f->slots[1].size, f->slots[1].align, &p);
    if (rc != 0) {
        return rc;
    }
    f->ready = p;
    atomic_store(f->ready, 0);
    cfg.flags = f->slots[0].init_flags;
    rc = posixipc_mutex_init(f->mutex, &cfg);
    if (rc != 0) {
        return rc;
    }
    return posixipc_shm_publish(&f->shm);
}

static void teardown(fixture *f)
{
    char *name = f->shm.name;
    char copy[80];

    if (name != NULL) {
        snprintf(copy, sizeof(copy), "%s", name);
    } else {
        copy[0] = '\0';
    }
    posixipc_shm_close(&f->shm);
    if (copy[0] != '\0') {
        posixipc_shm_unlink(copy);
    }
}

static void hold_until_killed(void)
{
    alarm(5);
    for (;;) {
        pause();
    }
}

static void wait_ready(atomic_uint *ready)
{
    int i;

    for (i = 0; i < 5000; i++) {
        if (atomic_load(ready) != 0) {
            return;
        }
        usleep(1000);
    }
}

static void test_recover(int use_exit)
{
    fixture f;
    pid_t pid;
    int rc;

    CHECK(setup(&f, use_exit ? "ex" : "kill", 1) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        CHECK(posixipc_mutex_lock(f.mutex) == 0);
        atomic_store(f.ready, 1);
        if (use_exit) {
            _exit(0);
        }
        for (;;) {
            hold_until_killed();
        }
    }
    wait_ready(f.ready);
    if (!use_exit) {
        CHECK(kill(pid, SIGKILL) == 0);
    }
    CHECK(wait_exit(pid) >= 0);
    rc = posixipc_mutex_lock(f.mutex);
    CHECK(rc == EOWNERDEAD);
    CHECK(posixipc_mutex_consistent(f.mutex) == 0);
    CHECK(posixipc_mutex_unlock(f.mutex) == 0);
    CHECK(posixipc_mutex_lock(f.mutex) == 0);
    CHECK(posixipc_mutex_unlock(f.mutex) == 0);
    teardown(&f);
}

static void test_poison(void)
{
    fixture f;
    pid_t pid;
    int i;
    int rc;

    CHECK(setup(&f, "poison", 1) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_mutex_lock(f.mutex);
        atomic_store(f.ready, 1);
        hold_until_killed();
    }
    wait_ready(f.ready);
    kill(pid, SIGKILL);
    wait_exit(pid);
    CHECK(posixipc_mutex_lock(f.mutex) == EOWNERDEAD);
    CHECK(posixipc_mutex_unlock(f.mutex) == 0);
    for (i = 0; i < 3; i++) {
        rc = posixipc_mutex_lock(f.mutex);
        CHECK(rc == ENOTRECOVERABLE);
    }
    teardown(&f);
}

static void test_trylock_ownerdead(void)
{
    fixture f;
    pid_t pid;

    CHECK(setup(&f, "try", 1) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_mutex_lock(f.mutex);
        atomic_store(f.ready, 1);
        hold_until_killed();
    }
    wait_ready(f.ready);
    kill(pid, SIGKILL);
    wait_exit(pid);
    CHECK(posixipc_mutex_trylock(f.mutex) == EOWNERDEAD);
    posixipc_mutex_consistent(f.mutex);
    posixipc_mutex_unlock(f.mutex);
    teardown(&f);
}

static void test_lock_until_ownerdead(void)
{
    fixture f;
    pid_t pid;
    posixipc_deadline dl;

    CHECK(setup(&f, "until", 1) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_mutex_lock(f.mutex);
        atomic_store(f.ready, 1);
        hold_until_killed();
    }
    wait_ready(f.ready);
    kill(pid, SIGKILL);
    wait_exit(pid);
    CHECK(posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 1.0, &dl) == 0);
    CHECK(posixipc_mutex_lock_until(f.mutex, &dl) == EOWNERDEAD);
    posixipc_mutex_consistent(f.mutex);
    posixipc_mutex_unlock(f.mutex);
    teardown(&f);
}

static void test_nonrobust_timeout(void)
{
    fixture f;
    pid_t pid;
    posixipc_deadline dl;

    CHECK(setup(&f, "plain", 0) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_mutex_lock(f.mutex);
        atomic_store(f.ready, 1);
        hold_until_killed();
    }
    wait_ready(f.ready);
    CHECK(posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 0.2, &dl) == 0);
    CHECK(posixipc_mutex_lock_until(f.mutex, &dl) == ETIMEDOUT || posixipc_mutex_lock_until(f.mutex, &dl) == EINVAL);
    if (posixipc_have_mutex_clocklock() == 0) {
        CHECK(posixipc_deadline_from_seconds(CLOCK_REALTIME, 0.2, &dl) == 0);
        CHECK(posixipc_mutex_lock_until(f.mutex, &dl) == ETIMEDOUT);
    }
    kill(pid, SIGKILL);
    wait_exit(pid);
    teardown(&f);
}

int main(void)
{
    test_recover(0);
    test_recover(1);
    test_poison();
    test_trylock_ownerdead();
    test_lock_until_ownerdead();
    test_nonrobust_timeout();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
