#ifndef POSIXIPC_LAYOUT_H
#define POSIXIPC_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "posixipc_shm.h"

#define POSIXIPC_LAYOUT_VERSION 1u

#define POSIXIPC_KIND_BYTES 1u
#define POSIXIPC_KIND_MUTEX 2u
#define POSIXIPC_KIND_ROBUST_MUTEX 3u
#define POSIXIPC_KIND_RWLOCK 4u
#define POSIXIPC_KIND_COND 5u
#define POSIXIPC_KIND_SEM 6u
#define POSIXIPC_KIND_BARRIER 7u
#define POSIXIPC_KIND_SPIN 8u

#define POSIXIPC_FLAG_ROBUST (1u << 0)
#define POSIXIPC_FLAG_PROCESS_SHARED (1u << 1)
#define POSIXIPC_FLAG_OWNS_STORAGE (1u << 2)
#define POSIXIPC_FLAG_CLOSED (1u << 3)
#define POSIXIPC_FLAG_BOUND (1u << 4)
#define POSIXIPC_FLAG_PRIORITY_INHERIT (1u << 5)
#define POSIXIPC_FLAG_MONOTONIC (1u << 6)
#define POSIXIPC_SLOT_AUX_SHIFT 16u

uint32_t posixipc_abi_tag(void);
uint32_t posixipc_kind_align(uint16_t kind);
uint32_t posixipc_kind_size(uint16_t kind, uint32_t requested);
uint32_t posixipc_layout_digest(uint16_t layout_version, uint32_t abi_tag, const posixipc_slot *slots, uint16_t nslots);
int posixipc_layout_build(posixipc_slot *slots, uint16_t nslots, posixipc_shm_expect *out);

#endif
