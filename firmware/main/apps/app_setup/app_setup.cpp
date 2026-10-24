/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_setup.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <i18n/i18n.h>

using namespace mooncake;
using namespace view;
using namespace setup_workers;

/* One line describing both slots, e.g. "A: 3.0MB run  B: empty" */
static std::string slot_status_text()
{
    Hal::OtaSlot slots[2] = {};
    const int n = GetHAL().otaSlots(slots, 2);
    if (n == 0) return "No slots found";
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (i) s += "   ";
        s += slots[i].name;
        s += slots[i].running ? ": running"
                              : (slots[i].valid ? ": backup ok" : ": empty");
    }
    return s;
}

AppSetup::AppSetup()
{
    setAppInfo().name = "Settings";
    setAppInfo().icon = (void*)&icon_setup;
}


void AppSetup::buildMenuSections(bool create_page)
{
    _menu_sections = {
        {
            tr("device"),
            {
                {tr("brightness"),
                 [&]() {
                     _destroy_menu = true;
                     _worker       = std::make_unique<BrightnessWorker>();
                 }},
                {tr("volume"),
                 [&]() {
                     _destroy_menu = true;
                     _worker       = std::make_unique<VolumeWorker>();
                 }},
                {tr("button"),
                 [&]() {
                     _destroy_menu = true;
                     _worker       = std::make_unique<ButtonWorker>();
                 }},
                {fmt::format("{}: {}", tr("language"), i18nLangName(i18nLang())),
                 [&]() {
                     /* tap to cycle through the available languages */
                     const int next = ((int)i18nLang() + 1) % (int)Lang::COUNT;
                     i18nSetLang((Lang)next);
                     _need_rebuild = true;      /* rebuild in onRunning, not inside the event */
                 }},
            },
        },
        {
            tr("time_date"),
            {
                {tr("set_time"),
                 [&]() {
                     _destroy_menu = true;
                     _worker       = std::make_unique<SetTimeWorker>();
                 }},
                {tr("set_date"),
                 [&]() {
                     _destroy_menu = true;
                     _worker       = std::make_unique<SetDateWorker>();
                 }},
            },
        },
        {
            tr("system_slots"),
            {
                {slot_status_text(),
                 [&]() { /* informational only */ }},
                {tr("switch_slot"),
                 [&]() {
                     _destroy_menu = true;
                     _worker = std::make_unique<SlotActionWorker>(SlotActionWorker::Action::Switch);
                 }},
                {tr("clone_slot"),
                 [&]() {
                     _destroy_menu = true;
                     /* Never overwrite the only good backup: cloning is only
                        allowed into an empty or broken slot. */
                     if (GetHAL().otaOtherSlotValid()) {
                         _worker = std::make_unique<SlotActionWorker>(SlotActionWorker::Action::Blocked);
                     } else {
                         _worker = std::make_unique<SlotActionWorker>(SlotActionWorker::Action::Clone);
                     }
                 }},
            },
        },
        {
            tr("firmware"),
            {
                {fmt::format("{}: {}", tr("version"), common::FirmwareVersion),
                 [&]() {
                     _magic_count++;
                     if (_magic_count >= 10) {
                         _magic_count  = 0;
                         _destroy_menu = true;
                         _worker       = std::make_unique<AboutWorker>();
                     }
                 }},
            },
        },
    };

    if (create_page) {
        _menu_page = std::make_unique<view::SelectMenuPage>(_menu_sections);
    }
}

void AppSetup::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
    // open();
}

void AppSetup::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    // Reset state
    _destroy_menu    = false;
    _need_warm_reset = false;
    _magic_count     = 0;

    buildMenuSections(true);

    LvglLockGuard lock;
}

void AppSetup::onRunning()
{
    if (_key_manager && _key_manager->update() == input::KeyEvent::GoHome) {
        close();
        return;
    }

    LvglLockGuard lock;

    if (_menu_page) {
        _menu_page->update();
    }

    if (_destroy_menu) {
        _menu_page.reset();
        _destroy_menu = false;
    }

    if (_need_rebuild) {
        _need_rebuild = false;
        buildMenuSections(false);          /* new strings for the new language */
        _menu_page = std::make_unique<view::SelectMenuPage>(_menu_sections);
    }

    if (_worker) {
        _worker->update();
        if (_worker->isDone()) {
            _worker.reset();
            _menu_page = std::make_unique<view::SelectMenuPage>(_menu_sections);
        }
    }
}

void AppSetup::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();

    LvglLockGuard lock;

    _menu_sections.clear();
    _menu_page.reset();
    _worker.reset();

    if (_need_warm_reset) {
        // GetHAL().requestWarmReboot(6);
    }
}
