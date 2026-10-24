#include "i18n.h"
#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <mooncake_log.h>
#include <cstring>

#include <assets/fonts/i18n_fonts.h>

namespace {
const char* TAG = "i18n";
constexpr const char* kSettingsNs = "system";   /* same namespace as Hal::SettingsNs */

struct Entry {
    const char* key;
    const char* text[(int)Lang::COUNT];
};

/* Keep in sync with tools/i18n/strings.json */
const Entry kStrings[] = {
#include "i18n_strings.gen.inc"
};

constexpr int kCount = sizeof(kStrings) / sizeof(kStrings[0]);

const lv_font_t* kFonts[(int)Lang::COUNT] = {
#include "i18n_fonts.gen.inc"
};

const char* const kCodes[(int)Lang::COUNT] = {
#include "i18n_codes.gen.inc"
};

Lang g_lang = Lang::EN;
}  // namespace

void i18nInit()
{
    Settings settings(std::string(kSettingsNs), false);
    /* preferred: a stable language code, so reordering the list cannot
       silently select a different language after an upgrade */
    std::string code = settings.GetString("lang_code", "");
    g_lang = Lang::EN;
    if (!code.empty()) {
        for (int i = 0; i < (int)Lang::COUNT; ++i) {
            if (code == kCodes[i]) { g_lang = (Lang)i; break; }
        }
    } else {
        /* migration: older builds stored an integer index */
        int saved = settings.GetInt("lang", 0);
        if (saved >= 0 && saved < (int)Lang::COUNT) g_lang = (Lang)saved;
    }
    mclog::tagInfo(TAG, "language = {}", (int)g_lang);
}

Lang i18nLang()
{
    return g_lang;
}

void i18nSetLang(Lang lang)
{
    if (lang < Lang::EN || lang >= Lang::COUNT) return;
    if (lang == g_lang) return;              /* nothing to do, no NVS write */
    g_lang = lang;
    Settings settings(std::string(kSettingsNs), true);
    settings.SetString("lang_code", kCodes[(int)lang]);
    settings.SetInt("lang", (int)lang);      /* keep the legacy key in sync */
}

const char* i18nLangName(Lang lang)
{
    static const char* const names[(int)Lang::COUNT] = {
#include "i18n_names.gen.inc"
    };
    if (lang < Lang::EN || lang >= Lang::COUNT) return "?";
    return names[(int)lang];
}

const char* i18nCode(Lang lang)
{
    if (lang < Lang::EN || lang >= Lang::COUNT) return "en";
    return kCodes[(int)lang];
}

const char* tr(const char* key)
{
    for (int i = 0; i < kCount; ++i) {
        if (std::strcmp(kStrings[i].key, key) == 0) {
            return kStrings[i].text[(int)g_lang];
        }
    }
    return key;   /* untranslated keys fall back to the key itself */
}

const char* i18nAppName(const char* english_name)
{
    /* mooncake keeps the app name in English (logs, control API); only the
       launcher label is localised */
    static const struct { const char* name; const char* key; } map[] = {
        {"AlarmClock", "app_alarm"},
        {"WatchFace",  "app_watchface"},
        {"Stopwatch",  "app_stopwatch"},
        {"Qiuqiu",     "app_qiuqiu"},
        {"Settings",   "app_settings"},
    };
    for (const auto& m : map) {
        if (std::strcmp(m.name, english_name) == 0) return tr(m.key);
    }
    return english_name;
}

const lv_font_t* i18nFont()
{
    return kFonts[(int)g_lang];
}

const lv_font_t* i18nFontFor(Lang lang)
{
    if (lang < Lang::EN || lang >= Lang::COUNT) return kFonts[0];
    return kFonts[(int)lang];
}
