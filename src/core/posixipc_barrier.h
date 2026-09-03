#ifndef POSIXIPC_BARRIER_H
#define POSIXIPC_BARRIER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t flags;
    unsigned parties;
} posixipc_barrier_config;

int posixipc_barrier_init(pthread_barrier_t *b, const posixipc_barrier_config *cfg);
int posixipc_barrier_wait(pthread_barrier_t *b);
int posixipc_barrier_destroy(pthread_barrier_t *b);
size_t posixipc_barrier_storage_size(void);

#endif
