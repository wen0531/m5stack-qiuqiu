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
#include <lv_demos.h>
#include <apps/common/audio/audio.h>
#include <voice/voice_state.h>
#include <esp_pm.h>
#include <esp_log.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

#if CONFIG_PM_ENABLE
    /* DFS + auto light sleep. Off by default: this board's QSPI panel and
       Octal PSRAM hang when the CPU clock changes. */
    {
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz = 160,
            .min_freq_mhz = 80,
            .light_sleep_enable = false,   /* DFS only: light sleep hangs on this board */
        };
        esp_err_t pm_rc = esp_pm_configure(&pm_cfg);
        ESP_LOGI("main", "esp_pm_configure rc=%d (160/80, light sleep %s)",
                 pm_rc, pm_cfg.light_sleep_enable ? "on" : "off");
    }
#endif

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
    GetMooncake().installApp(std::make_unique<AppBadge>());
    GetMooncake().installApp(std::make_unique<AppImu>());
    GetMooncake().installApp(std::make_unique<AppFft>());
    GetMooncake().installApp(std::make_unique<AppLuckyWheel>());
    GetMooncake().installApp(std::make_unique<AppQiuqiu>());
    GetMooncake().installApp(std::make_unique<AppSetup>());
    // GetMooncake().installApp(std::make_unique<AppTemplate>());

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().screenTick();
        GetMooncake().update();
    }
}
