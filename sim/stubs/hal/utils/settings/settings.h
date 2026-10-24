// Host stub: in-memory replacement for the NVS-backed Settings class.
#pragma once
#include <string>
#include <string_view>
#include <map>
#include <cstdint>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false) : ns_(ns), rw_(read_write) {}
    std::string GetString(const std::string& key, const std::string& def = "")
    {
        auto it = store().find(ns_ + "/" + key);
        return it == store().end() ? def : it->second;
    }
    void SetString(const std::string& key, const std::string& v)
    {
        if (rw_) store()[ns_ + "/" + key] = v;
    }
    int32_t GetInt(const std::string& key, int32_t def = 0)
    {
        auto s = GetString(key, "");
        return s.empty() ? def : (int32_t)std::stol(s);
    }
    void SetInt(const std::string& key, int32_t v) { SetString(key, std::to_string(v)); }
    bool GetBool(const std::string& key, bool def = false) { return GetInt(key, def ? 1 : 0) != 0; }
    void SetBool(const std::string& key, bool v) { SetInt(key, v ? 1 : 0); }
    void EraseKey(const std::string& key) { store().erase(ns_ + "/" + key); }
    void EraseAll() { store().clear(); }

    static std::map<std::string, std::string>& store()
    {
        static std::map<std::string, std::string> s;
        return s;
    }

private:
    std::string ns_;
    bool rw_;
};
