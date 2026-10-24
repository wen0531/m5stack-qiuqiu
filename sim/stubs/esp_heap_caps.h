#pragma once
#include <cstdlib>
#include <cstddef>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void* heap_caps_calloc(size_t n, size_t s, int) { return std::calloc(n, s); }
inline void heap_caps_free(void* p) { std::free(p); }
inline size_t heap_caps_get_free_size(int) { return 1024 * 1024; }
