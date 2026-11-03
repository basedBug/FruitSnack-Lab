// Host-side FreeRTOS semaphore shim for the FruitSnack control-core simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
#pragma once

#include "freertos/FreeRTOS.h"

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)malloc(sizeof(int)); }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return xSemaphoreCreateMutex(); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
  (void)s; (void)t;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  (void)s;
  return pdTRUE;
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) {
  return xSemaphoreTake(s, t);
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }
inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
