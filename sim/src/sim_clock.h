// Deterministic simulated clock.
//
// In real-time mode the simulator follows the host clock.  In step mode time
// only advances when the test asks for it, which makes screenshots and
// animation checks reproducible.
#pragma once
#include <cstdint>

namespace sim_clock {
void     init();                     /* fixed epoch, paused unless --realtime */
void     advance(uint32_t ms);       /* step mode: move time forward */
void     set_paused(bool paused);
bool     paused();
uint32_t now_ms();                   /* simulated milliseconds since start */
void     set_epoch(int64_t unix_seconds);
int64_t  epoch();
void     get_hms(int& h, int& m, int& s);
void     get_ymd(int& y, int& mo, int& d);
}
