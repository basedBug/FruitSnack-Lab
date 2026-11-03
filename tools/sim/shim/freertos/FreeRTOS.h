// Host-side FreeRTOS shim for the FruitSnack control-core simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
#pragma once

#include <stdint.h>
#include <stdlib.h>

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffUL
#define configSTACK_DEPTH_TYPE uint32_t
#define tskNO_AFFINITY 0x7FFFFFFF

inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
inline void vTaskDelete(TaskHandle_t task) { (void)task; }
inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc) { (void)prev; (void)inc; }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 0; }

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                         void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                         BaseType_t core) {
  (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)core;
  if (handle) *handle = nullptr;
  return pdPASS;
}

inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                              UBaseType_t prio, TaskHandle_t *handle) {
  return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, tskNO_AFFINITY);
}

inline size_t xPortGetFreeHeapSize() { return 210000; }
inline size_t xPortGetMinimumEverFreeHeapSize() { return 198000; }
