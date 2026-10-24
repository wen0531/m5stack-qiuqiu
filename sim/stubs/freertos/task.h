#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
inline void vTaskDelete(TaskHandle_t) {}
inline int xTaskCreate(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*) { return 1; }
inline void vTaskDelayUntil(TickType_t*, TickType_t) {}
