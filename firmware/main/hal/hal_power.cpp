// Power management: automatic light sleep, gated by whoever is using the bus.
//
// The rule is simple: light sleep is only allowed when *nothing* needs the
// panel, the audio path or the flash.  Every such user holds the gate; when
// the last one releases it the chip may sleep, and a button press wakes it.
//
// The panel driver (M5GFX) writes QSPI registers and DMA buffers directly and
// never takes a PM lock, so sleeping mid-transfer corrupts the panel - hence
// the gate rather than relying on drivers.
#include <hal/hal.h>
#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <mooncake_log.h>
#include <mutex>

namespace {
const char* TAG = "HAL-Power";
esp_pm_lock_handle_t s_sleep_lock = nullptr;
int s_busy = 0;                     // number of users holding the gate
std::mutex s_gate_mutex;            // guards s_busy / s_sleep_allowed
bool s_sleep_allowed = false;
}  // namespace

void Hal::powerInit(int max_mhz, int min_mhz, bool light_sleep)
{
    if (s_sleep_lock != nullptr) return;          // already initialised

    const esp_err_t lock_err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "gate", &s_sleep_lock);
    if (lock_err == ESP_OK) {
        esp_pm_lock_acquire(s_sleep_lock);        // start awake
        s_busy = 1;                               // screen is on at boot
    }

    if (light_sleep && lock_err == ESP_OK) {
        /* Wake from light sleep on a button press. Buttons are active low and
           must stay inputs with pull-ups in the sleep domain. */
        for (gpio_num_t pin : {GPIO_NUM_1, GPIO_NUM_2}) {
            gpio_sleep_set_direction(pin, GPIO_MODE_INPUT);
            gpio_sleep_set_pull_mode(pin, GPIO_PULLUP_ONLY);
            gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
        }
        esp_sleep_enable_gpio_wakeup();

        /* Keep the CO5300's chip select de-asserted while its SPI pins float. */
        gpio_sleep_set_pull_mode(GPIO_NUM_39, GPIO_PULLUP_ONLY);

    }

    esp_pm_config_t cfg = {
        .max_freq_mhz = max_mhz,
        .min_freq_mhz = min_mhz,
        .light_sleep_enable = (light_sleep && lock_err == ESP_OK),
    };
    const esp_err_t rc = esp_pm_configure(&cfg);
    mclog::tagInfo(TAG, "pm {}/{} MHz, light_sleep={}, gate={}, rc={}",
                   max_mhz, min_mhz, (int)cfg.light_sleep_enable,
                   esp_err_to_name(lock_err), esp_err_to_name(rc));
}

/* One user starts needing the buses: forbid sleep. */
void Hal::powerBusy(const char* who)
{
    if (s_sleep_lock == nullptr) return;
    std::lock_guard<std::mutex> lock(s_gate_mutex);
    s_busy++;
    if (s_sleep_allowed) {
        esp_pm_lock_acquire(s_sleep_lock);
        s_sleep_allowed = false;
        mclog::tagInfo(TAG, "awake (%s), busy=%d", who ? who : "?", s_busy);
    }
}

/* That user is done; sleep is possible once every user is done. */
void Hal::powerIdle(const char* who)
{
    if (s_sleep_lock == nullptr) return;
    std::lock_guard<std::mutex> lock(s_gate_mutex);
    if (s_busy > 0) s_busy--;
    if (s_busy == 0 && !s_sleep_allowed) {
        /* NOTE: sleeping with USB attached kills the USB-Serial-JTAG console,
           but the device keeps running - that is what we want to verify here. */
        esp_pm_lock_release(s_sleep_lock);
        s_sleep_allowed = true;
        mclog::tagInfo(TAG, "light sleep allowed (%s)", who ? who : "?");
    }
}
