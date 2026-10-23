/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <esp_timer.h>
#include "utils/settings/settings.h"
#include <mooncake_log.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <memory>

static const std::string_view _tag = "HAL-Display";

/* -------------------------------------------------------------------------- */
/*                               Amoled display                               */
/* -------------------------------------------------------------------------- */
static constexpr gpio_num_t cfg_pin_sclk = GPIO_NUM_40;
static constexpr gpio_num_t cfg_pin_io0  = GPIO_NUM_41;
static constexpr gpio_num_t cfg_pin_io1  = GPIO_NUM_42;
static constexpr gpio_num_t cfg_pin_io2  = GPIO_NUM_46;
static constexpr gpio_num_t cfg_pin_io3  = GPIO_NUM_45;
static constexpr gpio_num_t cfg_pin_cs   = GPIO_NUM_39;
static constexpr gpio_num_t cfg_pin_te   = GPIO_NUM_38;
static constexpr gpio_num_t cfg_pin_rst  = GPIO_NUM_NC;

class Panel_CO5300 : public lgfx::Panel_AMOLED {
public:
    Panel_CO5300(void)
    {
        _cfg.memory_width = _cfg.panel_width = 480;
        _cfg.memory_height = _cfg.panel_height = 480;
        _write_depth                           = lgfx::color_depth_t::rgb565_2Byte;
        _read_depth                            = lgfx::color_depth_t::rgb565_2Byte;
    }

    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            0x11, 0 + CMD_INIT_DELAY,
            150,  // Sleep out
            0xC4, 1,
            0x80, 0x35,
            1,    0x80,
            0x44, 2,
            0x01, 0xD2,  // Tear Effect Line = 0x1D2 == 466
            0x53, 1,
            0x20, 0x20,
            0,    0x36,
            1,    0,
            0x51, 1,
            0xA0, 0x29,
            0,    0xff,
            0xff  // end
        };
        switch (listno) {
            case 0:
                return list0;
            default:
                return nullptr;
        }
    }
};

class M5StopWatch : public M5GFX {
    lgfx::Bus_SPI _bus_instance;
    Panel_CO5300 _panel_instance;

public:
    M5StopWatch(void)
    {
    }

    // static constexpr int in_i2c_port                   = 0;  // I2C_NUM_0

    bool init_impl(bool use_reset, bool use_clear) override
    {
        {
            auto cfg = _bus_instance.config();

            cfg.freq_write = 80000000;
            cfg.freq_read  = 10000000;  // irrelevant

            cfg.pin_sclk = cfg_pin_sclk;
            cfg.pin_io0  = cfg_pin_io0;
            cfg.pin_io1  = cfg_pin_io1;
            cfg.pin_io2  = cfg_pin_io2;
            cfg.pin_io3  = cfg_pin_io3;

            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;  // SPI_MODE0;
            cfg.spi_3wire   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg         = _panel_instance.config();
            cfg.pin_rst      = cfg_pin_rst;
            cfg.pin_cs       = cfg_pin_cs;
            cfg.panel_width  = 468;
            cfg.panel_height = 466;
            cfg.offset_x     = 6;
            cfg.offset_y     = 0;

            cfg.readable = false;

            _panel_instance.config(cfg);
        }

        setPanel(&_panel_instance);

        lgfx::pinMode(cfg_pin_te, lgfx::pin_mode_t::input_pullup);
        // lgfx::i2c::init(in_i2c_port);

        // io_expander.digitalWrite(PY32_L3B_EN_PIN, 1);
        // io_expander.digitalWrite(PY32_OLED_RST_PIN, 1);

        if (!LGFX_Device::init_impl(use_reset, use_clear)) return false;

        enableFrameBuffer(true);

        _panel_instance.setBrightness(128);

        return true;
    }

    /* real AMOLED power commands (the framebuffer panel's setSleep() is a no-op) */
    void panelSleepIn() { _panel_instance.write_cmd(0x28); _panel_instance.setSleep(true); }
    void panelSleepOut() { _panel_instance.setSleep(false); _panel_instance.write_cmd(0x29); }

