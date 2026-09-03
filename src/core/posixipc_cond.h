#ifndef POSIXIPC_COND_H
#define POSIXIPC_COND_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "posixipc_time.h"

typedef struct
{
    uint32_t flags;
} posixipc_cond_config;

int posixipc_cond_init(pthread_cond_t *c, const posixipc_cond_config *cfg);
int posixipc_cond_signal(pthread_cond_t *c);
int posixipc_cond_broadcast(pthread_cond_t *c);
int posixipc_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int posixipc_cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, const posixipc_deadline *d);
int posixipc_cond_destroy(pthread_cond_t *c);
size_t posixipc_cond_storage_size(void);
clockid_t posixipc_cond_clock(uint32_t flags);

#endif
