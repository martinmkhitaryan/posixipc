#ifndef POSIXIPC_MEMFD_H
#define POSIXIPC_MEMFD_H

#include <stddef.h>

typedef struct
{
    void *map;
    size_t size;
    int fd;
    char *name;
} posixipc_memfd;

int posixipc_memfd_create(const char *name, size_t size, posixipc_memfd *out);
int posixipc_memfd_from_fd(int fd, size_t size, const char *name, posixipc_memfd *out);
int posixipc_memfd_close(posixipc_memfd *m);

#endif
