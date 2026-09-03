#ifndef POSIXIPC_MUTEX_H
#define POSIXIPC_MUTEX_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "posixipc_time.h"

typedef struct
{
    uint32_t flags;
} posixipc_mutex_config;

int posixipc_mutex_init(pthread_mutex_t *m, const posixipc_mutex_config *cfg);
int posixipc_mutex_lock(pthread_mutex_t *m);
int posixipc_mutex_trylock(pthread_mutex_t *m);
int posixipc_mutex_unlock(pthread_mutex_t *m);
int posixipc_mutex_consistent(pthread_mutex_t *m);
int posixipc_mutex_destroy(pthread_mutex_t *m);
int posixipc_mutex_lock_until(pthread_mutex_t *m, const posixipc_deadline *d);
size_t posixipc_mutex_storage_size(void);

#endif
