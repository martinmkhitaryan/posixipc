#ifndef POSIXIPC_QUEUE_H
#define POSIXIPC_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define POSIXIPC_QUEUE_EMPTY 0u
#define POSIXIPC_QUEUE_RESERVED_PUT 1u
#define POSIXIPC_QUEUE_RESERVED_GET 2u
#define POSIXIPC_QUEUE_READY 3u

#define POSIXIPC_QUEUE_SLOTS 5u

typedef struct
{
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t flags;
    uint32_t depth;
    uint32_t item_size;
} posixipc_queue_ctrl;

typedef struct
{
    posixipc_queue_ctrl *ctrl;
    uint32_t *state;
    char *payload;
    uint32_t depth;
    uint32_t item_size;
    uint32_t stride;
} posixipc_queue_view;

uint32_t posixipc_queue_bytes_size(uint32_t depth, uint32_t item_size);
int posixipc_queue_map(void *bytes, uint32_t nbytes, uint32_t depth, uint32_t item_size, posixipc_queue_view *out);
int posixipc_queue_init_ctrl(posixipc_queue_view *v);
int posixipc_queue_recover(posixipc_queue_view *v);
int posixipc_queue_put(posixipc_queue_view *v, const void *data);
int posixipc_queue_get(posixipc_queue_view *v, void *data);
int posixipc_queue_qsize(const posixipc_queue_view *v, uint32_t *out);

#endif
