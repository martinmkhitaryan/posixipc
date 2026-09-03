#ifndef POSIXIPC_MQ_H
#define POSIXIPC_MQ_H

#include "posixipc_config.h"
#include "posixipc_time.h"

#include <signal.h>
#include <stddef.h>

#if POSIXIPC_HAVE_MQ_OPEN
#include <mqueue.h>
#else
typedef int mqd_t;
#endif

int posixipc_mq_validate_name(const char *name);
int posixipc_mq_create(const char *name, long maxmsg, long msgsize, mqd_t *out);
int posixipc_mq_attach(const char *name, mqd_t *out);
int posixipc_mq_open_or_create(const char *name, long maxmsg, long msgsize, mqd_t *out);
int posixipc_mq_close(mqd_t mq);
int posixipc_mq_unlink(const char *name);
int posixipc_mq_notify_signal(mqd_t mq, int signo);
int posixipc_mq_notify_thread(mqd_t mq, void (*fn)(union sigval), void *arg);
int posixipc_mq_notify_cancel(mqd_t mq);
int posixipc_mq_send(mqd_t mq, const void *buf, size_t len, unsigned prio);
int posixipc_mq_send_until(mqd_t mq, const void *buf, size_t len, unsigned prio, const posixipc_deadline *d);
int posixipc_mq_receive(mqd_t mq, void *buf, size_t len, unsigned *prio, ssize_t *got);
int posixipc_mq_receive_until(mqd_t mq, void *buf, size_t len, unsigned *prio, ssize_t *got,
                              const posixipc_deadline *d);
int posixipc_mq_msgsize(mqd_t mq, long *out);

#endif
