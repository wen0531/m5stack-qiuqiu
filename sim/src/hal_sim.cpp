// Host implementation of the firmware HAL.
// Hardware access is either emulated (buttons, screen state, time) or stubbed
// (audio, IMU, PMIC, OTA).  The goal is to run the real UI code unchanged.
#include <hal/hal.h>
#include "sim_input.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <thread>

namespace {
Hal g_hal;
uint32_t g_backlight = 80;
int      g_volume    = 70;
bool     g_screen_off = false;
uint32_t g_last_activity = 0;
Hal::ButtonConfig g_btn_cfg;
}  // namespace

Hal& GetHAL()
{
    return g_hal;
}

void Hal::init() {}
void Hal::delay(std::uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::uint32_t Hal::millis()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

void Hal::feedTheDog() { /* no watchdog on the host */ }

std::array<uint8_t, 6> Hal::getFactoryMac()
{
    return {0xC4, 0x9E, 0x7E, 0x12, 0xA5, 0x92};
}

std::string Hal::getFactoryMacString(std::string divider)
{
    return "C4" + divider + "9E" + divider + "7E" + divider + "12" + divider + "A5" + divider + "92";
}

void Hal::reboot() {}
void Hal::factoryReset() {}

uint8_t Hal::getBatteryLevel() { return 87; }
uint16_t Hal::getBatteryVoltage() { return 4080; }
bool Hal::isBatteryCharging(bool) { return true; }

void Hal::setBackLightBrightness(int brightness, bool)
{
    g_backlight = (uint32_t)(brightness < 0 ? 0 : (brightness > 100 ? 100 : brightness));
}
int Hal::getBackLightBrightness(bool) { return (int)g_backlight; }

void Hal::markActivity() { g_last_activity = millis(); }
bool Hal::screen_off_active() { return g_screen_off; }
void Hal::screenWake()
{
    g_screen_off = false;
    markActivity();
}
void Hal::screenTick()
{
    /* the simulator keeps the panel on unless SIM_AUTOSLEEP_MS is set */
    static const uint32_t timeout = [] {
        const char* v = std::getenv("SIM_AUTOSLEEP_MS");
        return v ? (uint32_t)std::atoi(v) : 0u;
    }();
    if (timeout && !g_screen_off && millis() - g_last_activity > timeout) g_screen_off = true;
}

LGFX_Device& Hal::getDisplay()
{
    static LGFX_Device d;
    return d;
}
LGFX_Sprite& Hal::getCanvas()
{
    static LGFX_Sprite s;
    return s;
}
void Hal::updateCanvas() {}

bool Hal::lvglLock() { return true; }
void Hal::lvglUnlock() {}
void Hal::startLvglUpdate() {}
void Hal::stopLvglUpdate() {}

Hal::TouchPoint Hal::getTouchPoint()
{
    Hal::TouchPoint tp;
    if (sim_input::touch_pressed()) {
        tp.num = 1;
        tp.x   = sim_input::touch_x();
        tp.y   = sim_input::touch_y();
    }
    return tp;
}

void Hal::setSpeakerVolume(int v, bool) { g_volume = v; }
int  Hal::getSpeakerVolume(bool) { return g_volume; }
void Hal::setSpeakerAmp(bool) {}
void Hal::setChargeLed(bool) {}
bool Hal::usbPowerPresent() { return true; }

int Hal::otaSlots(OtaSlot* out, int max)
{
    if (!out || max < 2) return 0;
    out[0] = {'A', 4940, 3200, true, true, 2};
    out[1] = {'B', 4940, 0, false, false, 0};
    return 2;
}
esp_err_t Hal::otaSwitchToOther() { return ESP_ERR_INVALID_STATE; }
esp_err_t Hal::otaCloneRunningToOther() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t Hal::otaConfirmIfPending(bool) { return ESP_OK; }
bool Hal::otaOtherSlotValid() { return false; }

void Hal::powerInit(int, int, bool) {}
void Hal::powerBusy(const char*) {}
void Hal::powerIdle(const char*) {}

int  Hal::getAudioSampleRate() { return 44100; }
void Hal::audioRecord(std::vector<int16_t>&, uint16_t, float) {}
void Hal::audioPlay(std::vector<int16_t>&, bool, bool) {}
void Hal::audioWriteStream(const int16_t*, size_t) {}
void Hal::audioIdleCheck() {}
void Hal::audioSetOutputSampleRate(int) {}
void Hal::audioSetPlaybackMode(int, bool) {}
bool Hal::audioBusy() { return false; }
void Hal::updateAudioSpectrum() {}
void Hal::playBootSfx() {}
void Hal::vibrate(uint16_t, uint8_t) {}
void Hal::stopVibrate() {}
void Hal::updateImuData() {}

void Hal::syncRtcTimeToSystem() {}
void Hal::syncSystemTimeToRtc() {}

DateYmd Hal::getDateYmd() { return {}; }
bool Hal::setDateYmd(const DateYmd&) { return true; }
TimeHms Hal::getTimeHms() { return {}; }
bool Hal::setTimeHms(const TimeHms&) { return true; }
void Hal::setTimezone(std::string_view) {}
std::string Hal::getTimezone() { return "UTC"; }

bool Hal::loadAlarmStorage(AlarmStorageSnapshot&) { return false; }
bool Hal::saveAlarmStorage(const AlarmStorageSnapshot&) { return true; }
void Hal::startAlarm() {}
void Hal::stopAlarm() {}

void Hal::updateButtonStates()
{
    /* injection happens in the control server; keep the state machine ticking */
    btnA.setRawState(millis(), sim_input::get_key('A'));
    btnB.setRawState(millis(), sim_input::get_key('B'));
}

void Hal::setButtonConfig(Hal::ButtonConfig c, bool) { g_btn_cfg = c; }
const Hal::ButtonConfig& Hal::getButtonConfig(bool) { return g_btn_cfg; }

bool Hal::loadBadgeImage(lv_obj_t*) { return false; }
bool Hal::loadNextBadgeImage(lv_obj_t*) { return false; }
bool Hal::loadPreviousBadgeImage(lv_obj_t*) { return false; }
void Hal::startBadgeEditModeViaAp(std::function<void(std::string_view)>) {}

bool Hal::shouldShowGuide() { return false; }
