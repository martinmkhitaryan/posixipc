#ifndef POSIXIPC_FUTEXQ_H
#define POSIXIPC_FUTEXQ_H

#include "posixipc_queue.h"

#define POSIXIPC_FUTEX_QUEUE_SLOTS 3u

typedef struct
{
    uint32_t *not_full;
    uint32_t *not_empty;
    posixipc_queue_view view;
} posixipc_futexq_view;

uint32_t posixipc_futexq_prefix_size(void);
uint32_t posixipc_futexq_bytes_size(uint32_t depth, uint32_t item_size);
int posixipc_futexq_map(void *bytes, uint32_t nbytes, uint32_t depth, uint32_t item_size, posixipc_futexq_view *out);
int posixipc_futexq_init(posixipc_futexq_view *v);

#endif
