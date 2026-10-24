// Minimal PNG writer (RGB565 framebuffer -> RGB888 PNG) using zlib.
#include "png.h"

#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <vector>

bool write_png_rgb565(const char* path, const uint16_t* fb, int w, int h)
{
    std::vector<unsigned char> png;
    if (!png_to_memory(fb, w, h, png)) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}

static void put32(std::vector<unsigned char>& v, uint32_t x)
{
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}
static void put_chunk(std::vector<unsigned char>& out, const char* type,
                      const std::vector<unsigned char>& data)
{
    put32(out, (uint32_t)data.size());
    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, body.data(), (uInt)body.size());
    put32(out, (uint32_t)crc);
}

bool png_to_memory(const uint16_t* fb, int w, int h, std::vector<unsigned char>& out)
{
    std::vector<unsigned char> raw;
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);                       /* filter: none */
        for (int x = 0; x < w; ++x) {
            uint16_t c = fb[(size_t)y * w + x];
            unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            raw.push_back((unsigned char)((r << 3) | (r >> 2)));
            raw.push_back((unsigned char)((g << 2) | (g >> 4)));
            raw.push_back((unsigned char)((b << 3) | (b >> 2)));
        }
    }
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<unsigned char> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    comp.resize(clen);

    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0);
    ihdr.push_back(0); ihdr.push_back(0);
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", comp);
    put_chunk(png, "IEND", {});
    out = std::move(png);
    return true;
}
