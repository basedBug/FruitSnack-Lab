// Fixed-size telemetry ring buffer for the temperature graph.
#include "telemetry.h"

#include <new>

Telemetry::~Telemetry() {
  delete[] buf_;
  buf_ = nullptr;
}

void Telemetry::begin(size_t capacity) {
  if (capacity == 0) capacity = 900;
  if (buf_ != nullptr && cap_ == capacity) {
    clear();
    return;
  }

  delete[] buf_;
  buf_ = new (std::nothrow) TelemetrySample[capacity];
  cap_ = (buf_ != nullptr) ? capacity : 0;
  head_ = 0;
  count_ = 0;
}

void Telemetry::push(uint32_t tMs, float tempC, float setpointC,
                     float outputPct, float duty) {
  if (buf_ == nullptr || cap_ == 0) return;

  TelemetrySample& s = buf_[head_];
  s.tMs = tMs;
  s.temperatureC = tempC;
  s.setpointC = setpointC;
  s.outputPct = outputPct;
  s.duty = duty;

  head_ = (head_ + 1) % cap_;
  if (count_ < cap_) ++count_;
}

size_t Telemetry::copyRecent(TelemetrySample* out, size_t maxSamples) const {
  if (buf_ == nullptr || cap_ == 0 || out == nullptr || maxSamples == 0) {
    return 0;
  }
  const size_t n = (count_ < maxSamples) ? count_ : maxSamples;
  if (n == 0) return 0;

  // Oldest of the last n samples.
  size_t idx = (head_ + cap_ - n) % cap_;
  for (size_t i = 0; i < n; ++i) {
    out[i] = buf_[idx];
    idx = (idx + 1) % cap_;
  }
  return n;
}

void Telemetry::clear() {
  head_ = 0;
  count_ = 0;
}
