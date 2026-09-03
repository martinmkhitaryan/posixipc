#ifndef POSIXIPC_CLOCKSYM_H
#define POSIXIPC_CLOCKSYM_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

void posixipc_clocksym_init(void);
void posixipc_clocksym_set_mutex_clocklock(int (*fn)(pthread_mutex_t *, clockid_t, const struct timespec *));
bool posixipc_have_mutex_clocklock(void);
int posixipc_mutex_clocklock(pthread_mutex_t *m, clockid_t clk, const struct timespec *abs);

bool posixipc_have_rwlock_clocklock(void);
int posixipc_rwlock_clockrdlock(pthread_rwlock_t *rw, clockid_t clk, const struct timespec *abs);
int posixipc_rwlock_clockwrlock(pthread_rwlock_t *rw, clockid_t clk, const struct timespec *abs);

bool posixipc_have_sem_clockwait(void);
int posixipc_sem_clockwait(sem_t *sem, clockid_t clk, const struct timespec *abs);

#endif
