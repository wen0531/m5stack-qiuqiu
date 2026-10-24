#include "sim_clock.h"
#include <chrono>
#include <cstdlib>
#include <ctime>

namespace sim_clock {
namespace {
uint32_t g_ms      = 0;
int64_t  g_epoch   = 0;      /* simulated wall clock at t=0 */
bool     g_paused  = false;
bool     g_inited  = false;
}  // namespace

void init()
{
    if (g_inited) return;
    g_inited = true;
    /* default deterministic start: 2026-10-24 12:00:00 UTC */
    g_epoch = 1792843200;
    if (const char* v = std::getenv("SIM_EPOCH")) g_epoch = std::atoll(v);
}

void advance(uint32_t ms) { g_ms += ms; }
void set_paused(bool p) { g_paused = p; }
bool paused() { return g_paused; }
uint32_t now_ms() { return g_ms; }

void set_epoch(int64_t unix_seconds) { g_epoch = unix_seconds; }
int64_t epoch() { return g_epoch + (int64_t)(g_ms / 1000); }

void get_hms(int& h, int& m, int& s)
{
    const int64_t t = epoch();
    const int64_t secs = t % 86400;
    h = (int)(secs / 3600);
    m = (int)((secs % 3600) / 60);
    s = (int)(secs % 60);
}

void get_ymd(int& y, int& mo, int& d)
{
    const time_t t = (time_t)epoch();
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    y  = tm_buf.tm_year + 1900;
    mo = tm_buf.tm_mon + 1;
    d  = tm_buf.tm_mday;
}
}  // namespace sim_clock
