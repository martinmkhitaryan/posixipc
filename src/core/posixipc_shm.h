#ifndef POSIXIPC_SHM_H
#define POSIXIPC_SHM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "posixipc_time.h"

#define POSIXIPC_SHM_MAGIC 0x50495043u
#define POSIXIPC_HEADER_BYTES 64u

#define POSIXIPC_STATE_UNINIT 0u
#define POSIXIPC_STATE_INITIALIZING 1u
#define POSIXIPC_STATE_READY 2u
#define POSIXIPC_STATE_BROKEN 3u

typedef struct
{
    uint32_t magic;
    uint16_t layout_version;
    uint16_t slot_count;
    uint32_t abi_tag;
    uint32_t flags;
    uint32_t total_size;
    uint32_t directory_bytes;
    _Atomic uint32_t state;
    uint32_t layout_digest;
    uint32_t reserved[8];
} posixipc_shm_header;

_Static_assert(sizeof(posixipc_shm_header) == 64, "header must be one cache line");

typedef struct
{
    uint16_t kind;
    uint16_t align;
    uint32_t offset;
    uint32_t size;
    uint32_t init_flags;
} posixipc_slot;

_Static_assert(sizeof(posixipc_slot) == 16, "slot directory record");

typedef struct
{
    uint16_t layout_version;
    uint16_t slot_count;
    uint32_t abi_tag;
    uint32_t flags;
    uint32_t total_size;
    uint32_t directory_bytes;
    uint32_t layout_digest;
    const posixipc_slot *slots;
} posixipc_shm_expect;

typedef struct
{
    void *map;
    size_t map_len;
    posixipc_shm_header *hdr;
    char *name;
} posixipc_shm;

int posixipc_shm_validate_name(const char *name);
bool posixipc_shm_stat_permitted(const struct stat *st);
int posixipc_shm_create(const char *name, const posixipc_shm_expect *expect, posixipc_shm *out);
int posixipc_shm_publish(posixipc_shm *h);
int posixipc_shm_mark_broken(posixipc_shm *h);
int posixipc_shm_attach(const char *name, const posixipc_shm_expect *expect, const posixipc_deadline *deadline,
                        posixipc_shm *out);
int posixipc_shm_open_or_create(const char *name, const posixipc_shm_expect *expect, const posixipc_deadline *deadline,
                                posixipc_shm *out);
int posixipc_shm_close(posixipc_shm *h);
int posixipc_shm_unlink(const char *name);
int posixipc_shm_offset_ptr(const posixipc_shm *h, uint32_t offset, uint32_t size, uint32_t align, void **out);
posixipc_shm_header *posixipc_shm_header_ptr(const posixipc_shm *h);
posixipc_slot *posixipc_shm_directory(const posixipc_shm *h);

#endif
