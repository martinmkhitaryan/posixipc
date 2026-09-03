#include "posixipc_shm.h"

#include "posixipc_config.h"
#include "posixipc_layout.h"
#include "posixipc_result.h"

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

static uint32_t magic_wire(void)
{
    return htobe32(POSIXIPC_SHM_MAGIC);
}

int posixipc_shm_validate_name(const char *name)
{
    const char *rest;
    size_t len;

    if (name == NULL || name[0] != '/') {
        return EINVAL;
    }
    rest = name + 1;
    if (rest[0] == '\0') {
        return EINVAL;
    }
    if (strchr(rest, '/') != NULL) {
        return EINVAL;
    }
    if (strcmp(rest, ".") == 0 || strcmp(rest, "..") == 0) {
        return EINVAL;
    }
    len = strlen(rest);
    if (len > (size_t)NAME_MAX) {
        return ENAMETOOLONG;
    }
    return 0;
}

bool posixipc_shm_stat_permitted(const struct stat *st)
{
    if (st == NULL) {
        return false;
    }
    if (st->st_uid != geteuid()) {
        return false;
    }
    if ((st->st_mode & 077) != 0) {
        return false;
    }
    return true;
}

static int sleep_backoff(unsigned *n)
{
    struct timespec ts;
    unsigned ms = *n;

    if (ms < 1) {
        ms = 1;
    }
    if (ms > 10) {
        ms = 10;
    }
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    *n = ms < 10 ? ms + 1 : 10;
    if (nanosleep(&ts, NULL) != 0 && errno != EINTR) {
        return errno;
    }
    return 0;
}

static void write_header(posixipc_shm_header *hdr, const posixipc_shm_expect *expect)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = magic_wire();
    hdr->layout_version = expect->layout_version;
    hdr->slot_count = expect->slot_count;
    hdr->abi_tag = expect->abi_tag;
    hdr->flags = expect->flags;
    hdr->total_size = expect->total_size;
    hdr->directory_bytes = expect->directory_bytes;
    hdr->layout_digest = expect->layout_digest;
}

static int write_directory(void *map, const posixipc_shm_expect *expect)
{
    if (expect->slot_count == 0) {
        return 0;
    }
    if (expect->slots == NULL) {
        return EINVAL;
    }
    memcpy((char *)map + POSIXIPC_HEADER_BYTES, expect->slots, (size_t)expect->directory_bytes);
    return 0;
}

static int dup_name(const char *name, char **out)
{
    size_t n = strlen(name) + 1;
    char *copy = malloc(n);

    if (copy == NULL) {
        return ENOMEM;
    }
    memcpy(copy, name, n);
    *out = copy;
    return 0;
}

static void reset_handle(posixipc_shm *h)
{
    h->map = NULL;
    h->map_len = 0;
    h->hdr = NULL;
    h->name = NULL;
}

int posixipc_shm_create(const char *name, const posixipc_shm_expect *expect, posixipc_shm *out)
{
    int fd = -1;
    int rc;
    void *map = MAP_FAILED;
    struct stat st;
    posixipc_shm_header *hdr;

    if (out == NULL || expect == NULL) {
        return EINVAL;
    }
    reset_handle(out);
    rc = posixipc_shm_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    if (expect->total_size < POSIXIPC_HEADER_BYTES) {
        return EINVAL;
    }
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        return errno;
    }
    if (fstat(fd, &st) != 0) {
        rc = errno;
        goto fail;
    }
    if (!posixipc_shm_stat_permitted(&st)) {
        rc = EACCES;
        goto fail;
    }
    if (ftruncate(fd, (off_t)expect->total_size) != 0) {
        rc = errno;
        goto fail;
    }
    map = mmap(NULL, expect->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        rc = errno;
        goto fail;
    }
    if (close(fd) != 0) {
        rc = errno;
        fd = -1;
        goto fail;
    }
    fd = -1;
    hdr = map;
    write_header(hdr, expect);
    rc = write_directory(map, expect);
    if (rc != 0) {
        goto fail;
    }
    atomic_store_explicit(&hdr->state, POSIXIPC_STATE_INITIALIZING, memory_order_release);
    rc = dup_name(name, &out->name);
    if (rc != 0) {
        goto fail;
    }
    out->map = map;
    out->map_len = expect->total_size;
    out->hdr = hdr;
    return 0;

