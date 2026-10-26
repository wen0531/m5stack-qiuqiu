/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <assets/assets.h>
#include <i18n/i18n.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <vector>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static const std::string_view _tag = "Setup-Language";

namespace setup_workers {

class LanguageWorker::LanguageSelectView {
public:
    LanguageSelectView()
    {
        _panel = std::make_unique<Container>(lv_screen_active());
        _panel->align(LV_ALIGN_CENTER, 0, 0);
        _panel->setSize(466, 466);
        _panel->setRadius(0);
        _panel->setBorderWidth(0);
        _panel->setPaddingAll(0);
        _panel->setBgColor(lv_color_hex(0x000000));
        _panel->setBgOpa(LV_OPA_COVER);
        _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

        auto* title = lv_label_create(_panel->get());
        lv_obj_set_style_text_font(title, i18nFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(title, tr("language"));
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

        _list = lv_obj_create(_panel->get());
        lv_obj_set_size(_list, 420, 356);
        lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 82);
        lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_list, 0, 0);
        lv_obj_set_style_pad_all(_list, 0, 0);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(_list, 8, 0);
        lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_ACTIVE);

        for (int i = 0; i < (int)Lang::COUNT; ++i) {
            const Lang lang = (Lang)i;
            lv_obj_t* row  = lv_obj_create(_list);
            lv_obj_set_size(row, 420, 56);
            lv_obj_set_style_radius(row, 14, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(lang == i18nLang() ? 0x2f6b46 : 0x1A1A1A), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(row);
            /* each language is drawn with its own subset font */
            lv_obj_set_style_text_font(label, i18nFontFor(lang), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_label_set_text(label, i18nLangName(lang));
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);

            lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);
            lv_obj_set_user_data(row, (void*)(intptr_t)i);
        }
    }

    bool hasSelection() const
    {
        return _selected >= 0;
    }
    Lang selected() const
    {
        return (Lang)_selected;
    }

private:
    static void on_row_clicked(lv_event_t* e)
    {
        lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
        auto* self    = (LanguageWorker::LanguageSelectView*)lv_event_get_user_data(e);
        self->_selected = (int)(intptr_t)lv_obj_get_user_data(row);
    }

    std::unique_ptr<Container> _panel;
    lv_obj_t* _list   = nullptr;
    int _selected     = -1;
};

LanguageWorker::LanguageWorker()
{
    mclog::tagInfo(_tag, "start");
    _view = std::make_unique<LanguageSelectView>();
}

LanguageWorker::~LanguageWorker() = default;

void LanguageWorker::update()
{
    if (_view && _view->hasSelection()) {
        const Lang chosen = _view->selected();
        if (chosen != i18nLang()) {
            i18nSetLang(chosen);
            mclog::tagInfo(_tag, "language set to {}", i18nCode(chosen));
        }
        _view.reset();
        _is_done = true;
    }
}

}  // namespace setup_workers
