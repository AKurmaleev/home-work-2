#define _GNU_SOURCE

#include <sched.h>
#include <stddef.h>
#include <time.h>

/* Link-time test seams exercise CLI serialization without real measurements. */
int __wrap_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *mask)
{
    (void)pid;
    (void)size;
    CPU_ZERO(mask);
    CPU_SET(0, mask);
    return 0;
}

int __wrap_sched_getcpu(void)
{
    return 0;
}

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    static long ticks;

    (void)clock_id;
    value->tv_sec = ticks / 100L;
    value->tv_nsec = (ticks % 100L) * 10000000L;
    ++ticks;
    return 0;
}
