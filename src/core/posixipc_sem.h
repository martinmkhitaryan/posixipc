#ifndef POSIXIPC_SEM_H
#define POSIXIPC_SEM_H

#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>

#include "posixipc_time.h"

typedef struct
{
    uint32_t flags;
    unsigned value;
} posixipc_sem_config;

int posixipc_sem_init(sem_t *s, const posixipc_sem_config *cfg);
int posixipc_sem_wait(sem_t *s);
int posixipc_sem_trywait(sem_t *s);
int posixipc_sem_post(sem_t *s);
int posixipc_sem_wait_until(sem_t *s, const posixipc_deadline *d);
int posixipc_sem_destroy(sem_t *s);
size_t posixipc_sem_storage_size(void);
clockid_t posixipc_sem_clock(void);

#endif
