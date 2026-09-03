#define POSIXIPC_NO_PYTHON
#include "posixipc.h"

#include <stdio.h>

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void)
{
    CHECK(sizeof(posixipc_shm_header) == 64);
    CHECK(sizeof(posixipc_slot) == 16);
    CHECK(POSIXIPC_HEADER_BYTES == 64u);
    CHECK(POSIXIPC_MUTEX_CAPSULE_VERSION == 1u);
    return 0;
}
