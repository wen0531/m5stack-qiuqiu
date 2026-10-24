#pragma once
#include <cstdint>
#include <chrono>
inline int64_t esp_timer_get_time()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
