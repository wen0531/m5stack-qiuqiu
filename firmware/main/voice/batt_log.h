#pragma once
#include <cstddef>
#include <cstdint>

/* Battery telemetry: samples VBAT/level once a minute into a ring buffer so the
   real runtime can be derived without a current meter. Dump via control 0x40. */
void batt_log_start();
void batt_log_dump();          // stream CSV over the debug characteristic
