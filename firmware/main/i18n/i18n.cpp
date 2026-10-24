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
{"settings",      {"Settings","设置","設定","設定","설정","Réglages","Einstellungen","Ajustes","Настройки","Optiones"}},
{"device",        {"Device","设备","裝置","デバイス","장치","Appareil","Gerät","Dispositivo","Устройство","Machina"}},
{"brightness",    {"Brightness","亮度","亮度","明るさ","밝기","Luminosité","Helligkeit","Brillo","Яркость","Luminositas"}},
{"volume",        {"Volume","音量","音量","音量","볼륨","Volume","Lautstärke","Volumen","Громкость","Sonus"}},
{"button",        {"Button","按键","按鍵","ボタン","버튼","Bouton","Taste","Botón","Кнопка","Bulla"}},
{"time_date",     {"Time & Date","时间与日期","時間與日期","時刻と日付","시간 및 날짜","Heure et date","Zeit & Datum","Hora y fecha","Время и дата","Tempus et dies"}},
{"set_time",      {"Set Time","设置时间","設定時間","時刻を設定","시간 설정","Régler l'heure","Zeit einstellen","Ajustar hora","Установить время","Pone tempus"}},
{"set_date",      {"Set Date","设置日期","設定日期","日付を設定","날짜 설정","Régler la date","Datum einstellen","Ajustar fecha","Установить дату","Pone diem"}},
{"language",      {"Language","语言","語言","言語","언어","Langue","Sprache","Idioma","Язык","Lingua"}},
{"system_slots",  {"System Slots","系统槽位","系統槽位","システムスロット","시스템 슬롯","Emplacements","Systemslots","Ranuras","Слоты системы","Loculi systematis"}},
{"switch_slot",   {"Switch to other slot","切换到另一系统","切換到另一系統","別スロットへ切替","다른 슬롯으로 전환","Basculer","Anderen Slot starten","Cambiar de ranura","Переключить слот","Muta loculum"}},
{"clone_slot",    {"Clone current to other","备份当前到另一槽","備份目前到另一槽","現在の領域を複製","현재 슬롯 복제","Cloner","Aktuellen klonen","Clonar actual","Клонировать","Copia loculum"}},
{"firmware",      {"Firmware","固件","韌體","ファームウェア","펌웨어","Micrologiciel","Firmware","Firmware","Прошивка","Firmware"}},
{"version",       {"Version","版本","版本","バージョン","버전","Version","Version","Versión","Версия","Versio"}},
{"slot_running",  {"running","运行中","執行中","実行中","실행 중","actif","läuft","activo","работает","currit"}},
{"slot_ready",    {"backup ok","备份可用","備份可用","予備あり","백업 양호","secours ok","Sicherung ok","respaldo ok","резерв ок","copia parata"}},
{"slot_empty",    {"empty","空","空","空き","비어 있음","vide","leer","vacío","пусто","vacua"}},
{"no_slots",      {"No slots found","未找到槽位","找不到槽位","スロットなし","슬롯 없음","Aucun emplacement","Keine Slots","Sin ranuras","Слотов нет","Nulli loculi"}},
{"switching",     {"Switching slot...","正在切换系统…","正在切換系統…","切替中…","전환 중…","Basculement…","Wechsle Slot…","Cambiando…","Переключение…","Mutans…"}},
{"cloning",       {"Cloning firmware...","正在备份固件…","正在備份韌體…","複製中…","복제 중…","Clonage…","Kopiere…","Clonando…","Копирование…","Copians…"}},
{"cloned",        {"Cloned. Switch to use it.","备份完成，切换即可使用","備份完成，切換即可使用","複製完了","복제 완료","Cloné","Kopiert","Clonado","Скопировано","Copiatum"}},
{"blocked",       {"Other slot already has a backup","另一槽已有备份","另一槽已有備份","別スロットに予備あり","다른 슬롯에 백업 있음","Autre emplacement occupé","Anderer Slot belegt","Otra ranura ocupada","Другой слот занят","Alius loculus plenus"}},
{"failed",        {"Failed","失败","失敗","失敗","실패","Échec","Fehlgeschlagen","Falló","Ошибка","Defecit"}},
};

constexpr int kCount = sizeof(kStrings) / sizeof(kStrings[0]);

const lv_font_t* kFonts[(int)Lang::COUNT] = {
    &lv_font_montserrat_28,   /* EN uses the built-in font */
    &i18n_zh_Hans, &i18n_zh_Hant, &i18n_ja, &i18n_ko,
    &i18n_fr, &i18n_de, &i18n_es, &i18n_ru, &i18n_la,
};

Lang g_lang = Lang::EN;
}  // namespace

void i18nInit()
{
    Settings settings(std::string(kSettingsNs), false);
    int saved = settings.GetInt("lang", 0);
    if (saved < 0 || saved >= (int)Lang::COUNT) saved = 0;
    g_lang = (Lang)saved;
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
    settings.SetInt("lang", (int)lang);
}

const char* i18nLangName(Lang lang)
{
    static const char* names[(int)Lang::COUNT] = {
        "English", "简体中文", "繁體中文", "日本語", "한국어",
        "Français", "Deutsch", "Español", "Русский", "Latina",
    };
    if (lang < Lang::EN || lang >= Lang::COUNT) return "?";
    return names[(int)lang];
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

const lv_font_t* i18nFont()
{
    return kFonts[(int)g_lang];
}

const lv_font_t* i18nFontFor(Lang lang)
{
    if (lang < Lang::EN || lang >= Lang::COUNT) return kFonts[0];
    return kFonts[(int)lang];
}
