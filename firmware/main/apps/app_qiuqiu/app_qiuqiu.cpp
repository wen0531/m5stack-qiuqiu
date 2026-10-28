/*
 * Qiuqiu app — 15-expression procedural avatar on the 466x466 AMOLED.
 *
 * Rendering path: the C++ engine draws into a PSRAM RGB565 framebuffer and the
 * finished frame is pushed straight to the M5GFX panel (same setAddrWindow /
 * writePixels pattern as the HAL's LVGL flush, which is known to work on this
 * board). Doing it this way avoids LVGL canvas image blits, which were slow and
 * produced tearing on the hardware.
 *
 * Controls:
 *   A / B short press : previous / next expression (manual mode for 10 s)
 *   A + B hold        : back to launcher
 *   tap screen        : next expression
 *   20 s idle         : screen off; any key / tap wakes it
 */
#include "app_qiuqiu.h"

#include <esp_heap_caps.h>

#include <M5GFX.h>

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include "qiuqiu_engine.h"
#include "voice/voice_state.h"
#include "voice/ble_svc.h"

using namespace mooncake;

static constexpr int RW = 186;   /* engine render size (half viewport) */
static constexpr int RH = 186;
static constexpr int QW = 372;   /* pushed viewport size */
static constexpr int QH = 372;
static constexpr int SCR_W = 466; /* panel size */
static constexpr int SCR_H = 466;
static constexpr int VX = (SCR_W - QW) / 2;
static constexpr int VY = (SCR_H - QH) / 2;
static constexpr float BALL_RADIUS_PX = 168.0f;
static constexpr uint32_t FRAME_MS = 50;      /* ~20 fps while animating */
static constexpr uint32_t FRAME_MS_IDLE = 120; /* ~8 fps once the ball settles */
static constexpr uint32_t SCREEN_OFF_MS = 20000;

AppQiuqiu::AppQiuqiu()
{
    setAppInfo().name = "Qiuqiu";
    setAppInfo().icon = (void*)&icon_qiuqiu;
}

void AppQiuqiu::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppQiuqiu::mark_input()
{
    _last_input = GetHAL().millis();
}

void AppQiuqiu::next_expression()
{
    _expr = (_expr + 1) % qiuqiu::expression_count();
    _manual_until = GetHAL().millis() + 10000;
    mark_input();
    _last_render = 0;  /* redraw immediately */
}

/* Debug: RLE-encode the framebuffer and push it to the PC over BLE.
   Trigger with control write 0x30; the host rebuilds a PNG from it. */
static void dump_fb_ble(const uint16_t* fb)
{
    static uint8_t* buf = nullptr;
    if (buf == nullptr) {
        buf = (uint8_t*)heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
        if (buf == nullptr) return;
    }
    size_t n = 0;
    for (int y = 0; y < QH && n + 8 < 32 * 1024; y++) {
        const uint16_t* row = fb + (size_t)y * QW;
        int x = 0;
        while (x < QW && n + 8 < 32 * 1024) {
            uint16_t v = row[x];
            int run = 1;
            while (x + run < QW && row[x + run] == v && run < 60000) run++;
            buf[n++] = (uint8_t)(run & 0xFF);
            buf[n++] = (uint8_t)(run >> 8);
            buf[n++] = (uint8_t)(v & 0xFF);
            buf[n++] = (uint8_t)(v >> 8);
            x += run;
        }
    }
    ble_svc_debug_send(buf, n);
    /* printf removed: console traffic keeps the CPU awake */
}

static int auto_expression()
{
    switch (voice_state_get()) {
    case VOICE_LISTENING: return 6; /* LISTENING */
    case VOICE_THINKING: return 7;  /* THINKING */
    case VOICE_SPEAKING: return 8;  /* EXCITED: lively while the reply plays */
    case VOICE_HAPPY: return 1;     /* HAPPY */
    case VOICE_SLEEPY: return voice_state_ever_connected() ? 5 : 0; /* dropped -> sleepy, never connected -> idle */
    case VOICE_IDLE:
    default: return 0;              /* NEUTRAL */
    }
}

