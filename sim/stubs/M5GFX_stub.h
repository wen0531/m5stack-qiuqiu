// Stand-in for LovyanGFX/M5GFX that writes into the simulator's virtual panel,
// so the qiuqiu app's direct pixel path is captured by screenshots too.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "../src/sim_display.h"

namespace lgfx {
struct rgb565_t {
    uint16_t v;
    rgb565_t() = default;
    rgb565_t(uint16_t x) : v(x) {}
    operator uint16_t() const { return v; }
};
struct rgb888_t {
    uint8_t r, g, b;
};
}  // namespace lgfx

class LGFX_Device {
public:
    int  width() const { return sim_display::WIDTH; }
    int  height() const { return sim_display::HEIGHT; }
    void setBrightness(uint8_t) {}

    void startWrite() { _in_write = true; }
    void endWrite() { _in_write = false; }

    void setAddrWindow(int x, int y, int w, int h)
    {
        _wx = x; _wy = y; _ww = w; _wh = h; _wx0 = x; _wy0 = y; _written = 0;
    }

    void writePixels(const uint16_t* data, uint32_t len)
    {
        int remaining = (int)len;
        while (remaining > 0) {
            const int col = _written % (_ww > 0 ? _ww : 1);
            const int row = _written / (_ww > 0 ? _ww : 1);
            if (row >= _wh) break;
            const int n = (_ww - col) < remaining ? (_ww - col) : remaining;
            sim_display::write_rect(_wx + col, _wy + row, n, 1, data);
            data += n;
            _written += n;
            remaining -= n;
        }
    }
    void writePixels(const lgfx::rgb565_t* data, uint32_t len)
    {
        writePixels((const uint16_t*)data, len);
    }
    void writePixels(const void* data, size_t len)
    {
        writePixels((const uint16_t*)data, (uint32_t)(len / 2));
    }

    void pushImage(int, int, int, int, const uint16_t*) {}
    void fillRect(int x, int y, int w, int h, uint32_t color)
    {
        /* only used for clears; fill the rect with the low 16 bits */
        static std::vector<uint16_t> line;
        line.assign((size_t)(w > 0 ? w : 0), (uint16_t)color);
        for (int row = 0; row < h; ++row) sim_display::write_rect(x, y + row, w, 1, line.data());
    }
    void fillScreen(uint32_t color) { fillRect(0, 0, width(), height(), color); }
    void drawPixel(int x, int y, uint32_t color)
    {
        uint16_t c = (uint16_t)color;
        sim_display::write_rect(x, y, 1, 1, &c);
    }

    struct StubCanvas {
        int  width() const { return 466; }
        int  height() const { return 466; }
        void setBrightness(uint8_t) {}
    };
    StubCanvas& panel() { static StubCanvas c; return c; }

private:
    bool _in_write = false;
    int  _wx = 0, _wy = 0, _ww = 466, _wh = 466, _wx0 = 0, _wy0 = 0;
    int  _written = 0;
};
using M5GFX = LGFX_Device;

class LGFX_Sprite : public LGFX_Device {
public:
    void createSprite(int, int) {}
    void deleteSprite() {}
    void pushSprite(int, int) {}
    void fillSprite(uint32_t) {}
    void* getBuffer() { return nullptr; }
};
