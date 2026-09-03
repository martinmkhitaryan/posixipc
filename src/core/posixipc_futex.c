#include "posixipc_futex.h"

#include "posixipc_config.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

uint32_t posixipc_futex_add(uint32_t *word, uint32_t delta)
{
    return atomic_fetch_add_explicit((_Atomic uint32_t *)word, delta, memory_order_acq_rel);
}

uint32_t posixipc_futex_load(const uint32_t *word)
{
    return atomic_load_explicit((const _Atomic uint32_t *)word, memory_order_acquire);
}

void posixipc_futex_store(uint32_t *word, uint32_t value)
{
    atomic_store_explicit((_Atomic uint32_t *)word, value, memory_order_release);
}

int posixipc_futex_wait(uint32_t *word, uint32_t expected, const posixipc_deadline *deadline, int process_shared)
{
#if defined(__linux__)
    int op = FUTEX_WAIT_BITSET;
    const struct timespec *tsp = NULL;
    long r;

    if (word == NULL) {
        return EINVAL;
    }
    if (!process_shared) {
        op |= FUTEX_PRIVATE_FLAG;
    }
    if (deadline != NULL) {
        if (deadline->clk != CLOCK_MONOTONIC) {
            return EINVAL;
        }
        tsp = &deadline->ts;
    }
    for (;;) {
        r = syscall(SYS_futex, word, op, expected, tsp, NULL, FUTEX_BITSET_MATCH_ANY);
        if (r == 0 || errno == EAGAIN) {
            return 0;
        }
        if (errno == ETIMEDOUT) {
            return ETIMEDOUT;
        }
        if (errno == EINTR) {
            if (deadline != NULL) {
                return EINTR;
            }
            continue;
        }
        return errno;
    }
#else
    (void)word;
    (void)expected;
    (void)deadline;
    (void)process_shared;
    return ENOSYS;
#endif
}

int posixipc_futex_wake(uint32_t *word, int nwaiters, int process_shared)
{
#if defined(__linux__)
    int op = FUTEX_WAKE;
    long r;

    if (word == NULL || nwaiters < 0) {
        return EINVAL;
    }
    if (nwaiters > INT_MAX) {
        nwaiters = INT_MAX;
    }
    if (!process_shared) {
        op |= FUTEX_PRIVATE_FLAG;
    }
    r = syscall(SYS_futex, word, op, nwaiters, NULL, NULL, 0);
    if (r < 0) {
        return errno;
    }
    return 0;
#else
    (void)word;
    (void)nwaiters;
    (void)process_shared;
    return ENOSYS;
#endif
}
