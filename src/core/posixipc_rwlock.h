#ifndef POSIXIPC_RWLOCK_H
#define POSIXIPC_RWLOCK_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "posixipc_time.h"

typedef struct
{
    uint32_t flags;
} posixipc_rwlock_config;

int posixipc_rwlock_init(pthread_rwlock_t *rw, const posixipc_rwlock_config *cfg);
int posixipc_rwlock_rdlock(pthread_rwlock_t *rw);
int posixipc_rwlock_wrlock(pthread_rwlock_t *rw);
int posixipc_rwlock_tryrdlock(pthread_rwlock_t *rw);
int posixipc_rwlock_trywrlock(pthread_rwlock_t *rw);
int posixipc_rwlock_unlock(pthread_rwlock_t *rw);
int posixipc_rwlock_destroy(pthread_rwlock_t *rw);
int posixipc_rwlock_rdlock_until(pthread_rwlock_t *rw, const posixipc_deadline *d);
int posixipc_rwlock_wrlock_until(pthread_rwlock_t *rw, const posixipc_deadline *d);
size_t posixipc_rwlock_storage_size(void);
clockid_t posixipc_rwlock_clock(void);

#endif
