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
#include <cstring>
#include <mutex>

namespace {
const char* TAG = "HAL-Power";
esp_pm_lock_handle_t s_sleep_lock = nullptr;

/* The gate is counted per owner: an unmatched powerIdle() can no longer steal
   another owner's hold (that used to be able to release the panel's lock while
   the screen was still lit). */
struct GateUser {
    const char* name;
    int         count;
};
constexpr int kMaxGateUsers = 4;
GateUser s_users[kMaxGateUsers] = {};
int s_busy   = 0;                   // sum of all owner counts
std::mutex s_gate_mutex;            // guards s_users / s_busy / s_sleep_allowed
bool s_sleep_allowed = false;

/* caller holds s_gate_mutex */
int gate_find(const char* who, bool create)
{
    int free_slot = -1;
    for (int i = 0; i < kMaxGateUsers; i++) {
        if (s_users[i].count > 0) {
            if (who != nullptr && strcmp(s_users[i].name, who) == 0) return i;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create || free_slot < 0) return -1;
    s_users[free_slot].name  = who;
    s_users[free_slot].count = 0;
    return free_slot;
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/* Counted inside the idle task right before esp_light_sleep_start(). Without a
   current meter this counter is the only honest proof that the chip slept. */
volatile uint32_t s_light_sleep_entries = 0;
volatile uint64_t s_slept_us            = 0;   /* total time actually spent asleep */

/* The enter callback fires *before* the 8-tick threshold check and even when
   esp_light_sleep_start() fails, so count on the way out and only accept a real
   episode (>= 1 ms actually spent asleep). */
esp_err_t on_light_sleep_exit(int64_t slept_us, void* arg)
{
    (void)arg;
    if (slept_us > 0) s_slept_us = s_slept_us + (uint64_t)slept_us;
    if (slept_us >= 1000) s_light_sleep_entries = s_light_sleep_entries + 1;
    return ESP_OK;
}
#endif
}  // namespace

void Hal::powerInit(int max_mhz, int min_mhz, bool light_sleep)
{
    if (s_sleep_lock != nullptr) return;          // already initialised

    const esp_err_t lock_err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "gate", &s_sleep_lock);
    if (lock_err == ESP_OK) {
        esp_pm_lock_acquire(s_sleep_lock);        // start awake; the display
                                                  // claims the gate right after
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

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {};
    cbs.exit_cb                            = on_light_sleep_exit;
    const esp_err_t cbrc                   = esp_pm_light_sleep_register_cbs(&cbs);
    if (cbrc == ESP_OK) {
        mclog::tagInfo(TAG, "light sleep counter registered");
    } else {
        mclog::tagWarn(TAG, "light sleep counter registration failed: {}", esp_err_to_name(cbrc));
    }
#endif
}

uint32_t Hal::powerLightSleepCount()
{
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    return s_light_sleep_entries;
#else
    return 0;
#endif
}

/* Microseconds spent in automatic light sleep since boot. Divided by uptime it
   gives the sleep ratio - the number that tells us whether a high wake count is
   actually costing us. */
uint64_t Hal::powerSleepUs()
{
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    return s_slept_us;
#else
    return 0;
#endif
}

/* One user starts needing the buses: forbid sleep. */
void Hal::powerBusy(const char* who)
{
    if (s_sleep_lock == nullptr) return;

    bool woke = false;
    bool full = false;
    int  busy = 0;
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        const int idx = gate_find(who, true);
        if (idx < 0) {
            full = true;
        } else {
            s_users[idx].count++;
            s_busy++;
            busy = s_busy;
            if (s_sleep_allowed) {
                /* Logical and physical lock must move together: doing the
                   acquire after leaving the mutex lets a racing idle() release
                   a lock this call has not taken yet, leaving the real lock
                   held while the counters read zero (sleep forbidden forever). */
                esp_pm_lock_acquire(s_sleep_lock);
                s_sleep_allowed = false;
                woke            = true;
            }
        }
    }

    if (full) {
        mclog::tagWarn(TAG, "gate table full, {} not tracked", who ? who : "?");
        return;
    }
    if (woke) mclog::tagInfo(TAG, "awake ({}), busy={}", who ? who : "?", busy);
}

/* That user is done; sleep is possible once every user is done.  An idle for an
   owner that holds nothing is ignored instead of stealing someone else's hold. */
void Hal::powerIdle(const char* who)
{
    if (s_sleep_lock == nullptr) return;

    bool slept   = false;
    bool matched = false;
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        const int idx = gate_find(who, false);
        if (idx >= 0 && s_users[idx].count > 0) {
            matched = true;
            s_users[idx].count--;
            if (s_busy > 0) s_busy--;
            if (s_busy == 0 && !s_sleep_allowed) {
                /* NOTE: sleeping with USB attached kills the USB-Serial-JTAG
                   console, but the device keeps running. */
                esp_pm_lock_release(s_sleep_lock);
                s_sleep_allowed = true;
                slept           = true;
            }
        }
    }

    if (!matched) {
        mclog::tagWarn(TAG, "unbalanced powerIdle({}) ignored", who ? who : "?");
        return;
    }
    if (slept) mclog::tagInfo(TAG, "light sleep allowed ({})", who ? who : "?");
}
