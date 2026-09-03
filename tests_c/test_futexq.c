#include "posixipc_futexq.h"

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
    uint32_t nbytes = posixipc_futexq_bytes_size(depth, item);
    void *mem;
    posixipc_futexq_view v;
    char buf[8];
    uint32_t n;

    CHECK(nbytes > posixipc_queue_bytes_size(depth, item));
    mem = calloc(1, nbytes);
    CHECK(mem != NULL);
    CHECK(posixipc_futexq_map(mem, nbytes, depth, item, &v) == 0);
    CHECK(posixipc_futexq_init(&v) == 0);
    CHECK(v.not_full[0] == 0);
    CHECK(v.not_empty[0] == 0);
    CHECK(posixipc_queue_qsize(&v.view, &n) == 0 && n == 0);
    CHECK(posixipc_queue_put(&v.view, "abcdefgh") == 0);
    CHECK(posixipc_queue_get(&v.view, buf) == 0);
    CHECK(memcmp(buf, "abcdefgh", 8) == 0);
    free(mem);
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