fail:
    if (fd >= 0) {
        close(fd);
    }
    if (map != MAP_FAILED) {
        munmap(map, expect->total_size);
    }
    shm_unlink(name);
    reset_handle(out);
    return rc;
}

int posixipc_shm_publish(posixipc_shm *h)
{
    if (h == NULL || h->hdr == NULL) {
        return EINVAL;
    }
    atomic_store_explicit(&h->hdr->state, POSIXIPC_STATE_READY, memory_order_release);
    return 0;
}

int posixipc_shm_mark_broken(posixipc_shm *h)
{
    if (h == NULL || h->hdr == NULL) {
        return EINVAL;
    }
    atomic_store_explicit(&h->hdr->state, POSIXIPC_STATE_BROKEN, memory_order_release);
    return 0;
}

static int compare_expect(const posixipc_shm_header *hdr, const posixipc_slot *dir, const posixipc_shm_expect *expect)
{
    uint16_t i;

    if (hdr->magic != magic_wire()) {
        return POSIXIPC_ERROR_LAYOUT_MISMATCH;
    }
    if (expect == NULL) {
        return 0;
    }
    if (hdr->layout_version != expect->layout_version || hdr->slot_count != expect->slot_count ||
        hdr->abi_tag != expect->abi_tag || hdr->total_size != expect->total_size ||
        hdr->directory_bytes != expect->directory_bytes || hdr->layout_digest != expect->layout_digest) {
        return POSIXIPC_ERROR_LAYOUT_MISMATCH;
    }
    if (expect->slot_count == 0) {
        return 0;
    }
    if (expect->slots == NULL || dir == NULL) {
        return POSIXIPC_ERROR_LAYOUT_MISMATCH;
    }
    for (i = 0; i < expect->slot_count; i++) {
        if (dir[i].kind != expect->slots[i].kind || dir[i].align != expect->slots[i].align ||
            dir[i].offset != expect->slots[i].offset || dir[i].size != expect->slots[i].size ||
            dir[i].init_flags != expect->slots[i].init_flags) {
            return POSIXIPC_ERROR_LAYOUT_MISMATCH;
        }
    }
    return 0;
}

int posixipc_shm_attach(const char *name, const posixipc_shm_expect *expect, const posixipc_deadline *deadline,
                        posixipc_shm *out)
{
    int fd = -1;
    int rc;
    unsigned backoff = 1;
    struct stat st;
    void *window = MAP_FAILED;
    void *map = MAP_FAILED;
    posixipc_shm_header *hdr;
    uint32_t total = 0;
    uint32_t state;

    if (out == NULL) {
        return EINVAL;
    }
    reset_handle(out);
    rc = posixipc_shm_validate_name(name);
    if (rc != 0) {
        return rc;
    }
    fd = shm_open(name, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        return errno;
    }

    for (;;) {
        if (posixipc_deadline_expired(deadline)) {
            rc = POSIXIPC_ERROR_NOT_READY;
            goto fail;
        }
        if (fstat(fd, &st) != 0) {
            rc = errno;
            goto fail;
        }
        if (!posixipc_shm_stat_permitted(&st)) {
            rc = EACCES;
            goto fail;
        }
        if ((uint64_t)st.st_size >= POSIXIPC_HEADER_BYTES) {
            break;
        }
        rc = sleep_backoff(&backoff);
        if (rc != 0) {
            goto fail;
        }
    }

    window = mmap(NULL, POSIXIPC_HEADER_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    if (window == MAP_FAILED) {
        rc = errno;
        goto fail;
    }
    hdr = window;
    backoff = 1;
    for (;;) {
        if (posixipc_deadline_expired(deadline)) {
            rc = POSIXIPC_ERROR_NOT_READY;
            goto fail;
        }
        state = atomic_load_explicit(&hdr->state, memory_order_acquire);
        if (state == POSIXIPC_STATE_READY) {
            break;
        }
        if (state == POSIXIPC_STATE_BROKEN) {
            rc = POSIXIPC_ERROR_BROKEN;
            goto fail;
        }
        rc = sleep_backoff(&backoff);
        if (rc != 0) {
            goto fail;
        }
    }
    total = hdr->total_size;
    if (total < POSIXIPC_HEADER_BYTES) {
        rc = POSIXIPC_ERROR_LAYOUT_MISMATCH;
        goto fail;
    }
    if (fstat(fd, &st) != 0) {
        rc = errno;
        goto fail;
    }
    if ((uint64_t)st.st_size < (uint64_t)total) {
        rc = POSIXIPC_ERROR_LAYOUT_MISMATCH;
        goto fail;
    }
    if (expect != NULL && total != expect->total_size) {
        rc = POSIXIPC_ERROR_LAYOUT_MISMATCH;
        goto fail;
    }
    munmap(window, POSIXIPC_HEADER_BYTES);
    window = MAP_FAILED;
    map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        rc = errno;
        goto fail;
    }
    if (close(fd) != 0) {
        rc = errno;
        fd = -1;
        goto fail;
    }
    fd = -1;
    hdr = map;
    rc = compare_expect(hdr, (posixipc_slot *)((char *)map + POSIXIPC_HEADER_BYTES), expect);
    if (rc != 0) {
        goto fail;
    }
    rc = dup_name(name, &out->name);
    if (rc != 0) {
        goto fail;
    }
    out->map = map;
    out->map_len = total;
    out->hdr = hdr;
    return 0;

fail:
    if (fd >= 0) {
        close(fd);
    }
    if (window != MAP_FAILED) {
        munmap(window, POSIXIPC_HEADER_BYTES);
    }
    if (map != MAP_FAILED) {
        munmap(map, total != 0 ? total : POSIXIPC_HEADER_BYTES);
    }
    reset_handle(out);
    return rc;
}

