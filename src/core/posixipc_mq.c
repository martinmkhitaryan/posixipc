#include "posixipc_mq.h"

#include "posixipc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

int posixipc_mq_validate_name(const char *name)
{
    int rc = posixipc_shm_validate_name(name);
    size_t len;

    if (rc != 0) {
        return rc;
    }
    len = strlen(name + 1);
    if (len > (size_t)NAME_MAX) {
        return ENAMETOOLONG;
    }
    return 0;
}

int posixipc_mq_create(const char *name, long maxmsg, long msgsize, mqd_t *out)
{
#if POSIXIPC_HAVE_MQ_OPEN
    struct mq_attr attr;
    mqd_t mq;
    int rc;

    if (out == NULL || maxmsg < 1 || msgsize < 1) {
        return EINVAL;
    }
    *out = (mqd_t)-1;
    rc = posixipc_mq_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = maxmsg;
    attr.mq_msgsize = msgsize;
    mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    if (mq == (mqd_t)-1) {
        return errno;
    }
    *out = mq;
    return 0;
#else
    (void)name;
    (void)maxmsg;
    (void)msgsize;
    (void)out;
    return ENOTSUP;
#endif
}

int posixipc_mq_open_or_create(const char *name, long maxmsg, long msgsize, mqd_t *out)
{
#if POSIXIPC_HAVE_MQ_OPEN
    struct mq_attr attr;
    mqd_t mq;
    int rc;

    if (out == NULL || maxmsg < 1 || msgsize < 1) {
        return EINVAL;
    }
    *out = (mqd_t)-1;
    rc = posixipc_mq_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = maxmsg;
    attr.mq_msgsize = msgsize;
    mq = mq_open(name, O_CREAT | O_RDWR, 0600, &attr);
    if (mq == (mqd_t)-1) {
        return errno;
    }
    *out = mq;
    return 0;
#else
    (void)name;
    (void)maxmsg;
    (void)msgsize;
    (void)out;
    return ENOTSUP;
#endif
}

int posixipc_mq_attach(const char *name, mqd_t *out)
{
#if POSIXIPC_HAVE_MQ_OPEN
    mqd_t mq;
    int rc;

    if (out == NULL) {
        return EINVAL;
    }
    *out = (mqd_t)-1;
    rc = posixipc_mq_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    mq = mq_open(name, O_RDWR);
    if (mq == (mqd_t)-1) {
        return errno;
    }
    *out = mq;
    return 0;
#else
    (void)name;
    (void)out;
    return ENOTSUP;
#endif
}

int posixipc_mq_close(mqd_t mq)
{
#if POSIXIPC_HAVE_MQ_OPEN
    if (mq == (mqd_t)-1) {
        return EINVAL;
    }
    if (mq_close(mq) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    return ENOTSUP;
#endif
}

int posixipc_mq_unlink(const char *name)
{
#if POSIXIPC_HAVE_MQ_OPEN
    int rc = posixipc_mq_validate_name(name);

    if (rc != 0) {
        return rc;
    }
    if (mq_unlink(name) != 0) {
        return errno;
    }
    return 0;
#else
    (void)name;
    return ENOTSUP;
#endif
}

int posixipc_mq_send(mqd_t mq, const void *buf, size_t len, unsigned prio)
{
#if POSIXIPC_HAVE_MQ_OPEN
    if (mq == (mqd_t)-1 || buf == NULL) {
        return EINVAL;
    }
    if (mq_send(mq, buf, len, prio) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    (void)buf;
    (void)len;
    (void)prio;
    return ENOTSUP;
#endif
}

int posixipc_mq_send_until(mqd_t mq, const void *buf, size_t len, unsigned prio, const posixipc_deadline *d)
{
#if POSIXIPC_HAVE_MQ_OPEN
    if (mq == (mqd_t)-1 || buf == NULL || d == NULL) {
        return EINVAL;
    }
    if (mq_timedsend(mq, buf, len, prio, &d->ts) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    (void)buf;
    (void)len;
    (void)prio;
    (void)d;
    return ENOTSUP;
#endif
}

int posixipc_mq_receive(mqd_t mq, void *buf, size_t len, unsigned *prio, ssize_t *got)
{
#if POSIXIPC_HAVE_MQ_OPEN
    ssize_t n;

    if (mq == (mqd_t)-1 || buf == NULL || got == NULL) {
        return EINVAL;
    }
    n = mq_receive(mq, buf, len, prio);
    if (n < 0) {
        return errno;
    }
    *got = n;
    return 0;
#else
    (void)mq;
    (void)buf;
    (void)len;
    (void)prio;
    (void)got;
    return ENOTSUP;
#endif
}

int posixipc_mq_receive_until(mqd_t mq, void *buf, size_t len, unsigned *prio, ssize_t *got, const posixipc_deadline *d)
{
#if POSIXIPC_HAVE_MQ_OPEN
    ssize_t n;

    if (mq == (mqd_t)-1 || buf == NULL || got == NULL || d == NULL) {
        return EINVAL;
    }
    n = mq_timedreceive(mq, buf, len, prio, &d->ts);
    if (n < 0) {
        return errno;
    }
    *got = n;
    return 0;
#else
    (void)mq;
    (void)buf;
    (void)len;
    (void)prio;
    (void)got;
    (void)d;
    return ENOTSUP;
#endif
}

int posixipc_mq_notify_signal(mqd_t mq, int signo)
{
#if POSIXIPC_HAVE_MQ_OPEN
    struct sigevent ev;

    if (mq == (mqd_t)-1 || signo < 1) {
        return EINVAL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = signo;
    if (mq_notify(mq, &ev) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    (void)signo;
    return ENOTSUP;
#endif
}

int posixipc_mq_notify_thread(mqd_t mq, void (*fn)(union sigval), void *arg)
{
#if POSIXIPC_HAVE_MQ_OPEN
    struct sigevent ev;

    if (mq == (mqd_t)-1 || fn == NULL) {
        return EINVAL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD;
    ev.sigev_notify_function = fn;
    ev.sigev_notify_attributes = NULL;
    ev.sigev_value.sival_ptr = arg;
    if (mq_notify(mq, &ev) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    (void)fn;
    (void)arg;
    return ENOTSUP;
#endif
}

int posixipc_mq_notify_cancel(mqd_t mq)
{
#if POSIXIPC_HAVE_MQ_OPEN
    if (mq == (mqd_t)-1) {
        return EINVAL;
    }
    if (mq_notify(mq, NULL) != 0) {
        return errno;
    }
    return 0;
#else
    (void)mq;
    return ENOTSUP;
#endif
}

int posixipc_mq_msgsize(mqd_t mq, long *out)
{
#if POSIXIPC_HAVE_MQ_OPEN
    struct mq_attr attr;

    if (mq == (mqd_t)-1 || out == NULL) {
        return EINVAL;
    }
    if (mq_getattr(mq, &attr) != 0) {
        return errno;
    }
    *out = attr.mq_msgsize;
    return 0;
#else
    (void)mq;
    (void)out;
    return ENOTSUP;
#endif
}
