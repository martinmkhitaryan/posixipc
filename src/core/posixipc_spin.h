#ifndef POSIXIPC_SPIN_H
#define POSIXIPC_SPIN_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t flags;
} posixipc_spin_config;

int posixipc_spin_init(pthread_spinlock_t *s, const posixipc_spin_config *cfg);
int posixipc_spin_trylock(pthread_spinlock_t *s);
int posixipc_spin_unlock(pthread_spinlock_t *s);
int posixipc_spin_destroy(pthread_spinlock_t *s);
size_t posixipc_spin_storage_size(void);

#endif
