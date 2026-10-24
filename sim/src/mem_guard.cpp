#include "mem_guard.h"
#include "sim_log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void  __real_free(void*);
}

namespace mem_guard {
namespace {

constexpr size_t kSlots  = 1 << 17;        /* 128k pointers */
constexpr size_t kMask   = kSlots - 1;
constexpr size_t kAlwaysInternal = 4096;   /* CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL */

struct Slot {
    void*  ptr = nullptr;
    size_t size = 0;
    uint8_t pool = 0;
};

Slot              g_slots[kSlots];
std::mutex        g_mutex;
std::atomic<size_t> g_used[2] = {0, 0};
size_t            g_limit[2] = {512 * 1024, 8 * 1024 * 1024};
bool              g_inited = false;

size_t key_of(void* p) { return (size_t)((uintptr_t)p >> 4) & kMask; }

bool record(void* p, size_t size, uint8_t pool)
{
    if (p == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[(key_of(p) + i) & kMask];
        if (s.ptr == nullptr) {
            s.ptr = p; s.size = size; s.pool = pool;
            g_used[pool] += size;
            return true;
        }
    }
    return false;
}

bool lookup(void* p, size_t& size, uint8_t& pool)
{
    if (p == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[(key_of(p) + i) & kMask];
        if (s.ptr == p) { size = s.size; pool = s.pool; return true; }
        if (s.ptr == nullptr) return false;
    }
    return false;
}

bool unrecord(void* p, size_t& size, uint8_t& pool)
{
    if (p == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < kSlots; ++i) {
        Slot& s = g_slots[(key_of(p) + i) & kMask];
        if (s.ptr == p) {
            size = s.size; pool = s.pool;
            g_used[pool] -= s.size;
            s = Slot{};
            return true;
        }
        if (s.ptr == nullptr) return false;
    }
    return false;
}

void fail(Pool pool, size_t want)
{
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[mem] OUT OF %s: tried to allocate %zu KB, budget %zu KB, in use %zu KB",
                  pool == POOL_PSRAM ? "PSRAM" : "internal RAM", want / 1024,
                  g_limit[pool] / 1024, g_used[pool].load() / 1024);
    sim_log_add(line);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    std::abort();          /* the supervisor reports this as a crash */
}

void check(Pool pool, size_t want)
{
    if (g_used[pool].load() + want > g_limit[pool]) fail(pool, want);
}

}  // namespace

void init()
{
    if (g_inited) return;
    g_inited = true;
    if (const char* v = std::getenv("SIM_INTERNAL_KB")) g_limit[POOL_INTERNAL] = (size_t)std::atoll(v) * 1024;
    if (const char* v = std::getenv("SIM_PSRAM_MB"))    g_limit[POOL_PSRAM]    = (size_t)std::atoll(v) * 1024 * 1024;
    char line[192];
    std::snprintf(line, sizeof(line), "[mem] budget: internal %zu KB, PSRAM %zu MB",
                  g_limit[POOL_INTERNAL] / 1024, g_limit[POOL_PSRAM] / (1024 * 1024));
    sim_log_add(line);
    std::printf("%s\n", line);
}

void* alloc(Pool pool, size_t size)
{
    check(pool, size);
    void* p = __real_malloc(size);
    if (!p) return nullptr;
    if (!record(p, size, (uint8_t)pool)) { __real_free(p); return nullptr; }
    return p;
}

void* realloc(void* ptr, size_t size)
{
    if (ptr == nullptr) return wrap_malloc(size);
    size_t old = 0; uint8_t pool = POOL_INTERNAL;
    if (!lookup(ptr, old, pool)) return __real_realloc(ptr, size);
    if (size > old) check((Pool)pool, size - old);
    void* np = __real_realloc(ptr, size);
    if (!np) return nullptr;
    if (np != ptr) { unrecord(ptr, old, pool); record(np, size, pool); }
    else {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < kSlots; ++i) {
            Slot& s = g_slots[(key_of(ptr) + i) & kMask];
            if (s.ptr == ptr) { g_used[pool] += (long)size - (long)old; s.size = size; break; }
            if (s.ptr == nullptr) break;
        }
    }
    return np;
}

void release(void* ptr)
{
    size_t size = 0; uint8_t pool = 0;
    unrecord(ptr, size, pool);
    __real_free(ptr);
}

void* wrap_malloc(size_t size)
{
    const Pool pool = size > kAlwaysInternal ? POOL_PSRAM : POOL_INTERNAL;
    check(pool, size);
    void* p = __real_malloc(size);
    if (p) record(p, size, (uint8_t)pool);
    return p;
}

void* wrap_calloc(size_t n, size_t size)
{
    const size_t total = n * size;
    const Pool pool = total > kAlwaysInternal ? POOL_PSRAM : POOL_INTERNAL;
    check(pool, total);
    void* p = __real_calloc(n, size);
    if (p) record(p, total, (uint8_t)pool);
    return p;
}

void* wrap_realloc(void* ptr, size_t size)
{
    if (ptr == nullptr) return wrap_malloc(size);
    size_t old = 0; uint8_t pool = POOL_INTERNAL;
    if (!lookup(ptr, old, pool)) return __real_realloc(ptr, size);
    if (size > old) check((Pool)pool, size - old);
    void* np = __real_realloc(ptr, size);
    if (!np) return nullptr;
    unrecord(ptr, old, pool);
    record(np, size, pool);
    return np;
}

void wrap_free(void* ptr)
{
    if (ptr == nullptr) return;
    size_t size = 0; uint8_t pool = 0;
    unrecord(ptr, size, pool);
    __real_free(ptr);
}

size_t used(Pool pool) { return g_used[pool].load(); }
size_t limit(Pool pool) { return g_limit[pool]; }
double usage_percent(Pool pool) { return 100.0 * (double)g_used[pool].load() / (double)g_limit[pool]; }

const char* report()
{
    static char buf[192];
    std::snprintf(buf, sizeof(buf), "mem_internal_kb=%zu mem_internal_limit_kb=%zu mem_psram_kb=%zu mem_psram_limit_kb=%zu",
                  g_used[POOL_INTERNAL].load() / 1024, g_limit[POOL_INTERNAL] / 1024,
                  g_used[POOL_PSRAM].load() / 1024, g_limit[POOL_PSRAM] / 1024);
    return buf;
}

}  // namespace mem_guard

/* ---- interposition: every plain malloc in the firmware goes through us ---- */
extern "C" void* __wrap_malloc(size_t n) { return mem_guard::wrap_malloc(n); }
extern "C" void* __wrap_calloc(size_t a, size_t b) { return mem_guard::wrap_calloc(a, b); }
extern "C" void* __wrap_realloc(void* p, size_t n) { return mem_guard::wrap_realloc(p, n); }
extern "C" void  __wrap_free(void* p) { mem_guard::wrap_free(p); }

/* C++ new/delete */
void* operator new(size_t n) { return mem_guard::wrap_malloc(n ? n : 1); }
void* operator new[](size_t n) { return mem_guard::wrap_malloc(n ? n : 1); }
void  operator delete(void* p) noexcept { mem_guard::wrap_free(p); }
void  operator delete[](void* p) noexcept { mem_guard::wrap_free(p); }
void  operator delete(void* p, size_t) noexcept { mem_guard::wrap_free(p); }
void  operator delete[](void* p, size_t) noexcept { mem_guard::wrap_free(p); }
