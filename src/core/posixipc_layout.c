#include "posixipc_layout.h"

#include "posixipc_config.h"

#include <errno.h>
#include <stdalign.h>
#include <string.h>

static void feed_u8(uint32_t *h, uint8_t b)
{
    *h ^= b;
    *h *= 0x01000193u;
}

static void feed_u16_le(uint32_t *h, uint16_t v)
{
    feed_u8(h, (uint8_t)(v & 0xffu));
    feed_u8(h, (uint8_t)((v >> 8) & 0xffu));
}

static void feed_u32_le(uint32_t *h, uint32_t v)
{
    feed_u8(h, (uint8_t)(v & 0xffu));
    feed_u8(h, (uint8_t)((v >> 8) & 0xffu));
    feed_u8(h, (uint8_t)((v >> 16) & 0xffu));
    feed_u8(h, (uint8_t)((v >> 24) & 0xffu));
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    uint32_t mask;

    if (align <= 1u) {
        return value;
    }
    mask = align - 1u;
    if ((align & mask) == 0u) {
        if (value > UINT32_MAX - mask) {
            return 0;
        }
        return (value + mask) & ~mask;
    }
    if (align != 0u && value % align != 0u) {
        uint32_t add = align - (value % align);
        if (value > UINT32_MAX - add) {
            return 0;
        }
        return value + add;
    }
    return value;
}

uint32_t posixipc_abi_tag(void)
{
    uint32_t tag = (uint32_t)POSIXIPC_ABI_TAG_SEED;

    tag ^= (uint32_t)alignof(pthread_mutex_t) * 0x9e3779b1u;
    tag ^= (uint32_t)alignof(pthread_rwlock_t) * 0x85ebca77u;
    tag ^= (uint32_t)alignof(pthread_cond_t) * 0xc2b2ae3du;
    tag ^= (uint32_t)alignof(sem_t) * 0x27d4eb2fu;
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    tag ^= (uint32_t)alignof(pthread_barrier_t) * 0x165667b1u;
#endif
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    tag ^= (uint32_t)alignof(pthread_spinlock_t) * 0x85ebca6bu;
#endif
    tag ^= (uint32_t)POSIXIPC_CACHELINE_BYTES;
    tag ^= (uint32_t)POSIXIPC_LIBC_FAMILY << 24;
    if (tag == 0u) {
        tag = 1u;
    }
    return tag;
}

uint32_t posixipc_kind_align(uint16_t kind)
{
    switch (kind) {
    case POSIXIPC_KIND_MUTEX:
    case POSIXIPC_KIND_ROBUST_MUTEX:
        return (uint32_t)alignof(pthread_mutex_t);
    case POSIXIPC_KIND_RWLOCK:
        return (uint32_t)alignof(pthread_rwlock_t);
    case POSIXIPC_KIND_COND:
        return (uint32_t)alignof(pthread_cond_t);
    case POSIXIPC_KIND_SEM:
        return (uint32_t)alignof(sem_t);
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    case POSIXIPC_KIND_BARRIER:
        return (uint32_t)alignof(pthread_barrier_t);
#endif
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    case POSIXIPC_KIND_SPIN:
        return (uint32_t)alignof(pthread_spinlock_t);
#endif
    case POSIXIPC_KIND_BYTES:
        return (uint32_t)POSIXIPC_CACHELINE_BYTES;
    default:
        return 1u;
    }
}

uint32_t posixipc_kind_size(uint16_t kind, uint32_t requested)
{
    uint32_t raw = 0;

    switch (kind) {
    case POSIXIPC_KIND_MUTEX:
    case POSIXIPC_KIND_ROBUST_MUTEX:
        raw = (uint32_t)POSIXIPC_SIZEOF_PTHREAD_MUTEX_T;
        break;
    case POSIXIPC_KIND_RWLOCK:
        raw = (uint32_t)POSIXIPC_SIZEOF_PTHREAD_RWLOCK_T;
        break;
    case POSIXIPC_KIND_COND:
        raw = (uint32_t)POSIXIPC_SIZEOF_PTHREAD_COND_T;
        break;
    case POSIXIPC_KIND_SEM:
        raw = (uint32_t)POSIXIPC_SIZEOF_SEM_T;
        break;
#if POSIXIPC_HAVE_PTHREAD_BARRIER_WAIT
    case POSIXIPC_KIND_BARRIER:
        raw = (uint32_t)POSIXIPC_SIZEOF_PTHREAD_BARRIER_T;
        break;
#endif
#if POSIXIPC_HAVE_PTHREAD_SPIN_INIT
    case POSIXIPC_KIND_SPIN:
        raw = (uint32_t)POSIXIPC_SIZEOF_PTHREAD_SPINLOCK_T;
        break;
#endif
    case POSIXIPC_KIND_BYTES:
        raw = requested;
        break;
    default:
        return 0;
    }
    return align_up(raw, (uint32_t)POSIXIPC_CACHELINE_BYTES);
}

uint32_t posixipc_layout_digest(uint16_t layout_version, uint32_t abi_tag, const posixipc_slot *slots, uint16_t nslots)
{
    uint32_t h = 0x811C9DC5u;
    uint16_t i;

    feed_u16_le(&h, layout_version);
    feed_u32_le(&h, abi_tag);
    feed_u32_le(&h, (uint32_t)POSIXIPC_CACHELINE_BYTES);
    for (i = 0; i < nslots; i++) {
        feed_u16_le(&h, slots[i].kind);
        feed_u16_le(&h, slots[i].align);
        feed_u32_le(&h, slots[i].size);
        feed_u32_le(&h, slots[i].init_flags);
    }
    if (h == 0u) {
        h = 1u;
    }
    return h;
}

int posixipc_layout_build(posixipc_slot *slots, uint16_t nslots, posixipc_shm_expect *out)
{
    uint32_t dir_bytes;
    uint32_t cursor;
    uint16_t i;

    if (out == NULL) {
        return EINVAL;
    }
    if (nslots > 0 && slots == NULL) {
        return EINVAL;
    }
    dir_bytes = (uint32_t)nslots * (uint32_t)sizeof(posixipc_slot);
    if (nslots != 0 && dir_bytes / (uint32_t)sizeof(posixipc_slot) != nslots) {
        return EOVERFLOW;
    }
    cursor = POSIXIPC_HEADER_BYTES + dir_bytes;
    for (i = 0; i < nslots; i++) {
        uint32_t al = slots[i].align ? slots[i].align : posixipc_kind_align(slots[i].kind);
        uint32_t sz = slots[i].size ? slots[i].size : posixipc_kind_size(slots[i].kind, 0);
        uint32_t off;

        if (al == 0 || sz == 0) {
            return EINVAL;
        }
        off = align_up(cursor, al);
        if (off == 0 && cursor != 0) {
            return EOVERFLOW;
        }
        slots[i].align = (uint16_t)al;
        slots[i].size = sz;
        slots[i].offset = off;
        if (sz > UINT32_MAX - off) {
            return EOVERFLOW;
        }
        cursor = off + sz;
    }
    if (cursor == 0) {
        return EOVERFLOW;
    }
    out->layout_version = POSIXIPC_LAYOUT_VERSION;
    out->slot_count = nslots;
    out->abi_tag = posixipc_abi_tag();
    out->flags = 0;
    out->total_size = cursor;
    out->directory_bytes = dir_bytes;
    out->layout_digest = posixipc_layout_digest(out->layout_version, out->abi_tag, slots, nslots);
    out->slots = slots;
    return 0;
}
