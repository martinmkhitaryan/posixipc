#include "posixipc_layout.h"
#include "posixipc_result.h"
#include "posixipc_shm.h"

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fails;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static void fill_name(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "/posixipc-%s-%d", tag, (int)getpid());
}

static int make_bytes_expect(posixipc_slot *slot, posixipc_shm_expect *expect, uint32_t nbytes)
{
    memset(slot, 0, sizeof(*slot));
    slot->kind = POSIXIPC_KIND_BYTES;
    slot->size = nbytes;
    slot->init_flags = 0;
    return posixipc_layout_build(slot, 1, expect);
}

static int wait_exit(pid_t pid)
{
    int st = 0;

    if (waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    if (!WIFEXITED(st)) {
        return -1;
    }
    return WEXITSTATUS(st);
}

static void test_names(void)
{
    CHECK(posixipc_shm_validate_name("foo") == EINVAL);
    CHECK(posixipc_shm_validate_name("/a/b") == EINVAL);
    CHECK(posixipc_shm_validate_name("") == EINVAL);
    CHECK(posixipc_shm_validate_name("/") == EINVAL);
    CHECK(posixipc_shm_validate_name("/.") == EINVAL);
    CHECK(posixipc_shm_validate_name("/..") == EINVAL);
    CHECK(posixipc_shm_validate_name("/ok") == 0);
    {
        char longn[NAME_MAX + 8];
        size_t i;

        longn[0] = '/';
        for (i = 1; i <= (size_t)NAME_MAX + 1; i++) {
            longn[i] = 'a';
        }
        longn[NAME_MAX + 2] = '\0';
        CHECK(posixipc_shm_validate_name(longn) == ENAMETOOLONG);
    }
}

static void test_stat_predicate(void)
{
    struct stat st;

    memset(&st, 0, sizeof(st));
    st.st_uid = geteuid();
    st.st_mode = 0600;
    CHECK(posixipc_shm_stat_permitted(&st));
    st.st_mode = 0666;
    CHECK(!posixipc_shm_stat_permitted(&st));
    st.st_mode = 0600;
    st.st_uid = geteuid() + 1;
    CHECK(!posixipc_shm_stat_permitted(&st));
}

static void test_digest_vectors(void)
{
    posixipc_slot a[2];
    posixipc_slot b[2];
    posixipc_shm_expect ea;
    posixipc_shm_expect eb;
    uint32_t empty;

    empty = posixipc_layout_digest(POSIXIPC_LAYOUT_VERSION, posixipc_abi_tag(), NULL, 0);
    CHECK(empty != 0);

    memset(a, 0, sizeof(a));
    a[0].kind = POSIXIPC_KIND_MUTEX;
    CHECK(posixipc_layout_build(a, 1, &ea) == 0);
    CHECK(ea.layout_digest != 0);
    CHECK(ea.slots[0].offset >= POSIXIPC_HEADER_BYTES + ea.directory_bytes);

    memset(a, 0, sizeof(a));
    a[0].kind = POSIXIPC_KIND_MUTEX;
    a[1].kind = POSIXIPC_KIND_COND;
    CHECK(posixipc_layout_build(a, 2, &ea) == 0);

    memset(b, 0, sizeof(b));
    b[0].kind = POSIXIPC_KIND_MUTEX;
    b[1].kind = POSIXIPC_KIND_MUTEX;
    CHECK(posixipc_layout_build(b, 2, &eb) == 0);
    CHECK(ea.layout_digest != eb.layout_digest);

    memset(a, 0, sizeof(a));
    a[0].kind = POSIXIPC_KIND_MUTEX;
    a[1].kind = POSIXIPC_KIND_MUTEX;
    CHECK(posixipc_layout_build(a, 2, &ea) == 0);
    CHECK(ea.layout_digest == eb.layout_digest);
}

static void test_create_attach(void)
{
    char name[64];
    posixipc_slot slot;
    posixipc_shm_expect expect;
    posixipc_shm parent;
    posixipc_shm childh;
    posixipc_deadline dl;
    pid_t pid;
    int rc;
    posixipc_slot *dir;

    fill_name(name, sizeof(name), "ca");
    CHECK(make_bytes_expect(&slot, &expect, 64) == 0);
    CHECK(posixipc_shm_create(name, &expect, &parent) == 0);
    CHECK(posixipc_shm_publish(&parent) == 0);
    dir = posixipc_shm_directory(&parent);
    CHECK(dir != NULL);
    CHECK(dir[0].kind == POSIXIPC_KIND_BYTES);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        CHECK(posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 2.0, &dl) == 0);
        rc = posixipc_shm_attach(name, &expect, &dl, &childh);
        if (rc != 0) {
            _exit(1);
        }
        dir = posixipc_shm_directory(&childh);
        if (dir == NULL || dir[0].kind != POSIXIPC_KIND_BYTES) {
            _exit(2);
        }
        posixipc_shm_close(&childh);
        _exit(0);
    }
    CHECK(wait_exit(pid) == 0);
    CHECK(posixipc_shm_close(&parent) == 0);
    CHECK(posixipc_shm_close(&parent) == 0);
    CHECK(posixipc_shm_unlink(name) == 0);
    CHECK(posixipc_shm_unlink(name) == ENOENT);
}

