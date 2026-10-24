#include "sim_display.h"
#include <mutex>
#include <vector>

namespace sim_display {
namespace {
std::vector<uint16_t> g_pixels(WIDTH * HEIGHT, 0);
std::mutex           g_mutex;
uint32_t             g_version = 0;
}  // namespace

void clear()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fill(g_pixels.begin(), g_pixels.end(), (uint16_t)0);
    ++g_version;
}

void write_rect(int x, int y, int w, int h, const uint16_t* data)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= HEIGHT) { data += w; continue; }
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= WIDTH) continue;
            g_pixels[(size_t)dy * WIDTH + dx] = data[col];
        }
        data += w;
    }
    ++g_version;
}

void lock() { g_mutex.lock(); }
void unlock() { g_mutex.unlock(); }
const uint16_t* pixels() { return g_pixels.data(); }
uint32_t version() { return g_version; }
void bump_version() { ++g_version; }
}  // namespace sim_display
