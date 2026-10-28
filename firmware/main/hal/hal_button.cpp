/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/settings/settings.h"
#include <apps/common/audio/audio.h>
#include <mooncake_log.h>
#include <driver/gpio.h>
#include <memory>

static const std::string_view _tag = "HAL-BTN";

// USER BUTTON
#define USER_BUTTONA_PIN (gpio_num_t)2
#define USER_BUTTONB_PIN (gpio_num_t)1

/* The main loop sleeps up to 250 ms while the panel is dark, so a short press
   could start and end between two samples. Latch the edge in an ISR and poke
   the main task, so "the CPU woke up" and "the press was recorded" are the same
   event. */
/* bit0 = button A, bit1 = button B. Both are read-modify-written from the ISR
   and from the task, so every access goes through the spinlock - a lost bit
   would leave one pin disarmed for ever. */
static volatile uint8_t s_edge_mask = 0;
static volatile uint8_t s_masked    = 0;
static TaskHandle_t     s_wake_task = nullptr;
static portMUX_TYPE     s_btn_mux   = portMUX_INITIALIZER_UNLOCKED;

/* powerInit() enables the light-sleep wake source with gpio_wakeup_enable(LOW_LEVEL),
   and IDF implements that by writing the GPIO interrupt type - so this pin is
   level-triggered whether we like it or not. Mask the pin here and unmask it in
   updateButtonStates() once the key is released, otherwise holding a button
   re-enters this ISR for ever. */
static void IRAM_ATTR button_edge_isr(void* arg)
{
    const uint8_t bit    = (uint8_t)(uintptr_t)arg;
    const gpio_num_t pin = (bit == 1) ? USER_BUTTONA_PIN : USER_BUTTONB_PIN;

    gpio_intr_disable(pin);
    portENTER_CRITICAL_ISR(&s_btn_mux);
    s_masked |= bit;
    s_edge_mask |= bit;
    portEXIT_CRITICAL_ISR(&s_btn_mux);

    BaseType_t higher_prio_woken = pdFALSE;
    if (s_wake_task) vTaskNotifyGiveFromISR(s_wake_task, &higher_prio_woken);
    if (higher_prio_woken) portYIELD_FROM_ISR();
}

void Hal::buttonWakeTask(void* task)
{
    s_wake_task = (TaskHandle_t)task;
}

void Hal::button_init()
{
    mclog::tagInfo(_tag, "init");

    gpio_reset_pin(USER_BUTTONA_PIN);
    gpio_set_direction(USER_BUTTONA_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(USER_BUTTONA_PIN, GPIO_PULLUP_ONLY);

    gpio_reset_pin(USER_BUTTONB_PIN);
    gpio_set_direction(USER_BUTTONB_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(USER_BUTTONB_PIN, GPIO_PULLUP_ONLY);

    /* Latch press edges even while the main loop is in a long dark sleep.
       Level type on purpose: it has to match the light-sleep wake source set by
       powerInit(); the ISR masks the pin until release. */
    const esp_err_t svc = gpio_install_isr_service(0);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE) {
        mclog::tagWarn(_tag, "isr service failed: {} - short presses may be missed", esp_err_to_name(svc));
    }
    for (gpio_num_t pin : {USER_BUTTONA_PIN, USER_BUTTONB_PIN}) {
        const uint8_t bit = (pin == USER_BUTTONA_PIN) ? 1 : 2;
        gpio_set_intr_type(pin, GPIO_INTR_LOW_LEVEL);
        const esp_err_t rc = gpio_isr_handler_add(pin, button_edge_isr, (void*)(uintptr_t)bit);
        if (rc != ESP_OK) mclog::tagWarn(_tag, "isr add {} failed: {}", (int)pin, esp_err_to_name(rc));
        gpio_intr_enable(pin);
    }

    // Load config from settings
    getButtonConfig(true);
}

void Hal::updateButtonStates()
{
    uint8_t edges = 0;
    uint8_t masked = 0;
    portENTER_CRITICAL(&s_btn_mux);
    edges       = s_edge_mask;
    s_edge_mask = 0;
    masked      = s_masked;
    portEXIT_CRITICAL(&s_btn_mux);

    if (edges) {
        GetHAL().markActivity();
        if (GetHAL().screen_off_active()) GetHAL().screenWake();
    }

    /* Re-arm the pins whose key is already released. The hardware interrupt of a
       masked pin is off, so no ISR can set that bit while we clear it. */
    if ((masked & 1) && gpio_get_level(USER_BUTTONA_PIN) != 0) {
        portENTER_CRITICAL(&s_btn_mux);
        s_masked &= (uint8_t)~1;
        portEXIT_CRITICAL(&s_btn_mux);
        gpio_intr_enable(USER_BUTTONA_PIN);
    }
    if ((masked & 2) && gpio_get_level(USER_BUTTONB_PIN) != 0) {
        portENTER_CRITICAL(&s_btn_mux);
        s_masked &= (uint8_t)~2;
        portEXIT_CRITICAL(&s_btn_mux);
        gpio_intr_enable(USER_BUTTONB_PIN);
    }

    btnA.setRawState(millis(), !gpio_get_level(USER_BUTTONA_PIN));
    btnB.setRawState(millis(), !gpio_get_level(USER_BUTTONB_PIN));
    // btnPwr.setRawState(millis(), pmic_get_pwr_btn_state());

    auto& config = getButtonConfig();
    if (btnA.wasReleased()) {
        mclog::tagInfo(_tag, "BTN-DBG A released");
    }
    if (btnA.wasPressed() || btnB.wasPressed()) {
        GetHAL().markActivity();
        if (GetHAL().screen_off_active()) {
            GetHAL().screenWake();
        }
    }
    if (btnA.wasPressed()) {
        mclog::tagInfo(_tag, "BTN-DBG A pressed, sfx={}", config.sfxEnabled);
        if (config.sfxEnabled) {
            audio::play_tone_from_midi(62 + 32, 0.02);
        }
        if (config.vibrateEnabled) {
            vibrate(20, 60);
        }
    } else if (btnB.wasPressed()) {
        mclog::tagInfo(_tag, "BTN-DBG B pressed, sfx={}", config.sfxEnabled);
        if (config.sfxEnabled) {
            audio::play_tone_from_midi(64 + 32, 0.02);
        }
        if (config.vibrateEnabled) {
            vibrate(20, 60);
        }
    }
}

void Hal::setButtonConfig(ButtonConfig config, bool saveToSettings)
{
    _btn_config = config;
    if (saveToSettings) {
        Settings settings(std::string(Hal::SettingsNs), true);
        settings.SetBool("btn_sfx", config.sfxEnabled);
        settings.SetBool("btn_vibrate", config.vibrateEnabled);
        mclog::tagInfo(_tag, "config saved to settings: sfx={}, vibrate={}", config.sfxEnabled, config.vibrateEnabled);
    }
}

const Hal::ButtonConfig& Hal::getButtonConfig(bool loadFromSettings)
{
    if (loadFromSettings) {
        Settings settings(std::string(Hal::SettingsNs), false);
        _btn_config.sfxEnabled     = settings.GetBool("btn_sfx", true);
        _btn_config.vibrateEnabled = settings.GetBool("btn_vibrate", true);
        mclog::tagInfo(_tag, "config loaded from settings: sfx={}, vibrate={}", _btn_config.sfxEnabled,
                       _btn_config.vibrateEnabled);
    }
    return _btn_config;
}