static void test_not_ready_and_broken(void)
{
    char name[64];
    posixipc_slot slot;
    posixipc_shm_expect expect;
    posixipc_shm h;
    posixipc_deadline dl;
    pid_t pid;

    fill_name(name, sizeof(name), "nr");
    CHECK(make_bytes_expect(&slot, &expect, 32) == 0);
    CHECK(posixipc_shm_create(name, &expect, &h) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 0.3, &dl);
        _exit(posixipc_shm_attach(name, &expect, &dl, &(posixipc_shm){0}) == POSIXIPC_ERROR_NOT_READY ? 0 : 1);
    }
    CHECK(wait_exit(pid) == 0);
    CHECK(posixipc_shm_mark_broken(&h) == 0);
    pid = fork();
    if (pid == 0) {
        posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 0.3, &dl);
        _exit(posixipc_shm_attach(name, &expect, &dl, &(posixipc_shm){0}) == POSIXIPC_ERROR_BROKEN ? 0 : 1);
    }
    CHECK(wait_exit(pid) == 0);
    posixipc_shm_close(&h);
    posixipc_shm_unlink(name);
}

static void test_mismatch(void)
{
    char name[64];
    posixipc_slot slot;
    posixipc_shm_expect expect;
    posixipc_shm_expect bad;
    posixipc_shm h;
    posixipc_deadline dl;
    pid_t pid;

    fill_name(name, sizeof(name), "mm");
    CHECK(make_bytes_expect(&slot, &expect, 32) == 0);
    CHECK(posixipc_shm_create(name, &expect, &h) == 0);
    CHECK(posixipc_shm_publish(&h) == 0);
    bad = expect;
    bad.layout_digest ^= 1u;
    pid = fork();
    if (pid == 0) {
        posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 1.0, &dl);
        _exit(posixipc_shm_attach(name, &bad, &dl, &(posixipc_shm){0}) == POSIXIPC_ERROR_LAYOUT_MISMATCH ? 0 : 1);
    }
    CHECK(wait_exit(pid) == 0);
    h.hdr->abi_tag ^= 1u;
    pid = fork();
    if (pid == 0) {
        posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 1.0, &dl);
        _exit(posixipc_shm_attach(name, &expect, &dl, &(posixipc_shm){0}) == POSIXIPC_ERROR_LAYOUT_MISMATCH ? 0 : 1);
    }
    CHECK(wait_exit(pid) == 0);
    posixipc_shm_close(&h);
    posixipc_shm_unlink(name);
}

