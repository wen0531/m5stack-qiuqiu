// Host stub for the firmware logger.
#pragma once
#include <cstdio>
#include <cstdarg>
#include "../src/sim_log.h"
#include <string>
#include <fmt/format.h>
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
    std::string line = "[" + std::string(tag_v) + "] ";
    try { line += fmt::format(fmt::runtime(fmt), args...); } catch (...) { line += fmt; }
    sim_log_add(line.c_str());
    std::printf("%s\n", line.c_str());
}
template <typename... Args>
inline void tagWarn(std::string_view tag_v, const char* fmt, Args... args)
{
    std::string line = "[" + std::string(tag_v) + "][warn] ";
    try { line += fmt::format(fmt::runtime(fmt), args...); } catch (...) { line += fmt; }
    sim_log_add(line.c_str());
    std::printf("%s\n", line.c_str());
}
template <typename... Args>
inline void tagError(std::string_view tag_v, const char* fmt, Args... args)
{
    std::string line = "[" + std::string(tag_v) + "][err] ";
    try { line += fmt::format(fmt::runtime(fmt), args...); } catch (...) { line += fmt; }
    sim_log_add(line.c_str());
    std::printf("%s\n", line.c_str());
}
}  // namespace mclog
