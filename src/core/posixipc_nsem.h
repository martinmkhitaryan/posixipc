#ifndef POSIXIPC_NSEM_H
#define POSIXIPC_NSEM_H

#include <semaphore.h>
#include <stddef.h>

int posixipc_nsem_validate_name(const char *name);
int posixipc_nsem_create(const char *name, unsigned value, sem_t **out);
int posixipc_nsem_attach(const char *name, sem_t **out);
int posixipc_nsem_close(sem_t *s);
int posixipc_nsem_unlink(const char *name);

#endif
