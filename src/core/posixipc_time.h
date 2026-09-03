#ifndef POSIXIPC_TIME_H
#define POSIXIPC_TIME_H

#include <stdbool.h>
#include <time.h>

typedef struct
{
    clockid_t clk;
    struct timespec ts;
} posixipc_deadline;

int posixipc_now(clockid_t clk, struct timespec *out);
int posixipc_deadline_from_seconds(clockid_t clk, double seconds, posixipc_deadline *out);
void posixipc_deadline_min(const posixipc_deadline *a, const posixipc_deadline *b, posixipc_deadline *out);
bool posixipc_deadline_expired(const posixipc_deadline *d);

#endif
