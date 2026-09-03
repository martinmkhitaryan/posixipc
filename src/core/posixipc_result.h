#ifndef POSIXIPC_RESULT_H
#define POSIXIPC_RESULT_H

#include <errno.h>

#define POSIXIPC_ERROR_LAYOUT_MISMATCH 5000
#define POSIXIPC_ERROR_NOT_READY 5001
#define POSIXIPC_ERROR_BROKEN 5002
#define POSIXIPC_ERROR_INTERRUPTED 5003
#define POSIXIPC_BARRIER_SERIAL 5004

#define POSIXIPC_ERRNO_OR(expr, sentinel)                                                                              \
    __extension__({                                                                                                    \
        __typeof__(expr) _r = (expr);                                                                                  \
        (_r == (sentinel)) ? errno : 0;                                                                                \
    })

#endif
