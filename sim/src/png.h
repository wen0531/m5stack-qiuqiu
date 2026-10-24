#pragma once
#include <cstdint>
bool write_png_rgb565(const char* path, const uint16_t* fb, int w, int h);
