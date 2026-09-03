#include "posixipc_config.h"
#include "posixipc_mq.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if !POSIXIPC_HAVE_MQ_OPEN
int main(void)
{
    return 0;
}
#else

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
    char name[64];
    mqd_t mq = (mqd_t)-1;
    char buf[32];
    ssize_t got = 0;
    unsigned prio = 0;
    long msgsize = 0;

    snprintf(name, sizeof(name), "/posixipc-mq-%d", (int)getpid());
    (void)posixipc_mq_unlink(name);
    CHECK(posixipc_mq_create(name, 4, 32, &mq) == 0);
    CHECK(posixipc_mq_msgsize(mq, &msgsize) == 0 && msgsize == 32);
    CHECK(posixipc_mq_send(mq, "hello", 5, 0) == 0);
    CHECK(posixipc_mq_receive(mq, buf, 32, &prio, &got) == 0);
    CHECK(got == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK(posixipc_mq_close(mq) == 0);
    CHECK(posixipc_mq_attach(name, &mq) == 0);
    CHECK(posixipc_mq_close(mq) == 0);
    CHECK(posixipc_mq_unlink(name) == 0);
    CHECK(posixipc_mq_open_or_create(name, 4, 32, &mq) == 0);
    CHECK(posixipc_mq_close(mq) == 0);
    CHECK(posixipc_mq_open_or_create(name, 8, 64, &mq) == 0);
    CHECK(posixipc_mq_msgsize(mq, &msgsize) == 0 && msgsize == 32);
    CHECK(posixipc_mq_close(mq) == 0);
    CHECK(posixipc_mq_unlink(name) == 0);
    if (g_fails != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
#endif