    bool enableFrameBuffer(bool auto_display = false)
    {
        if (_panel_instance.initPanelFb()) {
            auto fbPanel = _panel_instance.getPanelFb();
            if (fbPanel) {
                fbPanel->setBus(&_bus_instance);
                fbPanel->setAutoDisplay(auto_display);
                setPanel(fbPanel);
                return true;
            }
        }
        return false;
    }

    void disableFrameBuffer()
    {
        auto fbPanel = _panel_instance.getPanelFb();
        if (fbPanel) {
            _panel_instance.deinitPanelFb();
            setPanel(&_panel_instance);
        }
    }

    void setBrightness(uint8_t brightness)
    {
        _panel_instance.setBrightness(brightness);
    }
};

static std::unique_ptr<M5StopWatch> _display;
static std::unique_ptr<LGFX_Sprite> _canvas;

void Hal::display_init()
{
    mclog::tagInfo(_tag, "display init");

    _display = std::make_unique<M5StopWatch>();
    if (!_display->init()) {
        mclog::tagError(_tag, "display init failed");
        _display.reset();
    }

    // mclog::tagInfo(_tag, "create full screen canvas");
    // _canvas = std::make_unique<LGFX_Sprite>(_display.get());
    // _canvas->setPsram(true);
    // if (!_canvas->createSprite(_display->width(), _display->height())) {
    //     mclog::tagError(_tag, "canvas init failed");
    //     _canvas.reset();
    // }

    // Load brightness from settings
    auto brightness = getBackLightBrightness(true);
    setBackLightBrightness(brightness, false);
}

LGFX_Device &Hal::getDisplay()
{
    return *_display;
}

LGFX_Sprite &Hal::getCanvas()
{
    return *_canvas;
}

void Hal::updateCanvas()
{
    _canvas->pushSprite(0, 0);
}

void Hal::setBackLightBrightness(int brightness, bool saveToSettings)
{
    _bl_brightness = uitk::clamp(brightness, 0, 100);
    if (_bl_brightness > 0) _bl_last_nonzero = _bl_brightness;   /* survives a dim-to-zero */

    int set_target = uitk::map_range(_bl_brightness, 0, 100, 0, 255);
    _display->setBrightness(set_target);

    if (saveToSettings) {
        Settings settings(std::string(Hal::SettingsNs), true);
        settings.SetInt("bl_lev", _bl_brightness);
        mclog::tagInfo(_tag, "brightness saved to settings: {}", _bl_brightness);
    }
}

/* -------------------------------------------------------------------------- */
/*                            Unified screen power                            */
/* -------------------------------------------------------------------------- */

static void lvgl_tick_set_enabled(bool on);   /* defined below */
static lv_indev_t* lvTouchpad = nullptr;      /* file scope: the idle task polls it */

namespace {
volatile uint32_t g_last_activity_ms = 0;
volatile bool g_screen_off = false;
constexpr uint32_t SCREEN_IDLE_MS = 5000;

void screen_sleep_now(const char* reason)
{
    if (g_screen_off) return;
    GetHAL().lvglLock();          /* serialise with the LVGL render task */
    g_screen_off = true;
    /* real panel sleep: display off + sleep in (framebuffer panel's own
       setSleep() is a no-op, so drive the base panel command directly) */
    _display->setBrightness(0);
    _display->panelSleepIn();
    /* panel is asleep and its bus is idle: now the chip may sleep, but only
       on battery - a USB host keeps the console (and our ability to flash)
       alive, and light sleep would take that away. */
    lvgl_tick_set_enabled(false);        /* no 10 ms wake-ups while dark */
    GetHAL().powerIdle("display");       /* nothing needs the panel now */
    mclog::tagInfo("HAL-Display", "screen off ({})", reason);
    GetHAL().lvglUnlock();
}

void screen_wake_now(int brightness)
{
    if (!g_screen_off) return;
    /* claim the buses before touching the panel again */
    GetHAL().powerBusy("display");
    lvgl_tick_set_enabled(true);
    GetHAL().lvglLock();
    g_screen_off = false;
    _display->panelSleepOut();            /* 0x11 SLPOUT + 150 ms, 0x29 DISPON */
    _display->setBrightness(uitk::map_range(uitk::clamp(brightness, 0, 100), 0, 100, 0, 255));
    lv_obj_invalidate(lv_screen_active());
    mclog::tagInfo("HAL-Display", "screen on");
    GetHAL().lvglUnlock();
}
}  // namespace

