#pragma once
#include <cstddef>
#include "../src/mem_guard.h"
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
inline void* heap_caps_malloc(size_t n, int caps)
{
    return mem_guard::alloc((caps & MALLOC_CAP_SPIRAM) ? mem_guard::POOL_PSRAM : mem_guard::POOL_INTERNAL, n);
}
inline void* heap_caps_calloc(size_t n, size_t s, int caps)
{
    void* p = heap_caps_malloc(n * s, caps);
    if (p) __builtin_memset(p, 0, n * s);
    return p;
}
inline void heap_caps_free(void* p) { mem_guard::release(p); }
inline size_t heap_caps_get_free_size(int caps)
{
    const auto pool = (caps & MALLOC_CAP_SPIRAM) ? mem_guard::POOL_PSRAM : mem_guard::POOL_INTERNAL;
    const size_t lim = mem_guard::limit(pool), use = mem_guard::used(pool);
    return use >= lim ? 0 : lim - use;
}
