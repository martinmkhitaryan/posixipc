#ifndef POSIXIPC_EVENTFD_H
#define POSIXIPC_EVENTFD_H

#include <stdint.h>

int posixipc_eventfd_create(unsigned initval, int flags, int *fd);
int posixipc_eventfd_write(int fd, uint64_t value);
int posixipc_eventfd_read(int fd, uint64_t *value);
int posixipc_eventfd_close(int fd);

#endif
