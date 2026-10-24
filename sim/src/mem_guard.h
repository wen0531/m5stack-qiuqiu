// Device capacity emulation.
//
// The watch has ~512 KB of internal SRAM and 8 MB of PSRAM; the firmware puts
// every allocation larger than 4 KB into PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
// A PC has far more memory than that, so without limits the simulator would
// happily run firmware that could never fit on the device.  This module tracks
// allocations against the device budgets and aborts loudly when they are
// exceeded.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mem_guard {
enum Pool { POOL_INTERNAL = 0, POOL_PSRAM = 1 };

void        init();
void*       alloc(Pool pool, size_t size);      /* aborts when over budget */
void*       realloc(void* ptr, size_t size);
void        release(void* ptr);
void*       wrap_malloc(size_t size);           /* size-based pool selection */
void*       wrap_calloc(size_t n, size_t size);
void*       wrap_realloc(void* ptr, size_t size);
void        wrap_free(void* ptr);

size_t      used(Pool pool);
size_t      limit(Pool pool);
double      usage_percent(Pool pool);
const char* report();                           /* one line, for 'state' */
}
