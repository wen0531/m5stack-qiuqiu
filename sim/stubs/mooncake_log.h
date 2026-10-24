// Host stub for the firmware logger.
#pragma once
#include <cstdio>
#include <string>
#include <string_view>
#include <cstdarg>

namespace mclog {
enum LogLevel_t { level_debug = 0, level_info, level_warn, level_error };
inline void set_level(LogLevel_t) {}
inline void set_time_format(int) {}

template <typename... Args>
inline void tagInfo(std::string_view tag_v, const char* fmt, Args... args)
{
    std::printf("[%.*s] ", (int)tag_v.size(), tag_v.data());
    std::printf(fmt, args...);
    std::printf("\n");
}
template <typename... Args>
inline void tagWarn(std::string_view tag_v, const char* fmt, Args... args)
{
    std::printf("[%.*s][warn] ", (int)tag_v.size(), tag_v.data());
    std::printf(fmt, args...);
    std::printf("\n");
}
template <typename... Args>
inline void tagError(std::string_view tag_v, const char* fmt, Args... args)
{
    std::printf("[%.*s][err] ", (int)tag_v.size(), tag_v.data());
    std::printf(fmt, args...);
    std::printf("\n");
}
}  // namespace mclog
