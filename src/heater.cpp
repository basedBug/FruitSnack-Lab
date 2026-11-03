// LEDC PWM heater output for the FruitSnack hotend.
//
// ledcWrite() takes raw counts in resolution units, not 0..255, so the percent
// to count conversion is done here and the clamp lives in exactly one place.
#include "heater.h"

#include <math.h>

void Heater::begin(uint8_t pin, uint32_t freqHz, uint8_t resolutionBits) {
  pin_ = pin;
  freq_hz_ = freqHz;
  bits_ = resolutionBits > 20 ? 20 : resolutionBits;  // LEDC supports <= 20
  max_count_ = bits_ == 0 ? 0u : ((1u << bits_) - 1u);

  count_ = 0;
  percent_ = 0.0f;
  ready_ = ledcAttach(pin_, freq_hz_, bits_);
  if (!ready_) {
    // Safety: without a working channel we never energise the heater.
    count_ = 0;
    percent_ = 0.0f;
  } else {
    // Explicitly drive 0 so the pin does not come up with a stale compare.
    ledcWrite(pin_, 0);
  }
}

uint32_t Heater::toCount(float percent) const {
  if (!ready_ || max_count_ == 0) return 0;
  if (percent <= 0.0f) return 0;
  if (percent >= 100.0f) return max_count_;
  const float scaled = percent / 100.0f * (float)max_count_;
  uint32_t count = (uint32_t)lroundf(scaled);
  if (count > max_count_) count = max_count_;
  return count;
}

void Heater::setDutyPercent(float percent) {
  if (!ready_) {
    count_ = 0;
    percent_ = 0.0f;
    return;
  }
  if (!isfinite(percent)) percent = 0.0f;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  count_ = toCount(percent);
  percent_ = percent;
  ledcWrite(pin_, count_);
}

float Heater::dutyPercent() const {
  return ready_ ? percent_ : 0.0f;
}

uint32_t Heater::dutyCount() const { return ready_ ? count_ : 0; }

uint32_t Heater::maxDuty() const { return max_count_; }

void Heater::off() { setDutyPercent(0.0f); }

bool Heater::active() const { return dutyCount() > 0; }

bool Heater::ready() const { return ready_; }
