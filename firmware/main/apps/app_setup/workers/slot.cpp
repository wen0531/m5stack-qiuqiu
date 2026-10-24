// System-slot actions: switch to the other firmware slot, or clone the
// running one into it.
//
// The flash work runs on its own task: doing it in the LVGL thread would
// block the UI for ~10 s and could even starve the task watchdog.
#include "workers.h"
#include <mooncake_log.h>
#include <i18n/i18n.h>
#include <esp_system.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace setup_workers;

static const std::string_view _tag = "Setup-Slot";

namespace {
struct Job {
    SlotActionWorker::Action action;
    volatile bool done;
    esp_err_t err;
};
Job g_job;

void slot_job_task(void*)
{
    switch (g_job.action) {
    case SlotActionWorker::Action::Blocked:
        g_job.err = ESP_ERR_INVALID_STATE;
        g_job.done = true;
        vTaskDelete(nullptr);
        return;
    case SlotActionWorker::Action::Switch: {
        esp_err_t err = GetHAL().otaSwitchToOther();
        g_job.err = err;
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(300));     /* let the label paint */
            esp_restart();
        }
        break;
    }
    case SlotActionWorker::Action::Clone:
        g_job.err = GetHAL().otaCloneRunningToOther();
        break;
    }
    g_job.done = true;
    vTaskDelete(nullptr);
}
}  // namespace

class SlotActionWorker::SlotView {
public:
    explicit SlotView(const std::string& text)
    {
        _panel = std::make_unique<Container>(lv_screen_active());
        _panel->setSize(466, 466);
        _panel->setRadius(0);
        _panel->setBorderWidth(0);
        _panel->setBgColor(lv_color_black());
        _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

        _label = std::make_unique<Label>(_panel->get());
        _label->setTextColor(lv_color_hex(0xFFFFFF));
        _label->setTextFont(i18nFont());
        _label->setTextAlign(LV_TEXT_ALIGN_CENTER);
        _label->setWidth(400);
        _label->align(LV_ALIGN_CENTER, 0, 0);
        _label->setText(text.c_str());
    }

    void set_text(const std::string& t)
    {
        if (_label) _label->setText(t.c_str());
    }

private:
    std::unique_ptr<Container> _panel;
    std::unique_ptr<Label> _label;
};

SlotActionWorker::SlotActionWorker(Action action) : _action(action)
{
    const char* text = tr("cloning");
    if (action == Action::Switch)  text = tr("switching");
    if (action == Action::Blocked) text = tr("blocked");
    _view = std::make_unique<SlotView>(text);
}

SlotActionWorker::~SlotActionWorker() = default;

void SlotActionWorker::update()
{
    if (_done) {
        if (lv_tick_get() - _done_at > 2500) _is_done = true;
        return;
    }

    if (!_started) {
        _started      = true;
        g_job.action  = _action;
        g_job.done    = false;
        g_job.err     = ESP_OK;
        xTaskCreate(slot_job_task, "slot_job", 8192, nullptr, 3, nullptr);
        return;
    }

    if (g_job.done) {
        _done   = true;
        _result = (g_job.err == ESP_OK)
                      ? (_action == Action::Clone ? tr("cloned") : tr("switched"))
                      : std::string(tr("failed")) + ": " + esp_err_to_name(g_job.err);
        _view->set_text(_result);
        _done_at = lv_tick_get();
        mclog::tagInfo(_tag, "%s", _result.c_str());
    }
}
