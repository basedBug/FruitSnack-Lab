// Fixed-size telemetry ring buffer for the temperature graph.
//
// The storage is allocated once in begin() and never resized afterwards, so
// the control loop only does a handful of stores per sample. Samples are
// always handed out oldest -> newest, which is the order the graph draws them.
#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "app_state.h"

class Telemetry {
 public:
  Telemetry() = default;
  ~Telemetry();
  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;

  /// Allocate the ring. A capacity of 0 falls back to 900 samples.
  void begin(size_t capacity);

  void push(uint32_t tMs, float tempC, float setpointC, float outputPct,
            float duty);

  /// Copy up to maxSamples, oldest first. Returns the number written.
  size_t copyRecent(TelemetrySample* out, size_t maxSamples) const;

  size_t size() const { return count_; }
  size_t capacity() const { return cap_; }
  void clear();

 private:
  TelemetrySample* buf_ = nullptr;
  size_t cap_ = 0;
  size_t head_ = 0;  ///< next write position
  size_t count_ = 0; ///< number of valid samples (<= cap_)
};
