#ifndef POSIXIPC_FUTEX_H
#define POSIXIPC_FUTEX_H

#include <stdint.h>

#include "posixipc_time.h"

int posixipc_futex_wait(uint32_t *word, uint32_t expected, const posixipc_deadline *deadline, int process_shared);
int posixipc_futex_wake(uint32_t *word, int nwaiters, int process_shared);
uint32_t posixipc_futex_add(uint32_t *word, uint32_t delta);
uint32_t posixipc_futex_load(const uint32_t *word);
void posixipc_futex_store(uint32_t *word, uint32_t value);

#endif
