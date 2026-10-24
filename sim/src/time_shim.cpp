// Route the C time functions through the simulated clock so that timestamps
// rendered by the UI are reproducible.
#include "sim_clock.h"
#include <ctime>
#include <sys/time.h>

extern "C" {
time_t __wrap_time(time_t* t)
{
    const time_t now = (time_t)sim_clock::epoch();
    if (t) *t = now;
    return now;
}

struct tm* __wrap_localtime(const time_t* t)
{
    time_t v = t ? *t : (time_t)sim_clock::epoch();
    return gmtime(&v);
}

struct tm* __wrap_localtime_r(const time_t* t, struct tm* out)
{
    time_t v = t ? *t : (time_t)sim_clock::epoch();
    return gmtime_r(&v, out);
}

int __wrap_gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec  = (time_t)sim_clock::epoch();
        tv->tv_usec = 0;
    }
    return 0;
}
}