static void test_mode_reject(void)
{
    char name[64];
    posixipc_slot slot;
    posixipc_shm_expect expect;
    posixipc_shm h;
    posixipc_deadline dl;
    int fd;
    pid_t pid;
    char path[128];

    fill_name(name, sizeof(name), "md");
    CHECK(make_bytes_expect(&slot, &expect, 32) == 0);
    CHECK(posixipc_shm_create(name, &expect, &h) == 0);
    CHECK(posixipc_shm_publish(&h) == 0);
    snprintf(path, sizeof(path), "/dev/shm/%s", name + 1);
    fd = open(path, O_RDWR);
    if (fd >= 0) {
        CHECK(fchmod(fd, 0666) == 0);
        close(fd);
        pid = fork();
        if (pid == 0) {
            posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 1.0, &dl);
            _exit(posixipc_shm_attach(name, &expect, &dl, &(posixipc_shm){0}) == EACCES ? 0 : 1);
        }
        CHECK(wait_exit(pid) == 0);
        fd = open(path, O_RDWR);
        if (fd >= 0) {
            fchmod(fd, 0600);
            close(fd);
        }
    }
    posixipc_shm_close(&h);
    posixipc_shm_unlink(name);
}

static void test_header_window(void)
{
    char name[64];
    posixipc_shm_expect expect;
    posixipc_deadline dl;
    int fd;
    pid_t pid;
    void *map;
    posixipc_shm_header *hdr;

    fill_name(name, sizeof(name), "hw");
    memset(&expect, 0, sizeof(expect));
    expect.layout_version = POSIXIPC_LAYOUT_VERSION;
    expect.abi_tag = posixipc_abi_tag();
    expect.total_size = POSIXIPC_HEADER_BYTES;
    expect.layout_digest = 0;
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    pid = fork();
    if (pid == 0) {
        posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 2.0, &dl);
        _exit(posixipc_shm_attach(name, &expect, &dl, &(posixipc_shm){0}) == 0 ? 0 : 1);
    }
    usleep(50000);
    CHECK(ftruncate(fd, POSIXIPC_HEADER_BYTES) == 0);
    map = mmap(NULL, POSIXIPC_HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    hdr = map;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = htobe32(POSIXIPC_SHM_MAGIC);
    hdr->layout_version = expect.layout_version;
    hdr->abi_tag = expect.abi_tag;
    hdr->total_size = expect.total_size;
    atomic_store_explicit(&hdr->state, POSIXIPC_STATE_READY, memory_order_release);
    munmap(map, POSIXIPC_HEADER_BYTES);
    close(fd);
    CHECK(wait_exit(pid) == 0);
    posixipc_shm_unlink(name);
}

static void test_race(void)
{
    int i;

    for (i = 0; i < 1000; i++) {
        char name[80];
        posixipc_slot slot;
        posixipc_shm_expect expect;
        posixipc_shm h;
        posixipc_deadline dl;
        pid_t pid;

        snprintf(name, sizeof(name), "/posixipc-r-%d-%d", (int)getpid(), i);
        CHECK(make_bytes_expect(&slot, &expect, 16) == 0);
        pid = fork();
        if (pid == 0) {
            int rc;
            posixipc_shm ch;

            posixipc_deadline_from_seconds(CLOCK_MONOTONIC, 5.0, &dl);
            for (;;) {
                rc = posixipc_shm_attach(name, &expect, &dl, &ch);
                if (rc == 0) {
                    break;
                }
                if (rc != ENOENT && rc != POSIXIPC_ERROR_NOT_READY) {
                    break;
                }
                if (posixipc_deadline_expired(&dl)) {
                    break;
                }
                usleep(1000);
            }
            if (rc == 0) {
                posixipc_shm_close(&ch);
            }
            _exit(rc == 0 ? 0 : 1);
        }
        CHECK(posixipc_shm_create(name, &expect, &h) == 0);
        CHECK(posixipc_shm_publish(&h) == 0);
        CHECK(wait_exit(pid) == 0);
        posixipc_shm_close(&h);
        posixipc_shm_unlink(name);
        if (g_fails) {
            return;
        }
    }
}

int main(void)
{
    test_names();
    test_stat_predicate();
    test_digest_vectors();
    test_create_attach();
    test_not_ready_and_broken();
    test_mismatch();
    test_mode_reject();
    test_header_window();
    test_race();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
