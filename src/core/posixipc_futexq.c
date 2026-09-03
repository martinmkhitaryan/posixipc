#include "posixipc_futexq.h"

#include "posixipc_config.h"
#include "posixipc_futex.h"

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

uint32_t posixipc_futexq_prefix_size(void)
{
    uint32_t n = align_up(8u, (uint32_t)POSIXIPC_CACHELINE_BYTES);

    return n == 0 ? (uint32_t)POSIXIPC_CACHELINE_BYTES : n;
}

uint32_t posixipc_futexq_bytes_size(uint32_t depth, uint32_t item_size)
{
    uint32_t prefix = posixipc_futexq_prefix_size();
    uint32_t ring = posixipc_queue_bytes_size(depth, item_size);

    if (prefix == 0 || ring == 0) {
        return 0;
    }
    if (prefix > UINT32_MAX - ring) {
        return 0;
    }
    return prefix + ring;
}

int posixipc_futexq_map(void *bytes, uint32_t nbytes, uint32_t depth, uint32_t item_size, posixipc_futexq_view *out)
{
    uint32_t prefix;
    uint32_t need;
    char *base;
    int rc;

    if (bytes == NULL || out == NULL) {
        return EINVAL;
    }
    prefix = posixipc_futexq_prefix_size();
    need = posixipc_futexq_bytes_size(depth, item_size);
    if (prefix == 0 || need == 0 || nbytes < need) {
        return EINVAL;
    }
    base = (char *)bytes;
    out->not_full = (uint32_t *)base;
    out->not_empty = (uint32_t *)(base + 4u);
    rc = posixipc_queue_map(base + prefix, nbytes - prefix, depth, item_size, &out->view);
    if (rc != 0) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

int posixipc_futexq_init(posixipc_futexq_view *v)
{
    if (v == NULL || v->not_full == NULL || v->not_empty == NULL) {
        return EINVAL;
    }
    posixipc_futex_store(v->not_full, 0u);
    posixipc_futex_store(v->not_empty, 0u);
    return posixipc_queue_init_ctrl(&v->view);
}
