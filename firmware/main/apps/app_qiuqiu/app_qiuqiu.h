/*
 * Qiuqiu app — 15-expression procedural white-ball avatar.
 */
#pragma once

#include <apps/common/key_manager/key_manager.h>
#include <lvgl.h>
#include <mooncake.h>
#include <memory>

class AppQiuqiu : public mooncake::AppAbility {
public:
    AppQiuqiu();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    void next_expression();

private:
    void mark_input();

    std::unique_ptr<input::KeyManager> _key_manager;
    lv_obj_t* _hit = nullptr;
    uint16_t* _up = nullptr;
    uint16_t* _fb[2] = {nullptr, nullptr};
    int _fb_idx = 0;
    uint32_t _last_render = 0;
    uint32_t _last_input = 0;
    int _expr = 0;
    uint32_t _manual_until = 0;
    int _saved_brightness = 80;
    bool _screen_off = false;
    bool _touch_down = false;
    int _render_calls = 0;
    int _fps_count = 0;
};
