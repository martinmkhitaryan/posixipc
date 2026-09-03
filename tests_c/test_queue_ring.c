#include "posixipc_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

int main(void)
{
    uint32_t depth = 4;
    uint32_t item = 8;
    uint32_t nbytes = posixipc_queue_bytes_size(depth, item);
    void *mem;
    posixipc_queue_view v;
    char buf[8];
    uint32_t n;
    int i;

    CHECK(nbytes > 0);
    mem = calloc(1, nbytes);
    CHECK(mem != NULL);
    CHECK(posixipc_queue_map(mem, nbytes, depth, item, &v) == 0);
    CHECK(posixipc_queue_init_ctrl(&v) == 0);
    CHECK(posixipc_queue_qsize(&v, &n) == 0 && n == 0);
    CHECK(posixipc_queue_get(&v, buf) == EAGAIN);
    for (i = 0; i < 4; i++) {
        memset(buf, (char)(i + 1), 8);
        CHECK(posixipc_queue_put(&v, buf) == 0);
    }
    CHECK(posixipc_queue_put(&v, buf) == EAGAIN);
    CHECK(posixipc_queue_qsize(&v, &n) == 0 && n == 4);
    CHECK(posixipc_queue_get(&v, buf) == 0);
    CHECK(buf[0] == 1);
    v.state[v.ctrl->tail] = POSIXIPC_QUEUE_RESERVED_PUT;
    v.state[v.ctrl->head] = POSIXIPC_QUEUE_RESERVED_GET;
    CHECK(posixipc_queue_recover(&v) == 0);
    CHECK(posixipc_queue_qsize(&v, &n) == 0);
    CHECK(n == 3);
    free(mem);
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
