/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <esp_ota_ops.h>
#include <voice/ble_svc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lv_demos.h>
#include <apps/common/audio/audio.h>
#include <voice/voice_state.h>
#include <esp_pm.h>
#include <esp_log.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

/* A watchdog that does not depend on the idle task being scheduled: the main
   loop bumps a counter, and if it stops moving the whole chip is restarted so
   that the bootloader can roll back a bad image. */
static volatile uint32_t g_heartbeat = 0;

static void heartbeat_task(void*)
{
    uint32_t last = 0;
    int stale = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const uint32_t now = g_heartbeat;
        if (now == last) {
            if (++stale >= 3) {          /* 15 s without progress */
                esp_restart();
            }
        } else {
            stale = 0;
            last  = now;
        }
    }
}

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_warn);   /* info logs wake the CPU; only warnings in production */
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // BLE voice link (NimBLE peripheral + PTT capture task)
    voice_start();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install apps
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppAlarmClock>());
    GetMooncake().installApp(std::make_unique<AppWatchFace>());
    GetMooncake().installApp(std::make_unique<AppStopWatch>());
    GetMooncake().installApp(std::make_unique<AppQiuqiu>());
    GetMooncake().installApp(std::make_unique<AppSetup>());

#if CONFIG_PM_ENABLE
    /* Enable power management only after every driver, task and heap user is
       up: sleeping during early init corrupts the panel/PSRAM state.
       80 MHz fixed (no DFS) + automatic light sleep gated by the display
       driver. See hal/hal_power.cpp. */
    GetHAL().powerInit(240, 80, /*light_sleep=*/true);   /* 240 MHz awake, sleep when idle */
#endif

    xTaskCreate(heartbeat_task, "heartbeat", 3072, nullptr, 2, nullptr);

    /* If this image came in via OTA the bootloader waits for a sign of life:
       confirm it once the system has been stable for a while, otherwise the
       next boot automatically rolls back to the previous slot. */
    uint32_t boot_ms = GetHAL().millis();
    bool     slot_confirmed = false;

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().screenTick();
        GetMooncake().update();
        /* Without this the main task spins at 100 %; with the screen off
           there is nothing to do for a long time, so sleep longer. */
        /* Confirm a freshly-installed image only if this build actually works:
           the bootloader rolls back automatically when we never confirm. */
        g_heartbeat++;
        if (!slot_confirmed && GetHAL().millis() - boot_ms > 30000) {
            const uint32_t uptime = GetHAL().millis();
            /* minimal self test: the UI loop runs (we are here) and the BLE
               stack is up.  A disconnected watch is fine - a dead radio is not. */
            const bool self_test_ok = ble_svc_started();
            const esp_err_t cerr = GetHAL().otaConfirmIfPending(self_test_ok);
            if (cerr == ESP_OK) {
                slot_confirmed = true;
            } else if (uptime > 90000) {
                /* still pending after 90 s: give up and let the old slot run */
                mclog::tagWarn("Main", "image never confirmed, rolling back");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(GetHAL().screen_off_active() ? 100 : 5));
    }
}
