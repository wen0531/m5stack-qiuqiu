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
#include <driver/gpio.h>
#include <voice/ble_svc.h>
#include <i18n/i18n.h>
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

/* 5 s window at boot: press B to start the other firmware slot.
   Both slots run this firmware, so the choice is reversible. */
static void boot_chooser()
{
    Hal::OtaSlot slots[2] = {};
    if (GetHAL().otaSlots(slots, 2) < 2) return;

    const Hal::OtaSlot* other = nullptr;
    for (int i = 0; i < 2; ++i) {
        if (!slots[i].running) other = &slots[i];
    }
    if (other == nullptr || !other->valid) return;   /* nothing to switch to */

    lv_obj_t *panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 466, 466);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(panel);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);

    char text[128];
    for (int t = 5; t > 0; --t) {
        snprintf(text, sizeof(text), "Booting %c system\n\npress B for %c system\n%d",
                 slots[0].running ? 'A' : 'B', other->name, t);
        lv_label_set_text(label, text);
        for (int i = 0; i < 10; ++i) {          /* 10 x 100 ms per second */
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(GPIO_NUM_1) == 0) {   /* B button, active low */
                lv_obj_del(panel);
                if (GetHAL().otaSwitchToOther() == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
                return;
            }
        }
    }
    lv_obj_del(panel);
}

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_warn);   /* info logs wake the CPU; only warnings in production */
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();
    i18nInit();

    boot_chooser();

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
    GetHAL().powerInit(160, 80, /*light_sleep=*/true);   /* 160 MHz is plenty for LVGL; sleep when idle */
    GetHAL().powerBusy("display");                        /* the panel is lit from here on */
    GetHAL().buttonWakeTask((void*)xTaskGetCurrentTaskHandle());
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
        /* Inputs are consumed above, so run the screen state machine once more:
           otherwise a press that just woke us would wait for the next iteration
           (and that iteration's dark-sleep timeout) before the panel lights. */
        GetHAL().screenTick();
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
        if (GetHAL().screen_off_active()) {
            /* Sleep until a button edge arrives; the timeout is only a guard
               so housekeeping still runs while nobody touches the watch. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
