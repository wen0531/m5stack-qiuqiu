// Ring buffer of recent log lines, exposed through the control interface.
#pragma once
#include <string>
void        sim_log_add(const char* line);
std::string sim_log_tail(int max_lines);