int posixipc_shm_open_or_create(const char *name, const posixipc_shm_expect *expect, const posixipc_deadline *deadline,
                                posixipc_shm *out)
{
    int rc;
    unsigned tries;

    for (tries = 0; tries < 32; tries++) {
        rc = posixipc_shm_create(name, expect, out);
        if (rc == 0) {
            return 0;
        }
        if (rc != EEXIST) {
            return rc;
        }
        rc = posixipc_shm_attach(name, expect, deadline, out);
        if (rc == 0) {
            return 0;
        }
        if (rc != ENOENT) {
            return rc;
        }
    }
    return POSIXIPC_ERROR_NOT_READY;
}

int posixipc_shm_close(posixipc_shm *h)
{
    if (h == NULL || h->map == NULL) {
        return 0;
    }
    if (munmap(h->map, h->map_len) != 0) {
        return errno;
    }
    free(h->name);
    reset_handle(h);
    return 0;
}

int posixipc_shm_unlink(const char *name)
{
    int rc = posixipc_shm_validate_name(name);

    if (rc != 0) {
        return rc;
    }
    if (shm_unlink(name) != 0) {
        return errno;
    }
    return 0;
}

int posixipc_shm_offset_ptr(const posixipc_shm *h, uint32_t offset, uint32_t size, uint32_t align, void **out)
{
    uint32_t dir_end;

    if (h == NULL || h->hdr == NULL || out == NULL) {
        return EINVAL;
    }
    dir_end = POSIXIPC_HEADER_BYTES + h->hdr->directory_bytes;
    if (offset < dir_end) {
        return EINVAL;
    }
    if (size == 0 || offset > h->hdr->total_size) {
        return EINVAL;
    }
    if (size > h->hdr->total_size - offset) {
        return EINVAL;
    }
    if (align > 1 && (offset % align) != 0) {
        return EINVAL;
    }
    *out = (char *)h->map + offset;
    return 0;
}

posixipc_shm_header *posixipc_shm_header_ptr(const posixipc_shm *h)
{
    return h == NULL ? NULL : h->hdr;
}

posixipc_slot *posixipc_shm_directory(const posixipc_shm *h)
{
    if (h == NULL || h->map == NULL || h->hdr == NULL) {
        return NULL;
    }
    if (h->hdr->slot_count == 0) {
        return NULL;
    }
    return (posixipc_slot *)((char *)h->map + POSIXIPC_HEADER_BYTES);
}
