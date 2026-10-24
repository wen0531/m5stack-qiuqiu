// Virtual panel: the single 466x466 RGB565 surface that both the LVGL flush
// callback and direct LovyanGFX writes land in.
#pragma once
#include <cstdint>
#include <cstddef>

namespace sim_display {
constexpr int WIDTH  = 466;
constexpr int HEIGHT = 466;

void clear();
/* x/y is the top-left corner, data is w*h RGB565 pixels, row-major */
void write_rect(int x, int y, int w, int h, const uint16_t* data);
void lock();
void unlock();
const uint16_t* pixels();     /* call with lock held */
uint32_t version();
void bump_version();
}  // namespace sim_display
