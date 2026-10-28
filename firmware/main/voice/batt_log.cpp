#include "batt_log.h"
#include "ble_svc.h"
#include "hal/hal.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

static const char* TAG = "batt-log";

namespace {

constexpr int kCap      = 1440;   // 24 h at one sample per minute
constexpr int kPeriodMs = 60000;

struct Sample {
    uint32_t uptime_s;
    uint16_t mv;
    uint8_t pct;
    uint8_t pad;
    uint32_t ls;      /* cumulative light-sleep episodes >= 1 ms (see hal_power.cpp) */
    uint32_t slp_ms;  /* cumulative time asleep since boot, ms */
    uint32_t adv;     /* times advertising had to be restarted */
    int32_t  advrc;   /* rc of the last ble_gap_adv_start (0 = fine) */
};

Sample* s_buf = nullptr;          // PSRAM
int s_head   = 0;
int s_count  = 0;

}  // namespace

static void batt_log_task(void*)
{
    s_buf = (Sample*)heap_caps_malloc(sizeof(Sample) * kCap, MALLOC_CAP_SPIRAM);
    if (s_buf == nullptr) {
        ESP_LOGE(TAG, "no memory for telemetry ring");
        vTaskDelete(nullptr);
        return;
    }
    memset(s_buf, 0, sizeof(Sample) * kCap);

    int tick = 0;
    while (true) {
        Sample& sm = s_buf[s_head];
        sm.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        sm.mv       = GetHAL().getBatteryVoltage();
        sm.pct      = GetHAL().getBatteryLevel();
        sm.ls       = GetHAL().powerLightSleepCount();
        sm.slp_ms   = (uint32_t)(GetHAL().powerSleepUs() / 1000ull);
        sm.adv      = ble_svc_adv_restarts();
        sm.advrc    = ble_svc_adv_last_rc();
        s_head      = (s_head + 1) % kCap;
        if (s_count < kCap) s_count++;

        if ((tick % 10) == 0) {   // log every 10 min
            ESP_LOGI(TAG, "batt t=%lus %umV %u%%", (unsigned long)sm.uptime_s, sm.mv, sm.pct);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    }
}

void batt_log_start()
{
    if (xTaskCreate(batt_log_task, "batt_log", 4096, nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

void batt_log_dump()
{
    if (s_buf == nullptr || s_count == 0) {
        const char* empty = "batt,no samples\n";
        ble_svc_debug_send((const uint8_t*)empty, strlen(empty));
        return;
    }

    /* Build the CSV newest-last so the slope reads naturally. */
    static char chunk[220];
    const char* header = "uptime_s,vbat_mv,pct,ls,slp_ms,adv,advrc\n";
    ble_svc_debug_send((const uint8_t*)header, strlen(header));

    int start = (s_count == kCap) ? s_head : 0;
    for (int i = 0; i < s_count; i++) {
        const Sample& sm = s_buf[(start + i) % kCap];
        int n = snprintf(chunk, sizeof(chunk), "%lu,%u,%u,%lu,%lu,%lu,%ld\n",
                         (unsigned long)sm.uptime_s, sm.mv, sm.pct,
                         (unsigned long)sm.ls, (unsigned long)sm.slp_ms,
                         (unsigned long)sm.adv, (long)sm.advrc);
        if (n > 0) ble_svc_debug_send((const uint8_t*)chunk, (size_t)n);
        vTaskDelay(pdMS_TO_TICKS(15));   // don't overflow the debug path
    }
    const char* tail = "batt,done\n";
    ble_svc_debug_send((const uint8_t*)tail, strlen(tail));
}
