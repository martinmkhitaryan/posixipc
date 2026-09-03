#include "posixipc_nsem.h"
#include "posixipc_sem.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    sem_t *a = NULL;
    sem_t *b = NULL;

    snprintf(name, sizeof(name), "/posixipc-nsem-%d", (int)getpid());
    (void)posixipc_nsem_unlink(name);
    CHECK(posixipc_nsem_create(name, 1, &a) == 0);
    CHECK(a != NULL);
    CHECK(posixipc_sem_trywait(a) == 0);
    CHECK(posixipc_sem_trywait(a) == EAGAIN);
    CHECK(posixipc_nsem_close(a) == 0);
    CHECK(posixipc_nsem_attach(name, &b) == 0);
    CHECK(posixipc_sem_trywait(b) == EAGAIN);
    CHECK(posixipc_sem_post(b) == 0);
    CHECK(posixipc_nsem_close(b) == 0);
    CHECK(posixipc_nsem_unlink(name) == 0);
    CHECK(posixipc_nsem_attach(name, &a) == ENOENT);
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    return 0;
}