void Hal::markActivity()
{
    g_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Runs in the main loop (same context as app rendering): performs the actual
   panel sleep/wake I/O so it can never race another SPI writer. */
void Hal::screenTick()
{
    if (g_last_activity_ms == 0) return;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t idle = now - g_last_activity_ms;

    if (g_screen_off) {
        if (idle < SCREEN_IDLE_MS) {
            screen_wake_now(_bl_last_nonzero);
        }
    } else if (idle > SCREEN_IDLE_MS) {
        screen_sleep_now("idle");
    }
}

bool Hal::screen_off_active()
{
    return g_screen_off;
}

void Hal::screenWake()
{
    markActivity();   /* the main loop's screenTick() performs the wake */
}

int Hal::getBackLightBrightness(bool loadFromSettings)
{
    if (loadFromSettings) {
        Settings settings(std::string(Hal::SettingsNs), false);
        _bl_brightness = settings.GetInt("bl_lev", 80);
        _bl_brightness = uitk::clamp(_bl_brightness, 10, 100);
        mclog::tagInfo(_tag, "brightness loaded from settings: {}", _bl_brightness);
    }
    return _bl_brightness;
}

/* -------------------------------------------------------------------------- */
/*                                  Touchpad                                  */
/* -------------------------------------------------------------------------- */
#include "drivers/cst820/cst820.h"

static std::unique_ptr<Cst820> _cst820;

void Hal::touchpad_init()
{
    mclog::tagInfo(_tag, "touchpad init");

    ioe_tp_reset();

    _cst820 = std::make_unique<Cst820>();
    if (!_cst820->begin(i2c_bus_get_internal_bus_handle(_i2c_bus))) {
        mclog::tagError(_tag, "touchpad init failed");
        _cst820.reset();
    }
}

Hal::TouchPoint Hal::getTouchPoint()
{
    Hal::TouchPoint point;
    if (_cst820 && _cst820->read()) {
        point.num = _cst820->getFingerNum();
        if (point.num > 0) {
            point.x = _cst820->getX();
            point.y = _cst820->getY();
        }
    }
    return point;
}

/* -------------------------------------------------------------------------- */
/*                                    Lvgl                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/m5stack/lv_m5_emulator/blob/main/src/utility/lvgl_port_m5stack.cpp
#include <cstdlib>  // for aligned_alloc
#include <cstring>  // for memset
#include <lvgl.h>
#include <atomic>

static SemaphoreHandle_t xGuiSemaphore;
static std::atomic<bool> _lvgl_update_enabled = false;

#define LV_BUFFER_LINE 120

static esp_timer_handle_t s_lvgl_tick_timer = nullptr;

static void lvgl_tick_timer(void *arg)
{
    (void)arg;
    lv_tick_inc(10);
}

/* The 10 ms tick wakes the CPU 100 times per second - pointless while the
   panel is off, so stop it with the screen. */
static void lvgl_tick_set_enabled(bool on)
{
    if (s_lvgl_tick_timer == nullptr) return;
    if (on) esp_timer_start_periodic(s_lvgl_tick_timer, 10 * 1000);
    else    esp_timer_stop(s_lvgl_tick_timer);
}

static void lvgl_rtos_task(void *pvParameter)
{
    (void)pvParameter;
    while (1) {
        /* while the panel sleeps, do not render: saves the biggest chunk */
        if (_lvgl_update_enabled && pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            if (g_screen_off) {
                /* no rendering while dark, but the touch panel still has to be
                   polled or a tap can never wake the watch */
                lv_indev_read(lvTouchpad);
            } else {
                lv_timer_handler();
            }
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(g_screen_off ? 60 : 10));
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    M5GFX &gfx = *(M5GFX *)lv_display_get_driver_data(disp);

    uint32_t w      = (area->x2 - area->x1 + 1);
    uint32_t h      = (area->y2 - area->y1 + 1);
    uint32_t pixels = w * h;

    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);

    // Critical fix: Use safe pixel writing method to avoid M5GFX SIMD optimizations
    // Break large transfers into small chunks to avoid problematic copy_rgb_fast function
    const uint32_t SAFE_CHUNK_SIZE = 8192;  // 8K pixels per chunk, suitable for small buffer settings

    if (pixels > SAFE_CHUNK_SIZE) {
        // Chunked transmission for large data
        const lgfx::rgb565_t *src = (const lgfx::rgb565_t *)px_map;
        uint32_t remaining        = pixels;
        uint32_t offset           = 0;

        while (remaining > 0) {
            uint32_t chunk_size = (remaining > SAFE_CHUNK_SIZE) ? SAFE_CHUNK_SIZE : remaining;
            gfx.writePixels(src + offset, chunk_size);
            offset += chunk_size;
            remaining -= chunk_size;
        }
    } else {
        // Direct transmission for small data
        gfx.writePixels((lgfx::rgb565_t *)px_map, pixels);
    }

    gfx.endWrite();

    lv_display_flush_ready(disp);
}

static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    M5GFX &gfx = *(M5GFX *)lv_indev_get_driver_data(indev);

    auto tp = GetHAL().getTouchPoint();
    if (tp.num > 0) {
        GetHAL().markActivity();
        if (GetHAL().screen_off_active()) {
            GetHAL().screenWake();
            data->state = LV_INDEV_STATE_REL;   /* first tap only wakes */
            return;
        }
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = tp.x;
        data->point.y = tp.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void Hal::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init");
    g_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    lv_init();

    static lv_display_t *disp = lv_display_create(_display->width(), _display->height());
    if (disp == NULL) {
        printf("lv_display_create failed\n");
        return;
    }

    lv_display_set_driver_data(disp, _display.get());
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    static uint8_t *buf1 = (uint8_t *)heap_caps_malloc(_display->width() * LV_BUFFER_LINE, MALLOC_CAP_SPIRAM);
    static uint8_t *buf2 = (uint8_t *)heap_caps_malloc(_display->width() * LV_BUFFER_LINE, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, (void *)buf1, (void *)buf2, _display->width() * LV_BUFFER_LINE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lvTouchpad = lv_indev_create();
    LV_ASSERT_MALLOC(lvTouchpad);
    if (lvTouchpad == NULL) {
        printf("lv_indev_create failed\n");
        return;
    }
    lv_indev_set_driver_data(lvTouchpad, _display.get());
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    lv_indev_set_display(lvTouchpad, disp);

    xGuiSemaphore                                     = xSemaphoreCreateMutex();
    const esp_timer_create_args_t periodic_timer_args = {.callback = &lvgl_tick_timer, .name = "lvgl_tick_timer"};
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &s_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_tick_timer, 10 * 1000));
    xTaskCreate(lvgl_rtos_task, "lvgl_rtos_task", 4096 * 4, NULL, 1, NULL);

    startLvglUpdate();

    {
        LvglLockGuard lock;
        uitk::lvgl_cpp::ScreenActive screen;
        screen.setBgColor(lv_color_black());
        GetHAL().bootLogo = std::make_unique<BootLogo>();
    }
}

bool Hal::lvglLock()
{
    return xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE ? true : false;
}

void Hal::lvglUnlock()
{
    xSemaphoreGive(xGuiSemaphore);
}

void Hal::startLvglUpdate()
{
    _lvgl_update_enabled = true;
}

void Hal::stopLvglUpdate()
{
    _lvgl_update_enabled = false;
}
