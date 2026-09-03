#include "posixipc_queue.h"

#include "posixipc_config.h"

#include <errno.h>
#include <string.h>

static uint32_t align_up(uint32_t value, uint32_t align)
{
    uint32_t mask;

    if (align <= 1u) {
        return value;
    }
    mask = align - 1u;
    if ((align & mask) != 0u) {
        return 0;
    }
    if (value > UINT32_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static uint32_t ctrl_bytes(void)
{
    uint32_t n = align_up((uint32_t)sizeof(posixipc_queue_ctrl), (uint32_t)POSIXIPC_CACHELINE_BYTES);

    return n == 0 ? (uint32_t)POSIXIPC_CACHELINE_BYTES : n;
}

static uint32_t state_bytes(uint32_t depth)
{
    uint32_t raw;

    if (depth == 0 || depth > (UINT32_MAX / 4u)) {
        return 0;
    }
    raw = depth * 4u;
    return align_up(raw, (uint32_t)POSIXIPC_CACHELINE_BYTES);
}

uint32_t posixipc_queue_bytes_size(uint32_t depth, uint32_t item_size)
{
    uint32_t cb;
    uint32_t sb;
    uint32_t stride;
    uint32_t payload;
    uint32_t total;

    if (depth < 1u || item_size < 1u) {
        return 0;
    }
    cb = ctrl_bytes();
    sb = state_bytes(depth);
    stride = align_up(item_size, (uint32_t)POSIXIPC_CACHELINE_BYTES);
    if (cb == 0 || sb == 0 || stride == 0) {
        return 0;
    }
    if (depth > UINT32_MAX / stride) {
        return 0;
    }
    payload = depth * stride;
    if (cb > UINT32_MAX - sb) {
        return 0;
    }
    total = cb + sb;
    if (total > UINT32_MAX - payload) {
        return 0;
    }
    return total + payload;
}

int posixipc_queue_map(void *bytes, uint32_t nbytes, uint32_t depth, uint32_t item_size, posixipc_queue_view *out)
{
    uint32_t need;
    uint32_t cb;
    uint32_t sb;
    uint32_t stride;
    char *base;

    if (bytes == NULL || out == NULL) {
        return EINVAL;
    }
    need = posixipc_queue_bytes_size(depth, item_size);
    if (need == 0 || nbytes < need) {
        return EINVAL;
    }
    cb = ctrl_bytes();
    sb = state_bytes(depth);
    stride = align_up(item_size, (uint32_t)POSIXIPC_CACHELINE_BYTES);
    base = (char *)bytes;
    out->ctrl = (posixipc_queue_ctrl *)base;
    out->state = (uint32_t *)(base + cb);
    out->payload = base + cb + sb;
    out->depth = depth;
    out->item_size = item_size;
    out->stride = stride;
    return 0;
}

int posixipc_queue_init_ctrl(posixipc_queue_view *v)
{
    uint32_t i;

    if (v == NULL || v->ctrl == NULL || v->state == NULL) {
        return EINVAL;
    }
    memset(v->ctrl, 0, sizeof(*v->ctrl));
    v->ctrl->depth = v->depth;
    v->ctrl->item_size = v->item_size;
    for (i = 0; i < v->depth; i++) {
        v->state[i] = POSIXIPC_QUEUE_EMPTY;
    }
    return 0;
}

int posixipc_queue_recover(posixipc_queue_view *v)
{
    uint32_t i;
    uint32_t ready;

    if (v == NULL || v->ctrl == NULL || v->state == NULL) {
        return EINVAL;
    }
    ready = 0;
    for (i = 0; i < v->depth; i++) {
        uint32_t st = v->state[i];

        if (st == POSIXIPC_QUEUE_RESERVED_PUT) {
            v->state[i] = POSIXIPC_QUEUE_EMPTY;
        } else if (st == POSIXIPC_QUEUE_RESERVED_GET) {
            v->state[i] = POSIXIPC_QUEUE_READY;
            ready++;
        } else if (st == POSIXIPC_QUEUE_READY) {
            ready++;
        } else if (st != POSIXIPC_QUEUE_EMPTY) {
            v->state[i] = POSIXIPC_QUEUE_EMPTY;
        }
    }
    v->ctrl->count = ready;
    if (ready == 0) {
        v->ctrl->head = 0;
        v->ctrl->tail = 0;
        return 0;
    }
    for (i = 0; i < v->depth; i++) {
        if (v->state[i] == POSIXIPC_QUEUE_READY) {
            v->ctrl->head = i;
            break;
        }
    }
    v->ctrl->tail = (v->ctrl->head + ready) % v->depth;
    return 0;
}

int posixipc_queue_put(posixipc_queue_view *v, const void *data)
{
    uint32_t idx;

    if (v == NULL || v->ctrl == NULL || data == NULL) {
        return EINVAL;
    }
    if (v->ctrl->count >= v->depth) {
        return EAGAIN;
    }
    idx = v->ctrl->tail;
    if (idx >= v->depth) {
        return EINVAL;
    }
    v->state[idx] = POSIXIPC_QUEUE_RESERVED_PUT;
    memcpy(v->payload + (size_t)idx * v->stride, data, v->item_size);
    v->state[idx] = POSIXIPC_QUEUE_READY;
    v->ctrl->tail = (idx + 1u) % v->depth;
    v->ctrl->count += 1u;
    return 0;
}

int posixipc_queue_get(posixipc_queue_view *v, void *data)
{
    uint32_t idx;

    if (v == NULL || v->ctrl == NULL || data == NULL) {
        return EINVAL;
    }
    if (v->ctrl->count == 0u) {
        return EAGAIN;
    }
    idx = v->ctrl->head;
    if (idx >= v->depth || v->state[idx] != POSIXIPC_QUEUE_READY) {
        return EINVAL;
    }
    v->state[idx] = POSIXIPC_QUEUE_RESERVED_GET;
    memcpy(data, v->payload + (size_t)idx * v->stride, v->item_size);
    v->state[idx] = POSIXIPC_QUEUE_EMPTY;
    v->ctrl->head = (idx + 1u) % v->depth;
    v->ctrl->count -= 1u;
    return 0;
}

int posixipc_queue_qsize(const posixipc_queue_view *v, uint32_t *out)
{
    if (v == NULL || v->ctrl == NULL || out == NULL) {
        return EINVAL;
    }
    *out = v->ctrl->count;
    return 0;
}