void AppQiuqiu::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    /* A long-press talks only while the ball is on screen. */
    voice_set_ptt_enabled(true);

    _key_manager = std::make_unique<input::KeyManager>();

    for (int i = 0; i < 2; i++) {
        _fb[i] = (uint16_t*)heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
        if (_fb[i] != nullptr) memset(_fb[i], 0, (size_t)RW * RH * 2);
    }
    _up = (uint16_t*)heap_caps_malloc((size_t)QW * QH * 2, MALLOC_CAP_SPIRAM);
    if (_up) memset(_up, 0, (size_t)QW * QH * 2);
    if (_fb[0] == nullptr || _fb[1] == nullptr || _up == nullptr) {
        mclog::tagError(getAppInfo().name, "framebuffer alloc failed");
        return;
    }
    _fb_idx = 0;
    _touch_down = false;
    _render_calls = 0;
    _fps_count = 0;

    qiuqiu::init(RW, RH, BALL_RADIUS_PX * 0.5f);
    /* Wen 2026-10-21: jelly preset 1 (2.1 Hz / zeta 0.16), eye roundness 3 (45%) */
    qiuqiu::set_tuning(13.194689f, 0.16f, 1.0f, 1.0f);
    qiuqiu::set_eye_roundness(0.45f);

    _expr = 0;
    _manual_until = 0;
    qiuqiu::set_expression(_expr, GetHAL().millis());

    /* Invisible clickable layer: LVGL polls the touch chip at its own rate, so
       taps are not missed even while we are busy pushing a frame. */
    GetHAL().lvglLock();
    _hit = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_hit);
    lv_obj_set_size(_hit, SCR_W, SCR_H);
    lv_obj_set_pos(_hit, 0, 0);
    lv_obj_add_flag(_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_hit, [](lv_event_t* e) {
        auto* self = static_cast<AppQiuqiu*>(lv_event_get_user_data(e));
        if (self) self->next_expression();
    }, LV_EVENT_CLICKED, this);
    GetHAL().lvglUnlock();

    /* clear the whole panel once (the viewport covers only the centre) */
    {
        uint16_t* zero = (uint16_t*)heap_caps_malloc((size_t)SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
        if (zero) {
            memset(zero, 0, (size_t)SCR_W * SCR_H * 2);
            LGFX_Device& gfx = GetHAL().getDisplay();
            GetHAL().lvglLock();
            gfx.startWrite();
            gfx.setAddrWindow(0, 0, SCR_W, SCR_H);
            const lgfx::rgb565_t* zsrc = (const lgfx::rgb565_t*)zero;
            constexpr uint32_t kC = 8192;
            for (uint32_t off = 0; off < (uint32_t)SCR_W * SCR_H; off += kC) {
                uint32_t n = ((uint32_t)SCR_W * SCR_H - off) > kC ? kC : ((uint32_t)SCR_W * SCR_H - off);
                gfx.writePixels(zsrc + off, n);
            }
            gfx.endWrite();
            GetHAL().lvglUnlock();
            heap_caps_free(zero);
        }
    }

    _saved_brightness = GetHAL().getBackLightBrightness();
    if (_saved_brightness < 10) _saved_brightness = 80;
    _screen_off = false;
    mark_input();
    _last_render = 0;
}

