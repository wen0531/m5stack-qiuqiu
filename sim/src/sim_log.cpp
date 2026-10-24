#include "sim_log.h"
#include <deque>
#include <mutex>

namespace {
std::deque<std::string> g_lines;
std::mutex              g_mutex;
constexpr size_t        kMax = 300;
}  // namespace

void sim_log_add(const char* line)
{
    if (!line || !*line) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.emplace_back(line);
    while (g_lines.size() > kMax) g_lines.pop_front();
}

std::string sim_log_tail(int max_lines)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    const int start = g_lines.size() > (size_t)max_lines ? (int)g_lines.size() - max_lines : 0;
    for (int i = start; i < (int)g_lines.size(); ++i) out += g_lines[i] + "\n";
    return out.empty() ? "(no log lines yet)\n" : out;
}
