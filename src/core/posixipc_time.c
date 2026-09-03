#include "posixipc_time.h"

#include <errno.h>
#include <stddef.h>

int posixipc_now(clockid_t clk, struct timespec *out)
{
    if (out == NULL) {
        return EINVAL;
    }
    if (clock_gettime(clk, out) != 0) {
        return errno;
    }
    return 0;
}

static int add_seconds(struct timespec *ts, double seconds)
{
    time_t add;
    long nsec;
    double frac;

    add = (time_t)seconds;
    frac = seconds - (double)add;
    if (frac < 0.0) {
        frac = 0.0;
    }
    nsec = (long)(frac * 1000000000.0);
    if (add > (time_t)100000000) {
        add = (time_t)100000000;
    }
    ts->tv_sec += add;
    ts->tv_nsec += nsec;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
    return 0;
}

int posixipc_deadline_from_seconds(clockid_t clk, double seconds, posixipc_deadline *out)
{
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    if (seconds != seconds || seconds < 0.0) {
        return EINVAL;
    }
    rc = posixipc_now(clk, &out->ts);
    if (rc != 0) {
        return rc;
    }
    out->clk = clk;
    return add_seconds(&out->ts, seconds);
}

void posixipc_deadline_min(const posixipc_deadline *a, const posixipc_deadline *b, posixipc_deadline *out)
{
    const posixipc_deadline *pick = a;

    if (a->clk != b->clk) {
        *out = *a;
        return;
    }
    if (b->ts.tv_sec < a->ts.tv_sec || (b->ts.tv_sec == a->ts.tv_sec && b->ts.tv_nsec < a->ts.tv_nsec)) {
        pick = b;
    }
    *out = *pick;
}

bool posixipc_deadline_expired(const posixipc_deadline *d)
{
    struct timespec now;

    if (d == NULL) {
        return false;
    }
    if (posixipc_now(d->clk, &now) != 0) {
        return true;
    }
    if (now.tv_sec > d->ts.tv_sec) {
        return true;
    }
    if (now.tv_sec < d->ts.tv_sec) {
        return false;
    }
    return now.tv_nsec >= d->ts.tv_nsec;
}