void AppQiuqiu::onRunning()
{
    if (_key_manager) {
        auto ev = _key_manager->update();
        if (ev == input::KeyEvent::GoHome) {
            close();
            return;
        }
        if (ev != input::KeyEvent::None) {
            mclog::tagInfo(getAppInfo().name, "DEBUG key={} expr={} engine={} renders={}",
                           (int)ev, _expr, qiuqiu::expression(), _render_calls);
        }
        if (ev == input::KeyEvent::GoPrevious) {
            _expr = (_expr + qiuqiu::expression_count() - 1) % qiuqiu::expression_count();
            _manual_until = GetHAL().millis() + 10000;
            mark_input();
            _last_render = 0;
        } else if (ev == input::KeyEvent::GoNext) {
            next_expression();
            _last_render = 0;
        }
    }

    uint32_t now = GetHAL().millis();
    if (_fb[0] == nullptr || _fb[1] == nullptr) {
        return;
    }

    {
        uint8_t cmd, val;
        while (ble_svc_take_command(&cmd, &val)) {
            if (cmd == 0x30) {
                dump_fb_ble(_fb[_fb_idx]);
                mark_input();
            } else {
                /* not ours: give it back so voice_task can handle it */
                ble_svc_requeue_command(cmd, val);
                break;
            }
        }
    }

    /* auto screen off / wake */
    if (voice_state_get() == VOICE_LISTENING) {
        mark_input();
    }
    /* The HAL owns panel power and brightness; this app only stops rendering
       while the screen is dark. Two owners of the backlight meant a dimmed
       screen could be "restored" to 0 and stay black. */
    bool idle = GetHAL().screen_off_active();
    if (idle && !_screen_off) {
        _screen_off = true;
        mclog::tagInfo(getAppInfo().name, "screen off (idle)");
    } else if (!idle && _screen_off) {
        _screen_off = false;
        mclog::tagInfo(getAppInfo().name, "screen on (wake)");
    }
    if (_screen_off) {
        return;
    }

    /* the ball only needs full frame rate while it is actually moving */
    const bool active_voice = (voice_state_get() == VOICE_LISTENING) || (voice_state_get() == VOICE_SPEAKING) ||
                              (voice_state_get() == VOICE_THINKING);
    const uint32_t frame_ms = active_voice ? FRAME_MS : FRAME_MS_IDLE;
    if ((now - _last_render) < frame_ms) {
        return;
    }

    int want = (now < _manual_until) ? _expr : auto_expression();
    static int last_applied = -1;
    qiuqiu::set_expression(want, now);
    if (want != last_applied) {
        last_applied = want;
        mclog::tagInfo(getAppInfo().name, "DEBUG apply expr={} engine={} renders={}", want,
                       qiuqiu::expression(), _render_calls);
    }

    int back = 1 - _fb_idx;
    uint32_t t0 = GetHAL().millis();
    qiuqiu::render(_fb[back], now);
    /* nearest-neighbour 2x upscale into the push buffer */
    for (int y = 0; y < RH; y++) {
        const uint16_t* src = _fb[back] + (size_t)y * RW;
        uint16_t* d0 = _up + (size_t)(2 * y) * QW;
        uint16_t* d1 = d0 + QW;
        for (int x = 0; x < RW; x++) {
            uint16_t c = src[x];
            d0[2 * x] = c;
            d0[2 * x + 1] = c;
            d1[2 * x] = c;
            d1[2 * x + 1] = c;
        }
    }
    uint32_t t1 = GetHAL().millis();
    _render_calls++;
    _fps_count++;

    /* push finished frame straight to the panel */
    LGFX_Device& gfx = GetHAL().getDisplay();
    GetHAL().lvglLock();
    gfx.startWrite();
    gfx.setAddrWindow(VX, VY, QW, QH);
    const lgfx::rgb565_t* src = (const lgfx::rgb565_t*)_up;
    constexpr uint32_t kChunk = 8192;
    const uint32_t total = (uint32_t)QW * QH;
    for (uint32_t off = 0; off < total; off += kChunk) {
        uint32_t n = (total - off) > kChunk ? kChunk : (total - off);
        gfx.writePixels(src + off, n);
    }
    gfx.endWrite();
    GetHAL().lvglUnlock();
    uint32_t t2 = GetHAL().millis();

    static uint32_t dbg_log = 0;
    if (now - dbg_log > 2000) {
        dbg_log = now;
        mclog::tagInfo(getAppInfo().name, "DEBUG fps~{} render={}ms push={}ms", (int)_fps_count,
                       (int)(t1 - t0), (int)(t2 - t1));
        _fps_count = 0;
    }

    _fb_idx = back;
    _last_render = now;
}

void AppQiuqiu::onClose()
{
    voice_set_ptt_enabled(false);
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();

    _screen_off = false;   /* brightness stays with the HAL */

    GetHAL().lvglLock();
    if (_hit) {
        lv_obj_delete(_hit);
        _hit = nullptr;
    }
    GetHAL().lvglUnlock();

    for (int i = 0; i < 2; i++) {
        if (_fb[i]) {
            heap_caps_free(_fb[i]);
            _fb[i] = nullptr;
        }
    }
}
